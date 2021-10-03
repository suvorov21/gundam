#include "FitParameters.hh"
using xsllh::FitBin;

FitParameters::FitParameters(const std::string& par_name)
{
    m_name = par_name;
    signal_id = 0;
}

FitParameters::~FitParameters()
{;}

// Checks if binning file exists. If it does, it fills the given bins argument with the binning from the given text file:
bool FitParameters::SetBinning(const std::string& file_name, std::vector<FitBin>& bins)
{
    std::ifstream fin(file_name, std::ios::in);
    if(!fin.is_open())
    {
        std::cerr << ERR << "In FitParameters::SetBinning()\n"
                  << ERR << "Failed to open binning file: " << file_name << std::endl;
        return false;
    }

    else
    {
        std::string line;
        while(getline(fin, line))
        {
            std::stringstream ss(line);
            double D1_1, D1_2, D2_1, D2_2, D3_1, D3_2, D4_1, D4_2;
            if(!(ss>>D2_1>>D2_2>>D1_1>>D1_2>>D4_1>>D4_2>>D3_1>>D3_2))
            {
                std::cout << WAR << "In FitParameters::SetBinning()\n"
                          << WAR << "Bad line format: " << line << std::endl;
                continue;
            }
            bins.emplace_back(FitBin(D1_1, D1_2, D2_1, D2_2, D3_1, D3_2, D4_1, D4_2));
        }
        fin.close();

        std::cout << TAG << "Fit binning: \n";
        for(std::size_t i = 0; i < bins.size(); ++i)
        {
            std::cout << std::setw(5) << i
                      << std::setw(8) << bins[i].D2low
                      << std::setw(8) << bins[i].D2high
                      << std::setw(8) << bins[i].D1low
                      << std::setw(8) << bins[i].D1high
                      << std::setw(8) << bins[i].D4low
                      << std::setw(8) << bins[i].D4high
                      << std::setw(8) << bins[i].D3low
                      << std::setw(8) << bins[i].D3high << std::endl;
        }

        return true;
    }
}

int FitParameters::GetBinIndex(const int sig, double D1, double D2, double D3, double D4) const
{
    int bin = BADBIN;
    const std::vector<FitBin> &temp_bins = m_signal_bins.at(sig);

    for(int i = 0; i < temp_bins.size(); ++i)
    {
        if(D1 >= temp_bins[i].D1low && D1 < temp_bins[i].D1high &&
           D2 >= temp_bins[i].D2low && D2 < temp_bins[i].D2high &&
           D3 >= temp_bins[i].D3low && D3 < temp_bins[i].D3high &&
           D4 >= temp_bins[i].D4low && D4 < temp_bins[i].D4high)
        {
            bin = i;
            break;
        }
    }
    return bin;
}


// Determines which truth bin each signal events falls into:
void FitParameters::InitEventMap(std::vector<AnaSample*> &sample, int mode)
{
    // Loop over samples and check if the detector is part of the fit parameters:
    for(const auto& s : sample)
    {
        if(!std::any_of(v_detectors.begin(), v_detectors.end(),
                        [&](std::string det){ return s->GetDetector() == det; }))
        {
            std::cerr << ERR << "In FitParameters::InitEventMap\n"
                      << ERR << "Detector " << s -> GetDetector() << " not part of fit parameters.\n"
                      << ERR << "Not building event map." << std::endl;
            return;
        }
    }

    // Set up signal (template) parameters (one per signal bin):
    InitParameters();

    m_evmap.clear();

    // Loop over all samples:
    for(std::size_t s=0; s < sample.size(); s++)
    {
        // Map which will be filled with the bin index of each event (-1 if event is not signal and -2 if event is signal but does not fall into bins):
        std::vector<int> sample_map;

        int N_badbins = 0;

        // Loop over all events in current sample:
        for(int i=0; i < sample[s] -> GetN(); i++)
        {
            AnaEvent* ev = sample[s] -> GetEvent(i);

            // SIGNAL DEFINITION TIME
            // Warning, important hard coding up ahead:
            // This is where your signal is actually defined, i.e. what you want to extract an xsec for
            // N.B In Sara's original code THIS WAS THE OTHER WAY AROUND i.e. this if statement asked what was NOT your signal
            // Bare that in mind if you've been using older versions of the fitter.

            if(ev -> isSignalEvent())
            {
                double D1 = ev -> GetTrueD1();
                double D2 = ev -> GetTrueD2();
                double D3 = ev -> GetTrueD3();
                double D4 = ev -> GetTrueD4();

                //int bin = GetBinIndex(sample[s] -> GetDetector(), D1, D2);
                // Check which truth bin the event fall into:
                int bin = GetBinIndex(ev -> GetSignalType(), D1, D2, D3, D4);
#ifndef NDEBUG
                
                if(bin == BADBIN)
                {
                    ++N_badbins;
                    /*
                    std::cout << WAR << m_name << ", Event: " << i << std::endl
                              << WAR << "D1 = " << D1 << ", D2 = " << D2 << ", D3 = " << D3 << ", D4 = " << D4 << ", falls outside bin ranges." << std::endl
                              << WAR << "This event will be ignored in the analysis." << std::endl;
                    */
                }
                
#endif
                // If event is signal and falls into binning we append the bin index to sample_map:
                sample_map.push_back(bin);
            } // event loop

            // If the event is not signal, we append -1 to the sample_map:
            else
            {
                sample_map.push_back(PASSEVENT);
                continue;
            }

        } // sample loop
        std::cout << TAG << "Number of signal events that fall outside bin ranges for sample " << sample.at(s)->GetName() << ": " << N_badbins << std::endl;

        m_evmap.push_back(sample_map);
    }
}

// Multiplies the current event weight for AnaEvent* event with the template parameter for the sample and bin that this event falls in:
void FitParameters::ReWeight(AnaEvent* event, const std::string& det, int nsample, int nevent, std::vector<double> &params)
{
    // m_evmap is a vector containing vectors of which bin an event falls in for all samples. This event map needs to be built first, otherwise an error is thrown:
    if(m_evmap.empty())
    {
        std::cerr << ERR << "In FitParameters::ReWeight()\n"
                  << ERR << "Need to build event map index for " << m_name << std::endl;
        return;
    }

    // Get the bin that this event falls in:
    const int bin = m_evmap[nsample][nevent];

    // Event is skipped if it isn't signal (if bin = PASSEVENT = -1):
    if(bin == PASSEVENT) return;

    // If the bin fell out of the valid bin ranges (if bin = BADBIN = -2), we assign an event weight of 1 and pretend the event just didn't happen:
    if(bin == BADBIN)
        event -> AddEvWght(1.0);
    
    // Otherwise, we multiply the event weight with the parameter for this sample, signal and bin:
    else
    {
        // If the bin number is larger than the number of parameters, we set the event weight to zero (this should not happen):
        if(bin > params.size())
        {
            std::cout << WAR << "In FitParameters::ReWeight()\n"
                      << WAR << "Number of bins in " << m_name << " does not match num of parameters.\n"
                      << WAR << "Setting event weight to zero." << std::endl;
            event -> AddEvWght(0.0);
        }

        // Multiply the current event weight by the parameter for the bin and signal that this event falls in (defined in AnaEvent.hh):
        event -> AddEvWght(params[bin + m_sig_offset.at(event->GetSignalType())]);

        /*
        // Print information about current event:
        std::cout << "-----------------" << std::endl;
        std::cout << "Ev D1: " << event -> GetTrueD1() << std::endl
                  << "Ev D2: " << event -> GetTrueD2() << std::endl;
        std::cout << "Ev ST: " << event -> GetSignalType() << std::endl;
        std::cout << "Ev TP: " << event -> GetTopology() << std::endl;
        std::cout << "Ev TG: " << event -> GetTarget() << std::endl;
        std::cout << "Bin  : " << bin << std::endl;
        std::cout << "Off  : " << m_sig_offset.at(event->GetSignalType()) << std::endl;
        */
    }
}

// Set up fit parameters for signal (template) params:
void FitParameters::InitParameters()
{
    unsigned int offset = 0;
    for(const auto& sig : v_signals)
    {
        m_sig_offset.emplace(std::make_pair(sig, offset));
        const int nbins = m_signal_bins.at(sig).size();
        for(int i = 0; i < nbins; ++i)
        {
            pars_name.push_back(Form("%s_sig%d_%d", m_name.c_str(), sig, i));
            pars_prior.push_back(1.0); //all weights are 1.0 a priori
            pars_step.push_back(0.05);
            pars_limlow.push_back(0.0);
            pars_limhigh.push_back(10.0);
            pars_fixed.push_back(false);
        }

        std::cout << TAG << "Total " << nbins << " parameters at "
                  << offset << " for signal ID " << sig << std::endl;
        offset += nbins;
    }

    Npar = pars_name.size();
    pars_original = pars_prior;
}
/*
void FitParameters::AddDetector(const std::string& det, const std::string& f_binning)
{
    std::cout << TAG << "Adding detector " << det << " for " << m_name << std::endl;

    std::vector<FitBin> temp_vector;
    if(SetBinning(f_binning, temp_vector))
    {
        m_fit_bins.emplace(std::make_pair(det, temp_vector));
        v_detectors.emplace_back(det);
    }
    else
        std::cout << WAR << "Adding detector failed." << std::endl;
}
*/

void FitParameters::AddDetector(const std::string& det, const std::vector<SignalDef>& v_input)
{
    std::cout << TAG << "Adding detector " << det << " for " << m_name << std::endl;

    // Loop over all the signal definitions (defined as template_par in .json file):
    for(const auto& sig : v_input)
    {
        
        //if(sig.detector != det || sig.use_signal == false)
        //    continue;
        //std::cout << "sig.detector: " << sig.detector << std::endl;

        v_signals.emplace_back(signal_id);

        std::cout << TAG << "Adding signal " << sig.name << " with ID " << signal_id
                  << " to fit." << std::endl;

        std::vector<FitBin> temp_vector;

        // Checks if binning file exists. If yes, fills temp_vector with the binning from sig.binning file:
        if(SetBinning(sig.binning, temp_vector))
        {
            // Fill m_signal_bins with signal ID and current binning:
            m_signal_bins.emplace(std::make_pair(signal_id, temp_vector));
            v_detectors.emplace_back(det);
            signal_id++;
        }
        else
            std::cout << WAR << "Adding signal binning failed." << std::endl;
    }
}

double FitParameters::CalcRegularisation(const std::vector<double>& params) const
{
    return CalcRegularisation(params, m_regstrength, m_regmethod);
}

double FitParameters::CalcRegularisation(const std::vector<double>& params, double strength,
                                         RegMethod flag) const
{
    /*
    auto L2_lambda = [](double a, double b) -> double
    {
        return (a - b) * (a - b);
    };
    */

    double L_reg = 0;
    unsigned int offset = 0;
    for(const auto& signal_id : v_signals)
    {
        const unsigned int nbins = m_signal_bins.at(signal_id).size();

        if(flag == kL1Reg)
        {
            for(int i = offset; i < offset+nbins-1; ++i)
                L_reg += std::fabs(params[i] - params[i+1]);
        }

        else if(flag == kL2Reg)
        {
            for(int i = offset; i < offset+nbins-1; ++i)
                L_reg += (params[i] - params[i+1]) * (params[i] - params[i+1]);
        }

        else
        {
            std::cout << WAR << "In CalcRegularisation(): "
                      << "Invalid regularisation method! Returning 0.\n";
            break;
        }

        offset += nbins;
    }

    return strength * L_reg;
}
