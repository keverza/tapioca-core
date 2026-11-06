#include "SunStudy/SunStudyPointProfile.hpp"

#include <algorithm>

namespace evp::sunstudy {

namespace {

bool Faces (const double* normal, const double sun[3])
{
    if (normal == nullptr)
        return true;
    const double incidence = normal[0] * sun[0] + normal[1] * sun[1] + normal[2] * sun[2];
    return !(incidence <= 0.0); // the day engine's cull: NaN is traced, not culled
}

} // namespace

bool ComputePointProfile (const double position[3], const double* normal, const std::vector<ProfileDay>& days,
                          const ITraversal& traversal, double tmin, PointProfile& profile,
                          const std::function<bool ()>& cancelled)
{
    profile = PointProfile ();
    // Every facing step's ray in one batch: the traversal parallelises it.
    std::vector<OcclusionRay> rays;
    for (const ProfileDay& day : days)
        for (const SunStep& step : day.steps)
            if (Faces (normal, step.direction)) {
                OcclusionRay ray;
                std::copy (position, position + 3, ray.origin);
                std::copy (step.direction, step.direction + 3, ray.dir);
                ray.tmin = tmin;
                rays.push_back (ray);
            }
    if (cancelled && cancelled ())
        return false;
    std::vector<uint8_t> occluded (rays.size (), 0);
    if (!rays.empty ())
        traversal.OccludeRays (rays.data (), rays.size (), occluded.data ());
    if (cancelled && cancelled ())
        return false;
    profile.rays = rays.size ();

    size_t next = 0;
    for (const ProfileDay& day : days) {
        const int month = std::clamp (day.month, 1, 12) - 1;
        double up = 0.0, facing = 0.0, lit = 0.0;
        for (const SunStep& step : day.steps) {
            const int hour = std::clamp (step.time.hour, 0, 23);
            const double weighted = day.weightDays * day.hoursPerStep;
            profile.up[month][hour] += weighted;
            up += day.hoursPerStep;
            if (!Faces (normal, step.direction))
                continue;
            profile.facing[month][hour] += weighted;
            facing += day.hoursPerStep;
            if (next < occluded.size () && occluded[next] == 0) {
                profile.lit[month][hour] += weighted;
                lit += day.hoursPerStep;
            }
            ++next;
        }
        profile.dayMonth.push_back (day.month);
        profile.dayOfMonth.push_back (day.day);
        profile.dayUp.push_back (up);
        profile.dayFacing.push_back (facing);
        profile.dayLit.push_back (lit);
    }
    return true;
}

} // namespace evp::sunstudy
