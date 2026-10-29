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
    // The XY plane square between the X and Y arrows, when asked for and both
    // arrows show: its four screen corners.
    bool plane = false;
    ImVec2 square[4];
};

// HitGumball's parts beyond the three arrows.
constexpr int kGumballCentre = 3;
constexpr int kGumballPlane = 4;

// The arrows are ~70 px long whatever the zoom; an arrow seen end-on is hidden.
// `plane` adds the XY square.
Gumball MakeGumball (const double point[3], const float viewProj[16], uint32_t width, uint32_t height,
                     bool plane = false);
// 0..2 an axis arrow, 3 the centre disc, 4 the plane square, -1 nothing.
int HitGumball (const Gumball& gizmo, float x, float y);
// `hot` is the part under the cursor or being dragged, numbered as HitGumball
// numbers it (-1 none): drawn lighter and heavier, so it reads as grabbable.
// `centre` tells handles apart: the view point is yellow, a target another hue.
void DrawGumball (const Gumball& gizmo, ImDrawList* draw, int hot = -1, ImU32 centre = IM_COL32 (245, 196, 79, 255));
// A handle that is not the selected one: its centre alone, a click target.
void DrawGumballMarker (ImVec2 at, ImDrawList* draw, bool hot, ImU32 centre);
// How near the cursor must be to a marker to pick it, in pixels.
constexpr float kGumballMarkerPickPixels = 9.0f;

} // namespace geomsrv::archviz

#endif
