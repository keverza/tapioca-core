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

// Where the cut wins sun on exactly `steps` measured steps. The bands of a
// result partition its union: each point lies in one band, by how many steps
// won it, so a fill shaded by `steps` reads as hours gained without overlap.
struct SolarExposureBand {
    int steps = 0;
    std::vector<SolarCutTriangle> triangles;
};

struct SolarExposureResult {
    std::vector<SolarCutTriangle> triangles; // union over the requested steps
    std::vector<std::array<double, 6>> boundary;
    std::vector<SolarExposureStep> steps; // same geometry, with its actual time
    std::vector<SolarExposureBand> bands; // the union split by steps won, fewest first per surface
    std::string error;

    int MostSteps () const
    {
        int most = 0;
        for (const auto& band : bands)
            most = band.steps > most ? band.steps : most;
        return most;
    }
};

// receivers are global source-triangle IDs of the measured surfaces. The
// unchanged snapshot plus cut.kept is the after scene. Work is bounded and a
// refusal/cancellation returns no partial exposure, never a coarser substitute.
bool ComputeSolarExposure (const geomsrv::Snapshot& snapshot, const SolarCutResult& cut,
                           const std::vector<uint8_t>& blocks, const std::vector<uint32_t>& receivers,
                           const std::vector<SunStep>& steps, SolarExposureResult& result,
                           const std::function<bool ()>& cancelled = {}, size_t workLimit = 20000000);

// X-ray picking: nearest won surface along the cursor ray, earliest won time
// there. Hidden receiver geometry is intentionally inspectable.
int ReadSolarExposure (const SolarExposureResult& result, const double origin[3], const double direction[3]);

// Every step won at that same nearest surface point, ascending minutes. The
// hours gained there are their count times the study's timestep.
std::vector<int> ReadSolarExposureSteps (const SolarExposureResult& result, const double origin[3],
                                         const double direction[3]);

} // namespace evp::sunstudy

#endif
