#ifndef EVP_ARCHVIZ_OVERLAYHOVER_HPP
#define EVP_ARCHVIZ_OVERLAYHOVER_HPP

// ArchViz/OverlayHover -- hover mode's pick (the user's stage 3): which item of the layers
// is under the pointer, and what the HUD says of it (OverlayHud.hpp `Hover`) -- a storey
// slice's figures, a heatmap's value.
//
// ⚠️ PICKED ON THE SIDE THAT HAS THE TRANSFORM (HANDOFF-OverlayHud D13). The plan's is read
// on the main thread at its Present (finding 14), so the plan picks here, on the CPU, in
// model metres; the 3D camera is the GPU's (finding 1), and the 3D view picks there.
//
// ⚠️ ONLY WHAT SAYS SOMETHING IS PICKED: a mesh with hover text (`Mesh::hoverTitle`,
// `hoverRows`) or with values. Anything else -- a caller's plain fill -- is passed over, and
// what is under it may be found.
//
// Pure -- no DevKit, no ImGui -- so tests/cpp builds it.

#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayLayers.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace overlayhover {

struct Hit {
    bool found = false;
    size_t layer = 0; // in the list picked from
    size_t mesh = 0;  // in that layer's meshes
    uint32_t triangle = 0;
    bool hasValue = false;
    double value = 0.0; // the mesh's values at the point, between its triangle's corners
};

// The plan: the topmost mesh drawn in 2D whose triangle covers the model point (x, y) -- the
// layer drawn last, and in it the mesh drawn last, as on screen. `layers` are the ones shown.
Hit PickPlan (const std::vector<std::shared_ptr<const overlaylayers::Layer>>& layers, double x, double y);

// What the HUD says of a hit: the mesh's title (its layer's name without one) and rows; with
// values, the value at the point first, in the decimals and unit of the layer's first legend.
overlayhud::Hover Readout (const overlaylayers::Layer& layer, const Hit& hit);

// The hit mesh's triangles -- or only triangle `only` -- each corner through `project`
// (model x, y to view pixels), appended to `out` as x, y pairs. A slice is tinted whole; a
// heatmap, the cell under the pointer.
using Project = std::function<void (double x, double y, float& px, float& py)>;
void Tint (const overlaylayers::Mesh& mesh, const Project& project, std::vector<float>& out, int64_t only = -1);

} // namespace overlayhover
} // namespace archviz
} // namespace geomsrv

#endif
