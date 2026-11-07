#ifndef EVP_SUNSTUDY_SUNSTUDYOBSTRUCTION_HPP
#define EVP_SUNSTUDY_SUNSTUDYOBSTRUCTION_HPP

// Exact, nearest-first obstruction attribution for selected study-point cells.
// This is deliberately separate from ITraversal's hot Boolean contract: only an
// on-demand query pays for hit identity. Pure CPU geometry; no host or store lock.

#include "SunStudy/SunSeries.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace evp::sunstudy {

struct StudyRecord;

enum class SunObstructionState {
    Unavailable,
    Lit,
    Blocked,
    BackFacing,
};

// One piece of a cell's shadow footprint on its blocker (#168): a sub-cell of
// the receiving cell, cast along the sun onto the plane of the face its own ray
// hit. Corners in world space, in order. The footprint is a property of the
// cell and the sun, so it does not grow or shrink with the blocker's
// tessellation the way a whole hit triangle did.
struct SunObstructionFragment {
    double corners[4][3] = {};
    std::string blockerGuid;
    bool context = false;
};

struct SunObstructionCell {
    size_t sample = 0;
    SunObstructionState state = SunObstructionState::Unavailable;
    std::string blockerGuid;
    size_t sourceTriangle = 0; // mesh-local triangle in blockerGuid's source mesh
    bool context = false;
    double distance = 0.0;                         // from the sample, metres
    double origin[3] = {};                         // the receiving sample
    double hit[3] = {};                            // Blocked: where the cell's own ray met the blocker
    std::vector<SunObstructionFragment> fragments; // the cell's shadow footprint, any state facing the sun
};

struct SunObstructionResult {
    int minute = 0;                        // clock minute of the measured sun step
    std::vector<SunObstructionCell> cells; // aligned with the requested sample order
    std::string error;
};

struct SunObstructionDayResult {
    std::vector<SunObstructionResult> steps; // every measured step, with its own exact timed hits
    std::string error;
};

// Applies the day engine's face cull, then finds the nearer of the analysis and
// context hits. False refuses the whole query; a clear ray is a successful Lit
// cell. Custom ray bounds are refused because StudyRecord does not retain them.
//
// `footprintDivisions` splits each facing cell into N x N sub-cells and casts
// one more ray per sub-cell for the footprint (0: none). The cell's state and
// blocker stay those of its own centre ray -- the study's answer -- while the
// footprint may show a partial shadow on a lit cell or a second blocker.
bool AttributeSunObstruction (const StudyRecord& record, const std::vector<size_t>& samples, const SunStep& sun,
                              SunObstructionResult& result, int footprintDivisions = 0);

// The footprint resolution a selection can afford: finer for a few cells.
int SunObstructionFootprintDivisions (size_t samples);

// Whole-day attribution retains every timed hit rather than averaging away a
// part's multiple shadow times. Cancellation returns false with an empty error
// and no partial result, matching the point-profile worker convention.
// Only steps whose clock minute lies in [fromMinute, toMinute) are traced, so a
// time slice selected on the day diagram is the slice attributed.
bool AttributeSunObstructionDay (const StudyRecord& record, const std::vector<size_t>& samples,
                                 SunObstructionDayResult& result, const std::function<bool ()>& cancelled = {},
                                 int footprintDivisions = 0, int fromMinute = 0, int toMinute = 1440);

} // namespace evp::sunstudy

#endif
