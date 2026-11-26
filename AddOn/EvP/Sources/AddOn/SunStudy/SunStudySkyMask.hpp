#ifndef EVP_SUNSTUDY_SUNSTUDYSKYMASK_HPP
#define EVP_SUNSTUDY_SUNSTUDYSKYMASK_HPP

// SunStudy/SunStudySkyMask -- the share of every sky patch each sample sees,
// and the beam's path for one sun direction (#155 P0/F2, F3).
//
// Each patch is split into cells (rust::SkyCells); one ray per cell is cast
// from the sample lifted off its surface, and the patch's share is the open
// cells' projected weight max(0, n . moment) over the whole patch's. A single
// ray per patch would misread every patch an obstruction's edge cuts, and that
// error does not shrink with a finer dome: on #155 P0's overhang it is -4 % on
// Reinhart 577 with one ray, under 1 % with 4 x 4 cells. Cells with no weight
// on the sample's plane cast no ray.
//
// The mask is weather-free and traced once per scene, then integrated against
// any sky (SunStudy/SunStudyRustClimate). Every hit is opaque here: the
// traversal knows no canopy class yet, so no share is ever through canopy.

#include "SunStudy/SunStudyRustClimate.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace evp::sunstudy {

class ITraversal;

struct SkyMaskOptions {
    double normalOffset = 0.02; // m: origins lifted along the normal
    double maxDistance = 1.0e5; // m: ray length
    size_t raysPerBatch = 1u << 20;
    size_t maxParallel = 0; // 0: the traversal's default
};

// `positions` and `normals` hold 3 doubles per sample (unit normals); `cells`
// come from rust::SkyCells for a dome of `patchCount` patches. On success
// `open` holds, per sample then per patch, the share seen open (0-255). False
// with "Cancelled" when `cancelled` answers true between batches; `open` is
// then empty.
bool TraceSkyMasks (const ITraversal& traversal, const std::vector<double>& positions,
                    const std::vector<double>& normals, const std::vector<rust::SkyCell>& cells, size_t patchCount,
                    const SkyMaskOptions& options, const std::function<bool ()>& cancelled, std::vector<uint8_t>& open,
                    std::string& error);

// The beam's path per sample for one unit `sun` direction: Open, or Blocked
// when the sun is behind the sample's plane or something is in the way.
bool TraceBeam (const ITraversal& traversal, const std::vector<double>& positions, const std::vector<double>& normals,
                const std::array<double, 3>& sun, const SkyMaskOptions& options, std::vector<uint8_t>& paths,
                std::string& error);

} // namespace evp::sunstudy

#endif
