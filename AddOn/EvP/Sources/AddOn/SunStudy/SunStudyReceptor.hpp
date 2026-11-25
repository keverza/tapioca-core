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

#include "SunStudy/SunStudyAtlas.hpp"
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

// Unavailable plan area of one host (a grid group: the snapshot mesh), split by
// why (#278 area table). Never shade: it is reported beside the classes.
struct ReceptorHostLoss {
    uint32_t group = 0;
    double steepArea = 0.0;   // ReceptorStatus::SteepHost
    double blockedArea = 0.0; // ReceptorStatus::Blocked
    double invalidArea = 0.0; // ReceptorStatus::InvalidHost
};

// Per group, ascending, of `layer`'s non-available receptors, read from the grid
// the layer was built from (before KeepAvailableReceptors). Empty when the layer
// does not belong to the grid.
std::vector<ReceptorHostLoss> UnavailableByGroup (const SampleGrid& grid, const ReceptorLayer& layer);

// ⚠️ THE SECOND SENTINEL. An atlas texel below zero has no sample; one at
// kUnavailableTexel is a receptor that exists but has no answer (steep host,
// blocked overhead). The tint shader draws it as a neutral hatch rather than
// discarding it, so "unavailable" is visible and never reads as shade or as
// "not analysed". Mirrored in kArchVizSunTintPS (DiligentShaders.hpp).
constexpr float kUnavailableTexel = -2.0f;

// One unavailable receptor's cell, in its host face's lattice.
struct ReceptorCell {
    uint32_t face = 0;
    uint32_t column = 0;
    uint32_t row = 0;
};

// The cells of `layer`'s non-available receptors, read from the grid the layer
// was built from (before KeepAvailableReceptors). Empty without cell coordinates.
std::vector<ReceptorCell> UnavailableReceptorCells (const SampleGrid& grid, const ReceptorLayer& layer);

// Atlas texel of each cell whose face has a placed tile (the same arithmetic
// as the atlas's own sample texels); cells on unplaced faces are skipped.
std::vector<uint32_t> ReceptorCellTexels (const SunStudyAtlas& atlas, const std::vector<ReceptorCell>& cells);

// Writes kUnavailableTexel into `image` at `texels` that hold no sample (< 0).
// A measured value is never overwritten. Returns the texels written.
size_t StampUnavailableTexels (std::vector<float>& image, const std::vector<uint32_t>& texels);

} // namespace evp::sunstudy

#endif
