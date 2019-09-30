#include "XsecCalc.hh"
using json = nlohmann::json;

XsecCalc::XsecCalc(const std::string& json_config, const std::string& cli_filename)
    : num_toys(0)
    , rng_seed(0)
    , num_signals(0)
    , total_signal_bins(0)
    , signal_bins(0)
    , postfit_cov(nullptr)
    , postfit_cor(nullptr)
    , protonfsi_cov(nullptr)
    , toy_thrower(nullptr)
{
    std::cout << TAG << "Reading error propagation options." << std::endl;
    std::fstream f;

    config_file = json_config;
    f.open(json_config, std::ios::in);

    json j;
    f >> j;

    // std::string input_dir = std::string(std::getenv("XSLLHFITTER")) + j["input_dir"].get<std::string>();
    std::string input_dir = std::string(std::getenv("XSLLHFITTER")); //LM

    if(cli_filename.empty())
        input_file = input_dir + j["input_fit_file"].get<std::string>();
    else
        input_file = cli_filename;

    output_file = input_dir + j["output_file"].get<std::string>();

    do_read_data_events = j.value("read_data_events", false);

    extra_hists = j.value("extra_hists", "");
    if(!extra_hists.empty())
        extra_hists = input_dir + extra_hists;

    if(protonfsi_cov != nullptr)
        delete protonfsi_cov;
    protonfsi_name = j.value("proton_fsi_cov", "");
    if(protonfsi_name.empty())
        do_add_protonfsi_cov = false;
    else
    {
        std::cout << TAG << "Add proton FSI contribution to the xsec covariance matrix." << std::endl;
        do_add_protonfsi_cov = true;
        std::string inputname_protonfsicov = input_dir + j["proton_fsi_cov"].get<std::string>();
        TFile* inputfile_protonfsicov = TFile::Open(inputname_protonfsicov.c_str(), "READ");
        protonfsi_cov = (TMatrixDSym*)inputfile_protonfsicov->Get("cov_mat");
    }

    num_toys = j["num_toys"];
    rng_seed = j["rng_seed"];

    do_incompl_chol = j["decomposition"].value("incomplete_chol", false);
    dropout_tol = j["decomposition"].value("drop_tolerance", 1.0E-3);
    do_force_posdef = j["decomposition"].value("do_force_posdef", false);
    force_padd = j["decomposition"].value("force_posdef_val", 1.0E-9);

    std::string sel_json_config = input_dir + j["sel_config"].get<std::string>();
    std::string tru_json_config = input_dir + j["tru_config"].get<std::string>();

    std::cout << TAG << "Input file from fit: " << input_file << std::endl
              << TAG << "Output xsec file: " << output_file << std::endl
              << TAG << "Num. toys: " << num_toys << std::endl
              << TAG << "RNG  seed: " << rng_seed << std::endl
              << TAG << "Selected events config: " << sel_json_config << std::endl
              << TAG << "True events config: " << tru_json_config << std::endl;

    std::cout << TAG << "Initializing fit objects..." << std::endl;
    selected_events = new FitObj(sel_json_config, "selectedEvents", false);
    true_events     = new FitObj(tru_json_config, "trueEvents",     true);
    total_signal_bins = selected_events->GetNumSignalBins();
    signal_bins = total_signal_bins/2;
    is_fit_type_throw = selected_events->GetFitType() == 3 ? true : false;

    selected_events_ratio = new FitObj(sel_json_config, "selectedEvents", false);
    true_events_ratio     = new FitObj(tru_json_config, "trueEvents",     true);

    std::cout << TAG << "Reading post-fit file..." << std::endl;
    TH1::AddDirectory(false);
    ReadFitFile(input_file);

    InitNormalization(j["sig_norm"], input_dir);
    std::cout << TAG << "Finished initialization." << std::endl;
}

XsecCalc::~XsecCalc()
{
    delete toy_thrower;
    delete selected_events;
    delete true_events;
    delete selected_events_ratio;
    delete true_events_ratio;

    delete postfit_cov;
    delete postfit_cor;
    delete protonfsi_cov;
}

void XsecCalc::ReadFitFile(const std::string& file)
{
    if(postfit_cov != nullptr)
        delete postfit_cov;
    if(postfit_cor != nullptr)
        delete postfit_cor;

    postfit_param.clear();
    prefit_param_original.clear();
    prefit_param_decomp.clear();

    std::cout << TAG << "Opening " << file << std::endl;
    input_file = file;

    TFile* postfit_file = TFile::Open(file.c_str(), "READ");
    postfit_cov = (TMatrixDSym*)postfit_file->Get("res_cov_matrix");
    postfit_cor = (TMatrixDSym*)postfit_file->Get("res_cor_matrix");

    TVectorD* postfit_param_root = (TVectorD*)postfit_file->Get("res_vector");
    for(int i = 0; i < postfit_param_root->GetNoElements(); ++i)
        postfit_param.emplace_back((*postfit_param_root)[i]);

    TVectorD* prefit_original_root = (TVectorD*)postfit_file->Get("vec_prefit_original");
    for(int i = 0; i < prefit_original_root->GetNoElements(); ++i)
        prefit_param_original.emplace_back((*prefit_original_root)[i]);

    TVectorD* prefit_decomp_root = (TVectorD*)postfit_file->Get("vec_prefit_decomp");
    for(int i = 0; i < prefit_decomp_root->GetNoElements(); ++i)
        prefit_param_decomp.emplace_back((*prefit_decomp_root)[i]);

    if(is_fit_type_throw)
    {
        TVectorD* prefit_toy_root = (TVectorD*)postfit_file->Get("vec_par_all_iter0");
        for(int i = 0; i < prefit_toy_root->GetNoElements(); ++i)
            prefit_param_toy.emplace_back((*prefit_toy_root)[i]);
    }

    postfit_file->Close();
    use_prefit_cov = false;

    std::cout << TAG << "Successfully read fit file." << std::endl;
    InitToyThrower();
}

void XsecCalc::UsePrefitCov()
{
    if(selected_events == nullptr)
        std::cout << TAG << "FitObj not initialized for prefit covariance." << std::endl;
    else
    {
        use_prefit_cov = true;
        InitToyThrower();
    }
}

void XsecCalc::InitToyThrower()
{
    std::cout << TAG << "Initializing toy-thrower..." << std::endl;
    if(toy_thrower != nullptr)
        delete toy_thrower;

    TMatrixDSym cov_mat;
    if(use_prefit_cov)
    {
        std::cout << TAG << "Using prefit covariance matrix." << std::endl;
        cov_mat.ResizeTo(selected_events->GetNpar(), selected_events->GetNpar());
        cov_mat = selected_events->GetPrefitCov();
    }
    else
    {
        std::cout << TAG << "Using postfit covariance matrix." << std::endl;
        cov_mat.ResizeTo(postfit_cov->GetNrows(), postfit_cov->GetNrows());
        cov_mat = *postfit_cov;
    }

    toy_thrower = new ToyThrower(cov_mat, rng_seed, false, 1E-48);
    if(do_force_posdef)
    {
        if(!toy_thrower->ForcePosDef(force_padd, 1E-48))
        {
            std::cout << ERR << "Covariance matrix could not be made positive definite.\n"
                << "Exiting." << std::endl;
            exit(1);
        }
    }

    if(do_incompl_chol)
    {
        std::cout << TAG << "Performing incomplete Cholesky decomposition." << std::endl;
        toy_thrower->IncompCholDecomp(dropout_tol, true);
    }
    else
    {
        std::cout << TAG << "Performing ROOT Cholesky decomposition." << std::endl;
        toy_thrower->SetupDecomp(1E-48);
    }
}

void XsecCalc::InitNormalization(const nlohmann::json& j, const std::string input_dir)
{

    for(const auto& sig_def : selected_events->GetSignalDef())
    {
        if(sig_def.use_signal == true)
        {
            std::cout << TAG << "Adding normalization parameters for " << sig_def.name << " signal."
                      << std::endl;

            json s;
            try
            {
                s = j.at(sig_def.name);
            }
            catch(json::exception& e)
            {
                std::cout << ERR << "Signal " << sig_def.name
                          << " not found in error propagation config file." << std::endl;
                exit(1);
            }

            SigNorm n;
            n.name = sig_def.name;
            n.detector = sig_def.detector;
            n.flux_file = s["flux_file"];
            n.flux_name = s["flux_hist"];
            n.flux_int = s["flux_int"];
            n.flux_err = s["flux_err"];
            n.use_flux_fit = s["use_flux_fit"];
            n.num_targets_val = s["num_targets_val"];
            n.num_targets_err = s["num_targets_err"];
            n.is_rel_err = s["relative_err"];
            if(n.is_rel_err)
            {
                n.flux_err = n.flux_int * n.flux_err;
                n.num_targets_err = n.num_targets_val * n.num_targets_err;
            }

            BinManager bm(sig_def.binning);
            n.nbins = bm.GetNbins();

            std::cout << TAG << "Num. bins: " << n.nbins << std::endl
                      << TAG << "Flux file: " << n.flux_file << std::endl
                      << TAG << "Flux hist: " << n.flux_name << std::endl
                      << TAG << "Flux integral: " << n.flux_int << std::endl
                      << TAG << "Flux error: " << n.flux_err << std::endl
                      << TAG << "Use flux fit: " << std::boolalpha << n.use_flux_fit << std::endl
                      << TAG << "Num. targets: " << n.num_targets_val << std::endl
                      << TAG << "Num. targets err: " << n.num_targets_err << std::endl
                      << TAG << "Relative err: " << std::boolalpha << n.is_rel_err << std::endl;

            std::string temp_fname = input_dir + n.flux_file;
            TFile* temp_file = TFile::Open(temp_fname.c_str(), "READ");
            temp_file->cd();
            TH1D* temp_hist = (TH1D*)temp_file->Get(n.flux_name.c_str());
            n.flux_hist = *temp_hist;
            temp_file->Close();

            unsigned int nbins = 100;
            temp_hist
                = new TH1D("", "", nbins, n.flux_int - 5 * n.flux_err, n.flux_int + 5 * n.flux_err);
            n.flux_throws = *temp_hist;
            temp_hist = new TH1D("", "", nbins, n.num_targets_val - 5 * n.num_targets_err,
                                 n.num_targets_val + 5 * n.num_targets_err);
            n.target_throws = *temp_hist;
            v_normalization.push_back(n);
        }
    }
    num_signals = v_normalization.size();
}

void XsecCalc::ReweightBestFit()
{
    
    //We need the ratio of the selected to true events for the efficiency
    //But they need to have the same weights applied, so the postfit parameters
    //are applied to the true events. The nominal weights for both the selected
    //and true events could have been used here, and then the selected events
    //would be reweighted again using the postfit parameters.
    selected_events -> ReweightEvents(postfit_param);
    true_events     -> ReweightEvents(postfit_param);

    selected_events_ratio -> ReweightEvents(postfit_param);
    true_events_ratio     -> ReweightEvents(postfit_param);

    //Get the histograms. Currently both selected and true events have the
    //postfit parameters applied.
    auto sel_hists       = selected_events -> GetSignalHist();
    auto tru_hists       = true_events     -> GetSignalHist();

    auto sel_hists_ratio = selected_events_ratio -> GetRatioHist();
    auto tru_hists_ratio = true_events_ratio     -> GetRatioHist();

    //Calculate and apply efficiency using the postfit weighted selected
    //and true events. Apply the rest of the normalizations using the postfit
    //parameters to the selected events. Now we have the postfit cross-section.
    
    ApplyEffRatio(sel_hists_ratio, sel_hists, tru_hists, false); //LM 11.07.2019
    ApplyNormTargetsRatio(sel_hists_ratio, false);

    ApplyEff(sel_hists, tru_hists, false);
    ApplyNorm(sel_hists, postfit_param, false);

    //For comparisons, we need the nominal truth weights for the true events. Or
    //in the case of a toy throw, the nominal weights using the toy parameters.
    //Currently from above the true events have the postfit parameters applied, and
    //we need the nominal or toy parameters. So the true events have to be weighted
    //again with either the nominal or toy parameters.
    
    //Then we apply the normalizations to the true events using either the original
    //or toy flux parameters to have the truth cross-section. This is the only place
    //where the normalization is applied to the true events.
    
    //One operation that is not obvious from looking at this code is that the
    //ReweightEvents function always starts from the original MC weights.
    if(is_fit_type_throw)
    {
        true_events       -> ReweightEvents(prefit_param_toy);
        true_events_ratio -> ReweightEvents(prefit_param_toy);
        tru_hists       = true_events       -> GetSignalHist();
        tru_hists_ratio = true_events_ratio -> GetRatioHist();
        ApplyNorm(tru_hists, prefit_param_toy, false);
        ApplyNormTargetsRatio(tru_hists_ratio, false);
    }
    else
    {
        true_events       -> ReweightNominal();
        true_events_ratio -> ReweightNominal();
        tru_hists       = true_events       -> GetSignalHist();
        tru_hists_ratio = true_events_ratio -> GetRatioHist();
        ApplyNorm(tru_hists, prefit_param_original, false);
        ApplyNormTargetsRatio(tru_hists_ratio, false);
    }

    sel_best_fit = ConcatHist(sel_hists, "sel_best_fit");
    tru_best_fit = ConcatHist(tru_hists, "tru_best_fit");

    signal_best_fit = std::move(sel_hists);
    truth_best_fit  = std::move(tru_hists);

    sel_best_fit_ratio = std::move(sel_hists_ratio);
    tru_best_fit_ratio = std::move(tru_hists_ratio);
}

void XsecCalc::GenerateToys() { GenerateToys(num_toys); }

void XsecCalc::GenerateToys(const int ntoys)
{
    num_toys = ntoys;
    std::cout << TAG << "Throwing " << num_toys << " toys..." << std::endl;

    ProgressBar pbar(60, "#");
    pbar.SetRainbow();
    pbar.SetPrefix(std::string(TAG + "Throwing "));

    for(int i = 0; i < ntoys; ++i)
    {
        if(i % 10 == 0 || i == (ntoys - 1))
            pbar.Print(i, ntoys - 1);

        const unsigned int npar = postfit_param.size();
        std::vector<double> toy(npar, 0.0);
        toy_thrower->Throw(toy);

        std::transform(toy.begin(), toy.end(), postfit_param.begin(), toy.begin(),
                       std::plus<double>());
        //LM Commented out to allow template parameters to go negative
        //   ( boundary set in FitParameters.cc : pars_limlow.push_back(-10.0); )
        // for(int i = 0; i < npar; ++i)
        // {
        //     if(toy[i] < 0.0)
        //         toy[i] = 0.01;
        // }

        selected_events -> ReweightEvents(toy);
        true_events     -> ReweightEvents(toy);

        selected_events_ratio -> ReweightEvents(toy);
        true_events_ratio     -> ReweightEvents(toy);

        auto sel_hists       = selected_events       -> GetSignalHist();
        auto tru_hists       = true_events           -> GetSignalHist();
        auto sel_hists_ratio = selected_events_ratio -> GetRatioHist(); //LM
        auto tru_hists_ratio = true_events_ratio     -> GetRatioHist(); //LM

        ApplyEffRatio(sel_hists_ratio, sel_hists, tru_hists, true); //LM 11.07.2019
        ApplyNormTargetsRatio(sel_hists_ratio, true);

        ApplyEff(sel_hists, tru_hists, true);
        ApplyNorm(sel_hists, toy, true);

        toys_sel_events.emplace_back(ConcatHist(sel_hists, ("sel_signal_toy" + std::to_string(i))));
        toys_tru_events.emplace_back(ConcatHist(tru_hists, ("tru_signal_toy" + std::to_string(i))));

        toys_sel_ratio.emplace_back(sel_hists_ratio); //LM
        toys_tru_ratio.emplace_back(tru_hists_ratio); //LM

        /*
        total_signal_bins = npar;
        std::string temp = "toy" + std::to_string(i);
        TH1D h_toy(temp.c_str(), temp.c_str(), total_signal_bins, 0, total_signal_bins);
        for(int i = 0; i < total_signal_bins; ++i)
            h_toy.SetBinContent(i+1, toy[i]);
        toys_sel_events.emplace_back(h_toy);
        */
    }
}

void XsecCalc::ApplyEff(std::vector<TH1D>& sel_hist, std::vector<TH1D>& tru_hist, bool is_toy)
{
    std::vector<TH1D> eff_hist;
    for(int i = 0; i < num_signals; ++i)
    {
        TH1D eff = sel_hist[i];
        eff.Divide(&sel_hist[i], &tru_hist[i]);
        eff_hist.emplace_back(eff);

        for(int j = 1; j <= sel_hist[i].GetNbinsX(); ++j)
        {
            double bin_eff = eff.GetBinContent(j);
            double bin_val = sel_hist[i].GetBinContent(j);
            sel_hist[i].SetBinContent(j, bin_val / bin_eff);
        }
    }

    if(is_toy)
    {
        std::string eff_name = "eff_combined_toy" + std::to_string(toys_eff.size());
        toys_eff.emplace_back(ConcatHist(eff_hist, eff_name));
    }
    else
    {
        eff_best_fit = ConcatHist(eff_hist, "eff_best_fit");
    }
}

void XsecCalc::ApplyEffRatio(TH1D& sel_hist_ratio, std::vector<TH1D>& sel_hist, std::vector<TH1D>& tru_hist, bool is_toy)
{
    std::vector<TH1D> eff_hist;
    TH1D eff_hist_ratio;
    for(int i = 0; i < num_signals; ++i)
    {
        TH1D eff = sel_hist[i];
        eff.Divide(&sel_hist[i], &tru_hist[i]);
        eff_hist.emplace_back(eff);
    }

    for(int j = 1; j <= sel_hist_ratio.GetNbinsX(); ++j)
    {
        double bin_eff_C = eff_hist[0].GetBinContent(j);
        double bin_eff_O = eff_hist[1].GetBinContent(j);
        double bin_val = sel_hist_ratio.GetBinContent(j);
        sel_hist_ratio.SetBinContent(j, bin_val * bin_eff_C / bin_eff_O);
        eff_hist_ratio.SetBinContent(j, bin_eff_O / bin_eff_C);
    }

    if(is_toy)
    {
        std::string eff_name = "eff_combined_toy" + std::to_string(toys_eff.size());
        toys_eff_ratio.emplace_back(eff_hist_ratio);
    }
    else
    {
        eff_best_fit_ratio = eff_hist_ratio;
    }
}

void XsecCalc::ApplyNorm(std::vector<TH1D>& vec_hist, const std::vector<double>& param, bool is_toy)
{
    for(unsigned int i = 0; i < num_signals; ++i)
    {
        ApplyTargets(i, vec_hist[i], is_toy);
        ApplyFlux(i, vec_hist[i], param, is_toy);
        ApplyBinWidth(i, vec_hist[i], perGeV);
    }
}

void XsecCalc::ApplyNormTargetsRatio(TH1D& hist, bool is_toy)
{
    double num_targets_O = 1.0; // signal 1
    double num_targets_C = 1.0; // signal 0
    if(is_toy)
    {
        num_targets_O = toy_thrower->ThrowSinglePar(v_normalization[1].num_targets_val,v_normalization[1].num_targets_err);
        num_targets_C = toy_thrower->ThrowSinglePar(v_normalization[0].num_targets_val,v_normalization[0].num_targets_err);
        v_normalization[1].target_throws.Fill(num_targets_O);
        v_normalization[0].target_throws.Fill(num_targets_C);
    }
    else
    {
        num_targets_O = v_normalization[1].num_targets_val;
        num_targets_C = v_normalization[0].num_targets_val;
    }

    hist.Scale(num_targets_C / num_targets_O);

}

void XsecCalc::ApplyTargets(const unsigned int signal_id, TH1D& hist, bool is_toy)
{
    double num_targets = 1.0;
    if(is_toy)
    {
        num_targets = toy_thrower->ThrowSinglePar(v_normalization[signal_id].num_targets_val,
                                                  v_normalization[signal_id].num_targets_err);
        v_normalization[signal_id].target_throws.Fill(num_targets);
    }
    else
        num_targets = v_normalization[signal_id].num_targets_val;

    hist.Scale(1.0 / num_targets);
}

void XsecCalc::ApplyFlux(const unsigned int signal_id, TH1D& hist, const std::vector<double>& param,
                         bool is_toy)
{
    if(v_normalization[signal_id].use_flux_fit)
    {
        double flux_int = selected_events->ReweightFluxHist(
            param, v_normalization[signal_id].flux_hist, v_normalization[signal_id].detector);
        hist.Scale(1.0 / flux_int);

        if(is_toy)
            v_normalization[signal_id].flux_throws.Fill(flux_int);
    }
    else
    {
        double flux_int = 1.0;
        if(is_toy)
        {
            flux_int = toy_thrower->ThrowSinglePar(v_normalization[signal_id].flux_int,
                                                   v_normalization[signal_id].flux_err);
            v_normalization[signal_id].flux_throws.Fill(flux_int);
        }
        else
            flux_int = v_normalization[signal_id].flux_int;

        hist.Scale(1.0 / flux_int);
    }
}

void XsecCalc::ApplyBinWidth(const unsigned int signal_id, TH1D& hist, const double unit_scale)
{
    BinManager bm = selected_events->GetBinManager(signal_id);
    for(int i = 0; i < hist.GetNbinsX(); ++i)
    {
        const double bin_width = bm.GetBinWidth(i) / unit_scale;
        const double bin_value = hist.GetBinContent(i + 1);
        hist.SetBinContent(i + 1, bin_value / bin_width);
    }
}

TH1D XsecCalc::ConcatHist(const std::vector<TH1D>& vec_hist, const std::string& hist_name)
{
    TH1D hist_combined(hist_name.c_str(), hist_name.c_str(), total_signal_bins, 0,
                       total_signal_bins);

    unsigned int bin = 1;
    for(const auto& hist : vec_hist)
    {
        for(int i = 1; i <= hist.GetNbinsX(); ++i)
            hist_combined.SetBinContent(bin++, hist.GetBinContent(i));
    }

    return hist_combined;
}

void XsecCalc::CalcCovariance(bool use_best_fit)
{
    std::cout << TAG << "Calculating covariance matrix..." << std::endl;
    std::cout << TAG << "Using " << num_toys << " toys." << std::endl;

    TH1D h_cov;
    TH1D h_cov_ratio; //LM
    if(use_best_fit)
    {
        ReweightBestFit();
        h_cov       = sel_best_fit;
        h_cov_ratio = sel_best_fit_ratio; //LM
        std::cout << TAG << "Using best fit for covariance." << std::endl;
    }
    else
    {
        TH1D h_mean("", "", total_signal_bins, 0, total_signal_bins);
        for(const auto& hist : toys_sel_events)
        {
            for(int i = 0; i < total_signal_bins; ++i)
                h_mean.Fill(i + 0.5, hist.GetBinContent(i + 1));
        }
        h_mean.Scale(1.0 / (1.0 * num_toys));
        h_cov = h_mean;

        for(int i = 1; i <= total_signal_bins; ++i)
            std::cout << "Bin " << i << ": " << h_cov.GetBinContent(i) << std::endl;

        TH1D h_mean_ratio("", "", signal_bins, 0, signal_bins);
        for(const auto& hist : toys_sel_ratio)
        {
            for(int i = 0; i < signal_bins; ++i)
                h_mean_ratio.Fill(i + 0.5, hist.GetBinContent(i + 1));
        }
        h_mean_ratio.Scale(1.0 / (1.0 * num_toys));
        h_cov_ratio = h_mean_ratio;

        std::cout << TAG << "Using mean of toys for covariance." << std::endl;
    }

    xsec_cov.ResizeTo(total_signal_bins, total_signal_bins);
    xsec_cov.Zero();

    xsec_cor.ResizeTo(total_signal_bins, total_signal_bins);
    xsec_cor.Zero();

    ratio_cov.ResizeTo(signal_bins, signal_bins);
    ratio_cov.Zero();

    ratio_cor.ResizeTo(signal_bins, signal_bins);
    ratio_cor.Zero();

    // Compute the xsec covariance
    for(const auto& hist : toys_sel_events)
    {
        for(int i = 0; i < total_signal_bins; ++i)
        {
            for(int j = 0; j < total_signal_bins; ++j)
            {
                const double x = hist.GetBinContent(i + 1) - h_cov.GetBinContent(i + 1);
                const double y = hist.GetBinContent(j + 1) - h_cov.GetBinContent(j + 1);
                xsec_cov(i, j) += x * y / (1.0 * num_toys);
            }
        }
    }

    // Compute the ratio covariance
    for(const auto& hist : toys_sel_ratio)
    {
        for(int i = 0; i < signal_bins; ++i)
        {
            for(int j = 0; j < signal_bins; ++j)
            {
                const double x = hist.GetBinContent(i + 1) - h_cov_ratio.GetBinContent(i + 1);
                const double y = hist.GetBinContent(j + 1) - h_cov_ratio.GetBinContent(j + 1);
                ratio_cov(i, j) += x * y / (1.0 * num_toys);
            }
        }
    }

    if(do_add_protonfsi_cov)
    {
        // Add the proton FSI contribution to the xsec covariance
        for(int i = 0; i < total_signal_bins; ++i)
        {   
            const double a = xsec_cov(i, i);
            const double b = (*protonfsi_cov)(i, i) * sel_best_fit.GetBinContent(i+1) * sel_best_fit.GetBinContent(i+1);
            xsec_cov(i, i) = a + b;
        }

        // Add the proton FSI contribution to the ratio covariance
        // The error from FSI on the ratio is sqrt(2) times the FSI error we get for the xsec (proton FSI)
        for(int i = 0; i < signal_bins; ++i)
        {   
            const double a = ratio_cov(i, i);
            const double b = 2 * (*protonfsi_cov)(i, i) * sel_best_fit_ratio.GetBinContent(i+1) * sel_best_fit_ratio.GetBinContent(i+1);
            ratio_cov(i, i) = a + b;
        }
    }

    // Compute the xsec correlation
    for(int i = 0; i < total_signal_bins; ++i)
    {
        for(int j = 0; j < total_signal_bins; ++j)
        {
            const double x = xsec_cov(i, i);
            const double y = xsec_cov(j, j);
            const double z = xsec_cov(i, j);
            xsec_cor(i, j) = z / (sqrt(x * y));

            if(std::isnan(xsec_cor(i, j)))
                xsec_cor(i, j) = 0.0;
        }
    }

    // Compute the ratio correlation
    for(int i = 0; i < signal_bins; ++i)
    {
        for(int j = 0; j < signal_bins; ++j)
        {
            const double x = ratio_cov(i, i);
            const double y = ratio_cov(j, j);
            const double z = ratio_cov(i, j);
            ratio_cor(i, j) = z / (sqrt(x * y));

            if(std::isnan(ratio_cor(i, j)))
                ratio_cor(i, j) = 0.0;
        }
    }

    // Set cross-section bin errors
    for(int i = 0; i < total_signal_bins; ++i)
        sel_best_fit.SetBinError(i + 1, sqrt(xsec_cov(i, i)));

    unsigned int idx = 0;
    for(int n = 0; n < signal_best_fit.size(); ++n)
    {
        unsigned int nbins = v_normalization.at(n).nbins;
        for(int i = 0; i < nbins; ++i)
            signal_best_fit.at(n).SetBinError(i + 1, sqrt(xsec_cov(i+idx,i+idx)));

        idx += nbins;
    }

    // Set ratio bin errors
    for(int i = 0; i < signal_bins; ++i)
        sel_best_fit_ratio.SetBinError(i + 1, sqrt(ratio_cov(i, i)));

    std::cout << TAG << "Covariance and correlation matrix calculated." << std::endl;
    std::cout << TAG << "Errors applied to histograms." << std::endl;
}

void XsecCalc::SaveOutput(bool save_toys)
{
    TFile* file = TFile::Open(output_file.c_str(), "RECREATE");
    std::cout << TAG << "Saving output to " << output_file << std::endl;

    file->cd();
    if(save_toys)
    {
        std::cout << TAG << "Saving toys to file." << std::endl;
        for(int i = 0; i < num_toys; ++i)
        {
            toys_sel_events.at(i).Write();
            toys_tru_events.at(i).Write();
            toys_sel_ratio.at(i).Write();
            toys_tru_ratio.at(i).Write();
            toys_eff.at(i).Write();
            toys_eff_ratio.at(i).Write();
        }
    }

    sel_best_fit.Write("sel_best_fit");
    tru_best_fit.Write("tru_best_fit");
    sel_best_fit_ratio.Write("sel_best_fit_ratio");
    tru_best_fit_ratio.Write("tru_best_fit_ratio");
    eff_best_fit.Write("eff_best_fit");
    eff_best_fit_ratio.Write("eff_best_fit_ratio");

    xsec_cov.Write("xsec_cov");
    xsec_cor.Write("xsec_cor");

    ratio_cov.Write("ratio_cov");
    ratio_cor.Write("ratio_cor");

    if(use_prefit_cov)
        selected_events->GetPrefitCov().Write("prefit_cov");

    postfit_cov->Write("postfit_cov");
    postfit_cor->Write("postfit_cor");

    TVectorD postfit_param_root(postfit_param.size(), postfit_param.data());
    postfit_param_root.Write("postfit_param");

    TVectorD prefit_original_root(prefit_param_original.size(), prefit_param_original.data());
    prefit_original_root.Write("prefit_param_original");

    TVectorD prefit_decomp_root(prefit_param_decomp.size(), prefit_param_decomp.data());
    prefit_decomp_root.Write("prefit_param_decomp");

    if(is_fit_type_throw)
    {
        TVectorD prefit_toy_root(prefit_param_toy.size(), prefit_param_toy.data());
        prefit_toy_root.Write("prefit_param_toy");
    }

    SaveSignalHist(file, signal_best_fit,    "postfit");
    SaveSignalHist(file, truth_best_fit,     "nominal");
    SaveSignalHist(file, sel_best_fit_ratio, "ratio_postfit");
    SaveSignalHist(file, tru_best_fit_ratio, "ratio_nominal");

    for(const auto& n : v_normalization)
    {
        std::string name = n.name + "_flux_nominal";
        n.flux_hist.Write(name.c_str());

        name = n.name + "_flux_throws";
        n.flux_throws.Write(name.c_str());

        name = n.name + "_target_throws";
        n.target_throws.Write(name.c_str());
    }

    if(do_read_data_events)
        SaveDataEvents(file);

    SaveExtra(file);
    file->Close();
}

void XsecCalc::SaveSignalHist(TFile* file, std::vector<TH1D> v_hists, const std::string suffix)
{
    file->cd();
    for(int id = 0; id < num_signals; ++id)
    {
        std::string hist_name = v_normalization.at(id).name + "_" + suffix;
        v_hists.at(id).SetName(hist_name.c_str());
        v_hists.at(id).SetTitle(hist_name.c_str());
        v_hists.at(id).Write();

        BinManager bm = selected_events->GetBinManager(id);
        auto cos_edges = bm.GetEdgeVector(0);
        auto pmu_edges = bm.GetEdgeVector(1);

        std::vector<std::vector<double>> bin_edges;
        bin_edges.emplace_back(std::vector<double>());
        bin_edges.at(bin_edges.size()-1).emplace_back(pmu_edges.at(0).first);
        bin_edges.at(bin_edges.size()-1).emplace_back(pmu_edges.at(0).second);

        for(int m = 1; m < cos_edges.size(); ++m)
        {
            if(cos_edges[m] != cos_edges[m-1])
                bin_edges.emplace_back(std::vector<double>());

            bin_edges.at(bin_edges.size()-1).emplace_back(pmu_edges.at(m).first);
            bin_edges.at(bin_edges.size()-1).emplace_back(pmu_edges.at(m).second);
        }

        for(int n = 0; n < bin_edges.size(); ++n)
        {
            std::sort(bin_edges.at(n).begin(), bin_edges.at(n).end());
            auto iter = std::unique(bin_edges.at(n).begin(), bin_edges.at(n).end());
            bin_edges.at(n).erase(iter, bin_edges.at(n).end());
        }

        unsigned int offset = 0;
        for(int k = 0; k < bin_edges.size(); ++k)
        {
            std::string name = v_normalization.at(id).name + "_cos_bin" + std::to_string(k)
                               + "_" + suffix;
            TH1D temp(name.c_str(), name.c_str(), bin_edges.at(k).size()-1, bin_edges.at(k).data());

            for(int l = 1; l <= temp.GetNbinsX(); ++l)
            {
                temp.SetBinContent(l, v_hists.at(id).GetBinContent(l+offset));
                temp.SetBinError(l, v_hists.at(id).GetBinError(l+offset));
            }
            offset += temp.GetNbinsX();
            // temp.GetXaxis()->SetRange(1,temp.GetNbinsX()-1);
            temp.GetXaxis()->SetRange(1,temp.GetNbinsX()); //LM
            temp.Write();
        }
    }
}

void XsecCalc::SaveSignalHist(TFile* file, TH1D v_hists, const std::string suffix)
{
    file->cd();
    int id = 0;
    std::string hist_name = v_normalization.at(id).name + "_" + suffix;
    v_hists.SetName(hist_name.c_str());
    v_hists.SetTitle(hist_name.c_str());
    v_hists.Write();

    BinManager bm = selected_events->GetBinManager(id);
    auto cos_edges = bm.GetEdgeVector(0);
    auto pmu_edges = bm.GetEdgeVector(1);

    std::vector<std::vector<double>> bin_edges;
    bin_edges.emplace_back(std::vector<double>());
    bin_edges.at(bin_edges.size()-1).emplace_back(pmu_edges.at(0).first);
    bin_edges.at(bin_edges.size()-1).emplace_back(pmu_edges.at(0).second);

    for(int m = 1; m < cos_edges.size(); ++m)
    {
        if(cos_edges[m] != cos_edges[m-1])
            bin_edges.emplace_back(std::vector<double>());

        bin_edges.at(bin_edges.size()-1).emplace_back(pmu_edges.at(m).first);
        bin_edges.at(bin_edges.size()-1).emplace_back(pmu_edges.at(m).second);
    }

    for(int n = 0; n < bin_edges.size(); ++n)
    {
        std::sort(bin_edges.at(n).begin(), bin_edges.at(n).end());
        auto iter = std::unique(bin_edges.at(n).begin(), bin_edges.at(n).end());
        bin_edges.at(n).erase(iter, bin_edges.at(n).end());
    }

    unsigned int offset = 0;
    for(int k = 0; k < bin_edges.size(); ++k)
    {
        std::string name = v_normalization.at(id).name + "_cos_bin" + std::to_string(k) + "_" + suffix;
        TH1D temp(name.c_str(), name.c_str(), bin_edges.at(k).size()-1, bin_edges.at(k).data());

        for(int l = 1; l <= temp.GetNbinsX(); ++l)
        {
            temp.SetBinContent(l, v_hists.GetBinContent(l+offset));
            temp.SetBinError(l, v_hists.GetBinError(l+offset));
        }
        offset += temp.GetNbinsX();
        // temp.GetXaxis()->SetRange(1,temp.GetNbinsX()-1);
        temp.GetXaxis()->SetRange(1,temp.GetNbinsX()); //LM
        temp.Write();
    }
}

void XsecCalc::SaveExtra(TFile* output)
{
    if(extra_hists.empty())
        return;

    std::ifstream fin(extra_hists, std::ios::in);
    if(!fin.is_open())
    {
        std::cout << ERR << "Failed to open file: " << extra_hists << std::endl;
        return;
    }
    else
    {
        std::cout << TAG << "Saving extra histograms from fit output." << std::endl;
        TFile* file = TFile::Open(input_file.c_str(), "READ");
        output->cd();

        std::string line;
        while(std::getline(fin, line))
        {
            if(line.front() != COMMENT_CHAR)
            {
                TH1D* temp = (TH1D*)file->Get(line.c_str());
                if(temp != nullptr)
                    temp->Write();
            }
        }
        file->Close();
    }
}

void XsecCalc::SaveDataEvents(TFile* output)
{
    std::cout << TAG << "Reading data events." << std::endl;
    std::fstream f;
    f.open(config_file, std::ios::in);

    json j;
    f >> j;

    // std::string input_dir = std::string(std::getenv("XSLLHFITTER")) + j["input_dir"].get<std::string>();
    std::string input_dir = std::string(std::getenv("XSLLHFITTER")); //LM

    std::string true_events_config = input_dir + j["tru_config"].get<std::string>();
    FitObj fake_data_events(true_events_config, "trueEvents", true, true);

    fake_data_events.ReweightNominal();
    auto fake_data_hists = fake_data_events.GetSignalHist();

    ApplyNorm(fake_data_hists, prefit_param_original, false);

    for(unsigned int i = 0; i < fake_data_hists.size(); ++i)
    {
        //std::string name = "hist_data_signal_" + std::to_string(i);
        std::string name = v_normalization.at(i).name + "_data";
        fake_data_hists.at(i).SetName(name.c_str());
        fake_data_hists.at(i).SetTitle(name.c_str());
    }

    auto fake_data_concat = ConcatHist(fake_data_hists, "fake_data_concat");

    std::cout << TAG << "Saving data histograms." << std::endl;
    output->cd();
    fake_data_concat.Write();

    SaveSignalHist(output, fake_data_hists,       "data");



    // Save ratio //LM
    FitObj fake_data_events_ratio(true_events_config, "trueEvents", true, true);

    fake_data_events_ratio.ReweightNominal();
    auto fake_data_hists_ratio = fake_data_events_ratio.GetRatioHist();

    ApplyNormTargetsRatio(fake_data_hists_ratio, false);

    //std::string name = "hist_data_signal_" + std::to_string(i);
    std::string name = "fake_data_ratio";
    fake_data_hists_ratio.SetName(name.c_str());
    fake_data_hists_ratio.SetTitle(name.c_str());

    output->cd();
    fake_data_hists_ratio.Write();

    SaveSignalHist(output, fake_data_hists_ratio, "ratio_data");

    f.close();
}
