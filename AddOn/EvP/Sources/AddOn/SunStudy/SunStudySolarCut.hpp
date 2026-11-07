#ifndef EVP_SUNSTUDY_SUNSTUDYSOLARCUT_HPP
#define EVP_SUNSTUDY_SUNSTUDYSOLARCUT_HPP

// SunStudy/SunStudySolarCut -- how much of what blocks a study point must go
// for it to have direct sun through a chosen time slice (#152 follow-up).
//
// The point's rays toward the sun across the slice sweep a ruled surface, the
// SOLAR ENVELOPE: at horizontal distance d and azimuth a from the point, the
// sun ray of that azimuth is at height p.z + d * tan(altitude(a)). Material on
// that surface blocks. Two removals answer "how much must go":
//
//   RAY VOXELS    -- only the voxels a slice ray passes through: the least that
//                    clears the sun, a slot through the blocker. The lower
//                    bound, rarely buildable.
//   ENVELOPE CUT  -- every voxel above the envelope: the blocker sliced along
//                    the surface and its top removed, the form a solid
//                    building has to take. The buildable answer.
//
// Both come from one voxelisation of each blocking element (scanline parity
// per voxel row; an open mesh keeps only its surface voxels), so the two
// volumes can be compared directly. Voxels are tested conservatively: a voxel
// any slice ray may touch counts, so the result clears the sun at the voxel's
// resolution, never short of it.
//
// Pure CPU geometry on an immutable snapshot; cancellable; no host, no store.

#include "Geometry/Mesh.hpp"
#include "SunStudy/SunSeries.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace evp::sunstudy {

struct SolarCutInput {
    double point[3] = {};
    // The receiving surface's normal: steps with the sun behind it are left
    // out (no removal can light it then). Null for a point in the air.
    const double* normal = nullptr;
    std::vector<SunStep> steps; // the slice's measured suns, in time order
    double hoursPerStep = 0.25;
    // One per snapshot mesh: whether it can block (analysis or context, not
    // ignored), and the mesh the point stands on, which never blocks itself.
    std::vector<uint8_t> blocks;
    size_t ownMesh = SIZE_MAX;
    double voxel = 0.5;              // metres; raised per element to stay under the cap
    size_t voxelCapPerMesh = 250000; // inside voxels one element may have
};

struct SolarCutVoxel {
    float centre[3] = {};
    float size = 0.0f;
    uint32_t mesh = 0;
    int32_t index[3] = {}; // in its element's grid, for face sharing between neighbours
    bool ray = false;      // a ray voxel; every voxel listed is in the envelope cut
};

struct SolarCutObject {
    size_t mesh = 0;
    std::string guid;
    int32_t elemType = 0;
    double voxel = 0.0;
    size_t voxels = 0;         // the element's voxels in all
    size_t envelopeVoxels = 0; // above the envelope
    size_t rayVoxels = 0;      // on a slice ray
};

struct SolarCutResult {
    std::vector<SolarCutVoxel> removed;           // envelope cut; `ray` marks the subset
    std::vector<SolarCutObject> objects;          // elements with something to remove, most first
    double envelopeVolume = 0.0, rayVolume = 0.0; // cubic metres
    // The slice at the point: hours the sun is in front of it, and of those,
    // hours already lit (a clear ray now). The cut lights the rest.
    double facingHours = 0.0, litHours = 0.0;
    std::vector<SunStep> facingSteps; // the suns the envelope was built from
    std::string error;
};

// Envelope height limit (relative to the point) at horizontal distance `d` for
// the slice's suns within azimuth range [a0, a1] radians: d * min tan(alt).
// False when no slice sun covers that azimuth range.
bool SolarEnvelopeTan (const std::vector<SunStep>& steps, double a0, double a1, double& tanMin, double& tanMax);

// The two removals. `lit` answers whether a sun direction is already clear
// from the point (the study's occluders); null counts nothing as lit.
bool ComputeSolarCut (const geomsrv::Snapshot& snapshot, const SolarCutInput& input, SolarCutResult& result,
                      const std::function<bool (const double direction[3])>& lit = {},
                      const std::function<bool ()>& cancelled = {});

} // namespace evp::sunstudy

#endif
