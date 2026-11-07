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

double Distance2 (const double* a, const double* b)
{
    const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}

} // namespace

bool FacesSun (const double* normal, const double sun[3])
{
    return Faces (normal, sun);
}

std::vector<size_t> SamplesInSphere (const std::vector<double>& positions, const double centre[3], double radius,
                                     size_t cap, double snapped[3])
{
    const size_t count = positions.size () / 3;
    if (count == 0 || cap == 0)
        return {};
    size_t nearest = 0;
    double best = Distance2 (&positions[0], centre);
    for (size_t s = 1; s < count; ++s)
        if (const double d = Distance2 (&positions[3 * s], centre); d < best) {
            best = d;
            nearest = s;
        }
    std::copy (&positions[3 * nearest], &positions[3 * nearest] + 3, snapped);
    std::vector<std::pair<double, size_t>> within;
    const double limit = radius > 0.0 ? radius * radius : 0.0;
    for (size_t s = 0; s < count; ++s)
        if (s != nearest)
            if (const double d = Distance2 (&positions[3 * s], snapped); d <= limit)
                within.emplace_back (d, s);
    std::sort (within.begin (), within.end ());
    std::vector<size_t> samples { nearest };
    for (const auto& [distance, sample] : within) {
        if (samples.size () >= cap)
            break;
        samples.push_back (sample);
    }
    return samples;
}

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
        auto& stepFacing = profile.dayStepFacing.emplace_back ();
        auto& stepLit = profile.dayStepLit.emplace_back ();
        stepFacing.reserve (day.steps.size ());
        stepLit.reserve (day.steps.size ());
        for (const SunStep& step : day.steps) {
            const int hour = std::clamp (step.time.hour, 0, 23);
            const double weighted = day.weightDays * day.hoursPerStep;
            profile.up[month][hour] += weighted;
            up += day.hoursPerStep;
            const bool faces = Faces (normal, step.direction);
            stepFacing.push_back (faces ? 1u : 0u);
            bool visible = false;
            if (faces) {
                profile.facing[month][hour] += weighted;
                facing += day.hoursPerStep;
                visible = next < occluded.size () && occluded[next] == 0;
                if (visible) {
                    profile.lit[month][hour] += weighted;
                    lit += day.hoursPerStep;
                }
                ++next;
            }
            stepLit.push_back (visible ? 1u : 0u);
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
