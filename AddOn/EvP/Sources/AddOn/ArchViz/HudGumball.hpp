#ifndef EVP_ARCHVIZ_HUDGUMBALL_HPP
#define EVP_ARCHVIZ_HUDGUMBALL_HPP

// The viewer's screen-space translation gumball: three axis arrows and a centre
// disc projected from a world point, hit-tested and drawn with ImGui. The drag
// itself is world-space (ArchViz/PointGumball.hpp) -- this is only what the
// person sees and grabs.

#include <imgui.h>

#include <cstdint>

namespace geomsrv::archviz {

struct Gumball {
    ImVec2 center, ends[3];
    bool visible[3] = {};
    bool valid = false;
};

// The arrows are ~70 px long whatever the zoom; an arrow seen end-on is hidden.
Gumball MakeGumball (const double point[3], const float viewProj[16], uint32_t width, uint32_t height);
// 0..2 an axis arrow, 3 the centre disc, -1 nothing.
int HitGumball (const Gumball& gizmo, float x, float y);
// `hot` is the part under the cursor or being dragged, numbered as HitGumball
// numbers it (-1 none): drawn lighter and heavier, so it reads as grabbable.
void DrawGumball (const Gumball& gizmo, ImDrawList* draw, int hot = -1);

} // namespace geomsrv::archviz

#endif
