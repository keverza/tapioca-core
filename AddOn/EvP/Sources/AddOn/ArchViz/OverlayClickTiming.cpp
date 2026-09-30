// ArchViz/OverlayClickTiming -- see the header.

#include "ArchViz/OverlayClickTiming.hpp"

#include <algorithm>

namespace geomsrv {
namespace archviz {
namespace overlayclicks {

const char* TargetName (Target target)
{
    switch (target) {
        case Target::Hud:
            return "hud";
        case Target::View:
            return "view";
        case Target::Other:
            return "other";
    }
    return "other";
}

void Meter::Reset ()
{
    *this = Meter ();
}

void Meter::Wake (uint64_t now)
{
    if (busy_)
        return;
    busy_ = true;
    start_ = now;
}

void Meter::Press (Target target, const char* windowClass, uint64_t now)
{
    Sample& sample = samples_[count_ % kSamples];
    sample = Sample ();
    sample.target = target;
    sample.at = now;
    if (windowClass != nullptr) {
        size_t k = 0;
        for (; k + 1 < sizeof (sample.windowClass) && windowClass[k] != '\0'; ++k)
            sample.windowClass[k] = windowClass[k];
        sample.windowClass[k] = '\0';
    }
    ++count_;
    // Whatever the thread was busy with before the press was not the press's.
    busy_ = true;
    start_ = now;
}

Sample* Meter::Open (uint64_t now)
{
    if (count_ == 0)
        return nullptr;
    Sample& newest = samples_[(count_ - 1) % kSamples];
    return now >= newest.at && now - newest.at < kWindowMicroseconds ? &newest : nullptr;
}

void Meter::Idle (uint64_t now)
{
    ++idles_;
    if (!busy_)
        return;
    busy_ = false;
    if (count_ == 0)
        return;
    Sample& newest = samples_[(count_ - 1) % kSamples];
    const uint64_t end = newest.at + kWindowMicroseconds;
    if (start_ < newest.at || start_ >= end || now < start_)
        return;
    newest.busyMicroseconds += uint32_t ((std::min) (now, end) - start_);
    ++newest.bursts;
    if (newest.firstIdleMicroseconds == 0)
        newest.firstIdleMicroseconds = uint32_t ((std::max) (now - newest.at, uint64_t (1)));
}

void Meter::Layout (uint32_t microseconds, uint64_t now)
{
    if (Sample* sample = Open (now); sample != nullptr) {
        sample->layoutMicroseconds += microseconds;
        ++sample->layouts;
    }
}

void Meter::Redraw (uint32_t microseconds, uint64_t now)
{
    if (Sample* sample = Open (now); sample != nullptr) {
        sample->redrawMicroseconds += microseconds;
        ++sample->redraws;
    }
}

std::vector<Sample> Meter::Samples (uint64_t now) const
{
    std::vector<Sample> out;
    const uint64_t held = (std::min) (count_, uint64_t (kSamples));
    for (uint64_t k = count_ - held; k < count_; ++k) {
        Sample sample = samples_[k % kSamples];
        sample.complete = now >= sample.at && now - sample.at >= kWindowMicroseconds;
        out.push_back (sample);
    }
    return out;
}

} // namespace overlayclicks
} // namespace archviz
} // namespace geomsrv
