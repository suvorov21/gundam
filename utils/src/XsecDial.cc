#include "XsecDial.hh"

XsecDial::XsecDial(const std::string& dial_name)
    : m_name(dial_name)
{
}

XsecDial::XsecDial(const std::string& dial_name, const std::string& fname_binning,
                   const std::string& fname_splines)
    : m_name(dial_name)
{
    SetBinning(fname_binning);
    ReadSplines(fname_splines);
}

void XsecDial::SetBinning(const std::string& fname_binning)
{
    bin_manager.SetBinning(fname_binning);
    nbins = bin_manager.GetNbins();
}

void XsecDial::ReadSplines(const std::string& fname_splines)
{
    v_splines.clear();

    TFile* file_splines = TFile::Open(fname_splines.c_str(), "READ");
    if(file_splines == 0)
    {
        std::cout << "[ERROR]: Failed to open " << fname_splines << std::endl;
        return;
    }

    TIter key_list(file_splines -> GetListOfKeys());
    TKey* key = nullptr;
    while((key = (TKey*)key_list.Next()))
    {
        TGraph* spline = (TGraph*)key -> ReadObj();
        v_splines.emplace_back(*spline);
    }
    file_splines -> Close();
    delete file_splines;

    v_splines.shrink_to_fit();
}

int XsecDial::GetSplineIndex(int topology, int reaction, double q2) const
{
    const int b = bin_manager.GetBinIndex(std::vector<double>{q2});
    return topology * nreac * nbins + reaction * nbins + b;
}

int XsecDial::GetSplineIndex(const std::vector<int>& var, const std::vector<double>& bin) const
{
    if(var.size() != m_dimensions.size())
        return -1;

    int idx = bin_manager.GetBinIndex(bin);
    for(int i = 0; i < var.size(); ++i)
        idx += var[i] * m_dimensions[i];
    return idx;
}

double XsecDial::GetSplineValue(int index, double dial_value) const
{
    double splineValue;
    if(dial_value<m_limit_lo)
    {
        splineValue = v_splines.at(index).Eval(m_limit_lo);
        // std::cout << TAG << "Dial value is outside of range !!! " 
        //                 << "dial_value("<<index<<") = " << dial_value << " set to the low limit value instead, " << m_limit_lo
        //                 << " --> splineValue = " << splineValue << std::endl;
    }
    else if(dial_value>m_limit_hi)
    {
        splineValue = v_splines.at(index).Eval(m_limit_hi);
        // std::cout << TAG << "Dial value is outside of range! "
        //                 << "dial_value("<<index<<") = " << dial_value << " set to the high limit value instead, " << m_limit_hi
        //                 << " --> splineValue = " << splineValue << std::endl;
    }
    else
        splineValue = v_splines.at(index).Eval(dial_value);

    if(splineValue > 1E3)
    {
        splineValue = v_splines.at(index).Eval(m_limit_lo+0.13);
    }
// std::cout << TAG << "Dial value is " << dial_value << " (should be between " << m_limit_lo << " and " << m_limit_hi << ") "
//                      << "and spline value is " << splineValue << std::endl;
//     
    if(index >= 0)
        return splineValue;
    else
        return 1.0;
}

std::string XsecDial::GetSplineName(int index) const
{
    return std::string(v_splines.at(index).GetName());
}

void XsecDial::SetVars(double nominal, double step, double limit_lo, double limit_hi)
{
    m_nominal = nominal;
    m_step = step;
    m_limit_lo = limit_lo;
    m_limit_hi = limit_hi;
}

void XsecDial::SetDimensions(int num_top, int num_reac)
{
    ntop = num_top;
    nreac = num_reac;
}

void XsecDial::SetDimensions(const std::vector<int>& dim)
{
    m_dimensions = dim;
}

void XsecDial::Print(bool print_bins) const
{
    std::cout << TAG << "Name: " <<  m_name << std::endl
              << TAG << "Nominal: " << m_nominal << std::endl
              << TAG << "Step: " << m_step << std::endl
              << TAG << "Limits: [" << m_limit_lo << "," << m_limit_hi << "]" << std::endl
              << TAG << "Splines: " << v_splines.size() << std::endl;

    std::cout << TAG << "Dimensions:";
    for(const auto& dim : m_dimensions)
        std::cout << " " << dim;
    std::cout << std::endl;

    if(print_bins)
        bin_manager.Print();
}
