#include "ArchViz/ViewerFramePacing.hpp"

#include <algorithm>
#include <cstdio>

namespace geomsrv::archviz {

namespace {

// A gap this long is the viewer idle, closed or reopened, not a hitch.
constexpr double kRestartSeconds = 1.0;

} // namespace

std::string ViewerFramePacing::Note (double nowSeconds)
{
    if (last_ < 0.0 || nowSeconds - last_ > kRestartSeconds) {
        started_ = last_ = nowSeconds;
        intervals_.clear ();
        return {};
    }
    intervals_.push_back ((nowSeconds - last_) * 1000.0);
    last_ = nowSeconds;
    if (nowSeconds - started_ < window_ || intervals_.empty ())
        return {};

    std::vector<double> sorted = intervals_;
    std::sort (sorted.begin (), sorted.end ());
    const double median = sorted[sorted.size () / 2];
    const double p99 = sorted[(std::min) (sorted.size () - 1, sorted.size () * 99 / 100)];
    const double worst = sorted.back ();
    size_t hitches = 0;
    for (const double interval : intervals_)
        if (interval > 2.0 * median)
            ++hitches;
    char line[200];
    std::snprintf (line, sizeof (line),
                   "viewer pacing: frames=%u over %.1f s, interval median=%.2f ms p99=%.2f ms worst=%.2f ms, "
                   "hitches=%u (over 2x median)",
                   unsigned (intervals_.size ()), nowSeconds - started_, median, p99, worst, unsigned (hitches));
    started_ = nowSeconds;
    intervals_.clear ();
    return line;
}

} // namespace geomsrv::archviz
