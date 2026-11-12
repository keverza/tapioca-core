#ifndef TAPIOCA_SUNSTUDY_SOLAREXPOSURE_HPP
#define TAPIOCA_SUNSTUDY_SOLAREXPOSURE_HPP

#include "SunStudy/SunStudySolarCut.hpp"

#include <array>

namespace evp::sunstudy {

// Geometry-only shadow difference at the measured suns, not an interpolation of
// sample answers. Real holes (a remaining blocker or a receiver opening) stay.
struct SolarExposureStep {
    int minute = 0;
    std::vector<SolarCutTriangle> triangles;
    std::vector<std::array<double, 6>> boundary; // this step's own perimeter
};

// Where the cut wins sun during [from, to), clock minutes. Between two measured
// steps the won region is SWEPT, not sampled: the sun moves continuously, and
// a thin won strip that jumps between steps lights the ground it passes over.
// Sampled alone, the slice drew a comb of separate strips (#168).
struct SolarExposureSpan {
    double from = 0.0, to = 0.0;
    std::vector<SolarCutTriangle> triangles;
};

// Where the cut gains more than (level - 1) and at most `level` band widths
// of sun (SolarExposureResult::bandMinutes). The bands partition the union,
// so a fill shaded by level reads as hours gained without overlap.
struct SolarExposureBand {
    int level = 0;
    std::vector<SolarCutTriangle> triangles;
};

struct SolarExposureResult {
    std::vector<SolarCutTriangle> triangles; // union over the slice, swept between steps
    std::vector<std::array<double, 6>> boundary;
    std::vector<SolarExposureStep> steps; // each measured step's exact won region, with its time
    std::vector<SolarExposureSpan> spans; // the swept regions with the time each stands for
    std::vector<SolarExposureBand> bands; // the union split by minutes gained
    double bandMinutes = 0.0;             // the width of one band
    size_t work = 0;                      // work units used, of the call's limit
    std::string error;

    int MostLevel () const
    {
        int most = 0;
        for (const auto& band : bands)
            most = band.level > most ? band.level : most;
        return most;
    }
};

// receivers are global source-triangle IDs of the measured surfaces. The
// unchanged snapshot plus cut.kept is the after scene. Work is bounded and a
// refusal/cancellation returns no partial exposure, never a coarser substitute.
bool ComputeSolarExposure (const geomsrv::Snapshot& snapshot, const SolarCutResult& cut,
                           const std::vector<uint8_t>& blocks, const std::vector<uint32_t>& receivers,
                           const std::vector<SunStep>& steps, SolarExposureResult& result,
                           const std::function<bool ()>& cancelled = {}, size_t workLimit = 20000000,
                           const std::function<void (size_t done, size_t of)>& progress = {});

// X-ray picking: nearest won surface along the cursor ray, earliest won time
// there. Hidden receiver geometry is intentionally inspectable.
int ReadSolarExposure (const SolarExposureResult& result, const double origin[3], const double direction[3]);

// When the cut lights that same nearest surface point: its spans merged into
// ascending [from, to) clock-minute runs. Their total length is the gain.
std::vector<std::array<double, 2>> ReadSolarExposureRuns (const SolarExposureResult& result, const double origin[3],
                                                          const double direction[3]);

} // namespace evp::sunstudy

#endif
