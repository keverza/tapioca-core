#ifndef EVP_ARCHVIZ_OVERLAYHOVER_HPP
#define EVP_ARCHVIZ_OVERLAYHOVER_HPP

// ArchViz/OverlayHover -- hover mode's pick (the user's stage 3): which item of the layers
// is under the pointer, and what the HUD says of it (OverlayHud.hpp `Hover`) -- a storey
// slice's figures, a heatmap's value.
//
// ⚠️ PICKED ON THE SIDE THAT HAS THE TRANSFORM (HANDOFF-OverlayHud D13). The plan's is read
// on the main thread at its Present (finding 14), so the plan picks here, on the CPU, in
// model metres. ⚠️ AND SO DOES 3D (D19): the census already decodes the selected
// camera's `b1`/`b0` on the CPU after every capture (`freshness::LatestCamera`), so the 3D
// view projects its meshes through the matrix the shaders draw with and picks the nearest
// -- no pass of its own on the render thread.
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

// The hit mesh's triangles -- or only triangle `only` -- in model metres, appended to `out`
// as x, y, z for each corner (OverlayHud.hpp `Hover::tintModel`). A slice is tinted whole;
// a heatmap, the cell under the pointer. Both views: their renderers draw it with their own
// camera or transform, so the tint stays on the item as the view moves.
void TintModel (const overlaylayers::Mesh& mesh, std::vector<double>& out, int64_t only = -1);

// ---- the 3D view (D19) ------------------------------------------------------------------

// A model point to view pixels through a camera; `invW` is 1/w -- larger is nearer.
// False behind the eye, where the point has no place on the view.
using ProjectView = std::function<bool (double x, double y, double z, float& px, float& py, float& invW)>;

// ⚠️ THE 3D VIEW PICKS THE NEAREST, NOT THE LAST DRAWN: the triangle of a mesh drawn in 3D
// that says something, whose projection covers the view pixel (x, y), nearest the eye at
// that pixel. A triangle with a corner behind the eye is passed over. The value is
// interpolated perspective-correctly -- what the surface holds at the point, not on the
// screen. ⚠️ THE BUILDING DOES NOT HIDE WHAT IS PICKED: what it hides is drawn faded
// (`Behind::Fade`, the slices' default) or dashed, still seen, so still read.
Hit PickView (const std::vector<std::shared_ptr<const overlaylayers::Layer>>& layers, const ProjectView& project,
              float x, float y);

// The projection itself: clip = [x y z 1] * viewProjection (row vectors, as the shaders), and
// the census's screen convention (InjectionOracle): px = vx + (ndcX / 2 + 1/2) * width,
// py = vy + (1/2 - ndcY / 2) * height. `viewport` is x, y, width, height.
bool ProjectThrough (const double viewProjection[16], const float viewport[4], double x, double y, double z, float& px,
                     float& py, float& invW);

} // namespace overlayhover
} // namespace archviz
} // namespace geomsrv

#endif
