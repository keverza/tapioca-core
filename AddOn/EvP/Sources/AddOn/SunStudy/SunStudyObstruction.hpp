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

struct SunObstructionCell {
    size_t sample = 0;
    SunObstructionState state = SunObstructionState::Unavailable;
    std::string blockerGuid;
    size_t sourceTriangle = 0; // mesh-local triangle in blockerGuid's source mesh
    bool context = false;
    double distance = 0.0; // from the sample, metres
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
bool AttributeSunObstruction (const StudyRecord& record, const std::vector<size_t>& samples, const SunStep& sun,
                              SunObstructionResult& result);

// Whole-day attribution retains every timed hit rather than averaging away a
// part's multiple shadow times. Cancellation returns false with an empty error
// and no partial result, matching the point-profile worker convention.
bool AttributeSunObstructionDay (const StudyRecord& record, const std::vector<size_t>& samples,
                                 SunObstructionDayResult& result, const std::function<bool ()>& cancelled = {});

} // namespace evp::sunstudy

#endif
