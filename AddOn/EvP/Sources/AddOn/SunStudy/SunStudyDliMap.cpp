#include "SunStudy/SunStudyDliMap.hpp"

#include <algorithm>
#include <cmath>

namespace evp::sunstudy {

namespace {

constexpr double kRampStep = 5.0; // mol/m2/day

double ValueOf (const std::array<double, 12>& months, int month)
{
    if (month > 0)
        return months[size_t (month - 1)];
    double sum = 0.0;
    int count = 0;
    for (double v : months)
        if (std::isfinite (v)) {
            sum += v;
            ++count;
        }
    return count > 0 ? sum / count : std::nan ("");
}

} // namespace

double DliRampTop (double largest)
{
    if (!std::isfinite (largest) || largest <= kRampStep)
        return kRampStep;
    return std::ceil (largest / kRampStep) * kRampStep;
}

bool BuildDliMap (const std::vector<std::array<double, 12>>& dli, int month, DliMap& out, std::string& error)
{
    out = DliMap ();
    if (month < 0 || month > 12) {
        error = "the month must be 1-12, or 0 for the mean";
        return false;
    }
    out.values.reserve (dli.size ());
    for (const auto& months : dli) {
        const double v = ValueOf (months, month);
        if (!std::isfinite (v) || v < 0.0) {
            ++out.missing;
            out.values.push_back (0.0);
            continue;
        }
        out.values.push_back (v);
        out.largest = (std::max) (out.largest, v);
    }
    if (out.values.empty () || out.missing == out.values.size ()) {
        error =
            month > 0 ? "no sample has a daily light integral for that month" : "no sample has a daily light integral";
        out = DliMap ();
        return false;
    }
    out.top = DliRampTop (out.largest);
    return true;
}

} // namespace evp::sunstudy
