#ifndef EVP_SUNSTUDY_SUNSTUDYPOINTPROFILE_HPP
#define EVP_SUNSTUDY_SUNSTUDYPOINTPROFILE_HPP

// SunStudy/SunStudyPointProfile -- one point's year, on demand (#152).
//
// A period keeps per-sample monthly totals, not when in a day a point was lit.
// For the point under the cursor that is cheap to recompute: one ray per traced
// sun position (365 days x ~60 steps is ~22k rays), against the period's own
// traversal and sun directions, so it measures the same scene. The result is a
// month x clock-hour grid of lit, facing and sun-up hours (weighted as the
// period's days are) and one line per traced day.
//
// Pure: no host, no store. The caller owns the traversal's thread contract.

#include "SunStudy/ITraversal.hpp"
#include "SunStudy/SunSeries.hpp"

#include <array>
#include <functional>
#include <vector>

namespace evp::sunstudy {

// One traced day of the period, as it was traced.
struct ProfileDay {
    int month = 1, day = 1;
    double weightDays = 1.0; // calendar days it stands for
    double hoursPerStep = 0.25;
    std::vector<SunStep> steps; // above the floor, inside the time slice
};

struct PointProfile {
    // Weighted hours by month (0-11) and clock hour (0-23): sun above the
    // floor, sun in front of the surface, and a clear ray to it.
    using Grid = std::array<std::array<double, 24>, 12>;
    Grid up {}, facing {}, lit {};
    // One entry per traced day, in the period's order: hours that day.
    std::vector<int> dayMonth, dayOfMonth;
    std::vector<double> dayUp, dayFacing, dayLit;
    size_t rays = 0;
};

// The day engine's cull: incidence > 0, NaN traced; `normal` null faces all.
bool FacesSun (const double* normal, const double sun[3]);

// A study point (#152): the sample nearest `centre` -- the point snapped to
// the grid, its position in `snapped` -- then every other sample within
// `radius` of THAT position, nearest first, at most `cap` in all. Empty when
// there are no samples.
std::vector<size_t> SamplesInSphere (const std::vector<double>& positions, const double centre[3], double radius,
                                     size_t cap, double snapped[3]);

// One day at a study point, averaged over its samples: per kept step, the
// share of the samples lit and the share the sun is in front of.
struct PointDay {
    std::vector<uint16_t> minutes; // clock minute of each step
    std::vector<double> lit, facing;
    double hoursPerStep = 0.25;
    size_t samples = 0;
};

// `normal` null means always facing (a ground sample). False when cancelled.
// The cull is the day engine's: incidence > 0, NaN traced.
bool ComputePointProfile (const double position[3], const double* normal, const std::vector<ProfileDay>& days,
                          const ITraversal& traversal, double tmin, PointProfile& profile,
                          const std::function<bool ()>& cancelled = {});

} // namespace evp::sunstudy

#endif
