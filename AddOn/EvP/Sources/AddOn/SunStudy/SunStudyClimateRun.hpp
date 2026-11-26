#ifndef EVP_SUNSTUDY_SUNSTUDYCLIMATERUN_HPP
#define EVP_SUNSTUDY_SUNSTUDYCLIMATERUN_HPP

// SunStudy/SunStudyClimateRun -- one sample set's climate run (#155 F3, #278
// Stage 5): the sky masks once, then every weather interval's beam, into the
// climate accumulator; per sample and month the direct, diffuse and reflected
// energy (Wh/m2), the daily light integral, and the geometric sky view factor
// beside them.
//
// ⚠️ UNAVAILABLE IS NOT DARK. A sample marked unavailable (a planting layer's
// steep or blocked receptor) is neither traced nor accumulated; its values
// come back NaN and it is counted as excluded.
//
// ⚠️ SAY WHOSE SUN IT IS. Each interval carries a sun direction. From the host
// (Archicad's place settings, #155 P2) the result is an analysis; from the
// independent formula it is a preview and `sunSource` says so -- a caller
// never shows it as the analysis.
//
// ⚠️ CANCEL PUBLISHES NOTHING. Cancelled or failed, `result` is left as it was.
//
// Blocking only: the caller runs it off the main thread.

#include "SunStudy/SunStudyRustClimate.hpp"
#include "SunStudy/SunStudySkyMask.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace evp::sunstudy {

class ITraversal;

enum class SunSource : uint8_t {
    Host = 0,        // Archicad's sun: the analysis
    Independent = 1, // the cross-check formula: a preview only
};

struct ClimateRunRequest {
    std::vector<double> positions, normals; // 3 per sample
    std::vector<uint8_t> available;         // 1 traced, 0 excluded; empty: all traced
    std::vector<rust::ClimateInterval> intervals;
    SunSource sunSource = SunSource::Host;
    rust::SkyPatchSet set = rust::SkyPatchSet::Tregenza;
    double rotation = 0.0; // as rust::SkyPatches: the model frame's turn from true north
    rust::DiffuseSky sky = rust::DiffuseSky::Perez1993;
    std::array<double, 12> canopyTransmittance {};
    double luxThreshold = 0.0;    // lux; 0 or less: no illuminance (#155 P4)
    bool luxEfficacyOnly = false; // else the file's illuminance when valid
    uint32_t cellsPerSide = 4;
    SkyMaskOptions mask;
    rust::ParConversion par;
};

struct ClimateRunResult {
    // Per sample of the request, January first; NaN for an excluded sample.
    std::vector<rust::Monthly> direct, diffuse, reflected, dli;
    // Exterior illuminance when asked: lux-hours and hours at or above the
    // threshold per sample and month, NaN for an excluded sample; else empty.
    std::vector<rust::Monthly> luxHours, hoursAbove;
    std::vector<double> skyView; // geometric, open shares only
    rust::ClimateReport report;  // over the traced samples
    SunSource sunSource = SunSource::Host;
    rust::SkyPatchSet set = rust::SkyPatchSet::Tregenza;
    uint32_t cellsPerSide = 0;
    size_t traced = 0, excluded = 0;
    size_t beamPasses = 0; // intervals whose beam was traced
};

// Called after the masks (done 0) and after each interval.
using ClimateProgress = std::function<void (size_t done, size_t total)>;

bool RunClimate (const ITraversal& traversal, const ClimateRunRequest& request, const std::function<bool ()>& cancelled,
                 const ClimateProgress& progress, ClimateRunResult& result, std::string& error);

// Area-weighted mean of one month's values over `areas` (m2), skipping NaN
// samples; NaN when nothing with area remains. `month` 0 = January.
double AreaWeightedMean (const std::vector<rust::Monthly>& values, const std::vector<double>& areas, int month);

} // namespace evp::sunstudy

#endif
