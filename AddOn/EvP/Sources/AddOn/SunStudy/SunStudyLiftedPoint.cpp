#include "SunStudy/SunStudyLiftedPoint.hpp"

#include "SunStudy/SunStudyOccluders.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <algorithm>
#include <cmath>

namespace evp::sunstudy {

namespace {

constexpr double kTMin = 0.001;

// The nearest hit distance over both occluder engines, or < 0 for none.
double NearestT (const StudyRecord& record, const double origin[3], const double direction[3])
{
    if (record.occluders == nullptr)
        return -1.0;
    double best = -1.0;
    for (const auto& engine : { record.occluders->analysis, record.occluders->context }) {
        if (engine == nullptr)
            continue;
        const auto hit = engine->Raycast (origin, direction, 0.0);
        if (hit.hit && (best < 0.0 || hit.t < best))
            best = hit.t;
    }
    return best;
}

} // namespace

bool SunStudySceneHit (const StudyRecord& record, const double origin[3], const double direction[3], double hit[3])
{
    double unit[3] = { direction[0], direction[1], direction[2] };
    const double length = std::sqrt (unit[0] * unit[0] + unit[1] * unit[1] + unit[2] * unit[2]);
    if (!(length > 0.0) || !std::isfinite (length))
        return false;
    for (double& component : unit)
        component /= length;
    const double t = NearestT (record, origin, unit);
    if (t < 0.0)
        return false;
    for (int axis = 0; axis < 3; ++axis)
        hit[axis] = origin[axis] + t * unit[axis];
    return true;
}

bool LiftSunStudyPoint (const StudyRecord& record, const double surface[3], double height, LiftedPoint& lifted)
{
    lifted = LiftedPoint ();
    if (!std::isfinite (surface[0]) || !std::isfinite (surface[1]) || !std::isfinite (surface[2]) ||
        !std::isfinite (height))
        return false;
    std::copy (surface, surface + 3, lifted.surface);
    lifted.requestedHeight = (std::max) (0.0, height);
    lifted.height = lifted.requestedHeight;
    // Overhead: start just above the surface so the surface itself is not hit.
    const double up[3] = { 0.0, 0.0, 1.0 };
    const double origin[3] = { surface[0], surface[1], surface[2] + kTMin };
    const double ceiling = NearestT (record, origin, up);
    if (ceiling >= 0.0 && ceiling + kTMin - kLiftedPointClearance < lifted.height) {
        lifted.height = (std::max) (0.0, ceiling + kTMin - kLiftedPointClearance);
        lifted.clamped = true;
    }
    std::copy (surface, surface + 3, lifted.position);
    // A small lift even at zero keeps the receiver off the surface it stands on.
    lifted.position[2] += (std::max) (lifted.height, kTMin * 2.0);
    return true;
}

bool LiftedPointDay (const StudyRecord& record, const LiftedPoint& lifted, PointDay& day)
{
    day = PointDay ();
    if (record.occluders == nullptr)
        return false;
    const auto& steps = record.series.Steps ();
    day.hoursPerStep = double (record.timestepMinutes) / 60.0;
    day.windowFrom = std::clamp (record.hourFrom, 0, 24) * 60;
    day.windowTo = std::clamp (record.hourTo, 0, 24) * 60;
    day.samples = 1;
    for (const SunStep& step : steps) {
        double direction[3] = { step.direction[0], step.direction[1], step.direction[2] };
        const double length =
            std::sqrt (direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
        if (!(length > 0.0) || !std::isfinite (length))
            return false;
        for (double& component : direction)
            component /= length;
        double origin[3];
        for (int axis = 0; axis < 3; ++axis)
            origin[axis] = lifted.position[axis] + kTMin * direction[axis];
        day.minutes.push_back (uint16_t (step.time.hour * 60 + step.time.minute));
        day.lit.push_back (NearestT (record, origin, direction) < 0.0 ? 1.0 : 0.0);
        day.facing.push_back (1.0); // omnidirectional: the sun is never behind it
    }
    double direct = 0.0;
    for (const double lit : day.lit)
        direct += lit * day.hoursPerStep;
    day.minDirectHours = day.maxDirectHours = direct;
    return true;
}

} // namespace evp::sunstudy
