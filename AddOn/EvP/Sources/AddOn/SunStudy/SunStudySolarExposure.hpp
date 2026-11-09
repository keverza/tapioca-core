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
};

struct SolarExposureResult {
    std::vector<SolarCutTriangle> triangles; // union over the requested steps
    std::vector<std::array<double, 6>> boundary;
    std::vector<SolarExposureStep> steps; // same geometry, with its actual time
    std::string error;
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

} // namespace evp::sunstudy

#endif
