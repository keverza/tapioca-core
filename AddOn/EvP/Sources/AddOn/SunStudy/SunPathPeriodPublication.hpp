#ifndef TAPIOCA_SUNPATHPERIODPUBLICATION_HPP
#define TAPIOCA_SUNPATHPERIODPUBLICATION_HPP

#include "SunStudy/SunPathStudyData.hpp"

namespace geomsrv::sunstudyhud {

// Archive day/step offsets are not geometry identity. Match the committed
// calendar date and minute, including sunless days and nonidentical ordering.
inline SunPathAppliedFrame SunPathAppliedFrameFor (const SunPathPeriodGeometry& geometry, int frame, int month,
                                                   int dateMonth, int dateDay, int minute)
{
    SunPathAppliedFrame result { frame, month, -1, -1 };
    if (frame == 0)
        return result;
    for (size_t d = 0; d < geometry.days.size (); ++d) {
        const auto& day = geometry.days[d];
        if (day.month != dateMonth || day.day != dateDay)
            continue;
        result.day = int (d);
        for (size_t s = 0; s < day.steps.size (); ++s)
            if (day.steps[s].time.hour * 60 + day.steps[s].time.minute == minute) {
                result.step = int (s);
                break;
            }
        break;
    }
    return result;
}

// All methods run under the retained-period owner's mutex. Readers keep the
// returned immutable envelope after releasing that lock; no HUD state is shared.
class SunPathPeriodPublication {
  public:
    void Retain (std::shared_ptr<const SunPathPeriodGeometry> geometry)
    {
        data = std::make_shared<SunPathPeriodData> (SunPathPeriodData { std::move (geometry), {} });
    }

    bool Apply (const std::shared_ptr<const SunPathPeriodGeometry>& geometry, SunPathAppliedFrame applied)
    {
        if (data == nullptr || data->geometry != geometry)
            return false;
        data = std::make_shared<SunPathPeriodData> (SunPathPeriodData { geometry, applied });
        return true;
    }

    void Reset ()
    {
        data.reset ();
    }

    std::shared_ptr<const SunPathPeriodData> Latest () const
    {
        return data;
    }

  private:
    std::shared_ptr<const SunPathPeriodData> data;
};

} // namespace geomsrv::sunstudyhud

#endif
