#ifndef EVP_SUNSTUDY_SUNSTUDYRECEPTOR_HPP
#define EVP_SUNSTUDY_SUNSTUDYRECEPTOR_HPP

// SunStudy/SunStudyReceptor -- a planting height layer over a sampled grid (#278).
//
// "How much sun does this bed get" and "how much sun reaches foliage 1.5 m above
// this bed" are different questions. The sampler answers the first: each sample
// sits a hair above its face along the face normal. A receptor layer answers the
// second: every sample lifted STRAIGHT UP from its host point by the layer
// height, facing +Z, weighted by its plan area. The kernel is the Rust
// `receptor` module; this is the C++ value type and the host's overhead pass.
//
// ⚠️ SAMPLE ORDER IS UNCHANGED. Face, cell column/row, group and layout still
// address the host, so the existing atlas draws the layer -- as a plan
// projection on the host surface, not a surface at canopy height.
//
// ⚠️ UNAVAILABLE IS NOT DARK. A host too steep to plant on, or a receptor that
// would sit inside or above something overhead, has no answer. It must be
// excluded from hours, classes and area denominators, never traced as open sky
// and never reported as zero hours.
//
// ⚠️ A FIXED LAYER DOES NOT CLAMP. The placed study point (SunStudyLiftedPoint)
// lowers itself under an overhang and says so; a layer that did the same would
// mix heights inside one "3.00 m" map. A blocked receptor is marked Blocked.

#include "SunStudy/SunStudySampler.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace evp::sunstudy {

// Mirrors Rust `receptor::ReceptorStatus` (C ABI values).
enum class ReceptorStatus : uint8_t {
    Available = 0,
    SteepHost = 1,   // host normal Z below minHostUp, or facing down
    Blocked = 2,     // something overhead below height + clearance
    InvalidHost = 3, // non-finite host sample
};

constexpr double kReceptorDefaultMinHostUp = 0.5; // a 60 degree slope

struct ReceptorOptions {
    double height = 0.0;  // metres above the host, 0..100
    bool faceUp = true;   // +Z receptor; false keeps the host normal
    bool planArea = true; // area * |n_z|; false keeps the surface area
    double minHostUp = kReceptorDefaultMinHostUp;
};

// Overhead distance from `host + 0.001 m` straight up, or < 0 for nothing
// overhead. The host surface itself must not count.
using OverheadDistance = std::function<double (const double host[3])>;

// One layer, parallel to the source grid.
struct ReceptorLayer {
    ReceptorOptions options;
    std::vector<double> hosts;     // xyz host points (normal offset removed)
    std::vector<double> positions; // xyz receptors
    std::vector<double> normals;   // xyz
    std::vector<double> areas;     // m2 weights (plan or surface per options)
    std::vector<ReceptorStatus> status;
    size_t available = 0, steepHost = 0, blocked = 0, invalidHost = 0;
    double availableArea = 0.0, unavailableArea = 0.0;

    size_t Count () const
    {
        return status.size ();
    }
};

// Builds a layer from `grid` (sampled with `normalOffset`). With `overhead`, the
// host's occluders are queried once per receptor that is otherwise available,
// and those blocked are marked. False, with `error`, on a refused request; the
// output is then empty.
bool BuildReceptorLayer (const SampleGrid& grid, double normalOffset, const ReceptorOptions& options,
                         const OverheadDistance* overhead, ReceptorLayer& out, std::string& error);

// Replaces `grid`'s samples with the layer's AVAILABLE receptors: positions,
// normals and area weights from the layer; faces, groups and cell coordinates
// filtered in step; layouts untouched, so the atlas still maps onto the host and
// an unavailable cell keeps no texel. False (grid unchanged) when the layer does
// not belong to the grid.
bool KeepAvailableReceptors (SampleGrid& grid, const ReceptorLayer& layer);

} // namespace evp::sunstudy

#endif
