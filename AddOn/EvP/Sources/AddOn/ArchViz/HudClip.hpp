#ifndef EVP_ARCHVIZ_HUDCLIP_HPP
#define EVP_ARCHVIZ_HUDCLIP_HPP

// ArchViz/HudClip -- a HUD triangle cut to the rectangle ImGui drew it under.
//
// ⚠️ THE OVERLAYS DRAW IMGUI'S TRIANGLES WITHOUT A SCISSOR (OverlayHud.hpp: laid out into
// plain triangles, drawn by the guests as glyph quads). ImGui leaves the rest of its clipping
// to the renderer's scissor: a glyph half past the edge of a scrolled page, a row scrolled
// out of it, are still in the draw list. While no window scrolled, nothing reached past its
// window and the missing scissor never showed; a page that scrolls (the user, 2026-10-03: a
// vertical scroll bar, the panel never taller than the view) would spill over its own tab
// row and below the panel. So the engine cuts every triangle to its command's rectangle on
// the CPU, as a scissor would: the part inside, its uv and colour carried exactly -- both are
// affine across a 2D triangle.
//
// Pure: tests/cpp builds it.

#include <cstdint>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace hudclip {

// A corner: view pixels, the atlas uv, and the colour packed as ImGui packs it (any byte
// order -- each byte is carried on its own).
struct Corner {
    float x = 0.0f, y = 0.0f;
    float u = 0.0f, v = 0.0f;
    uint32_t col = 0;
};

// The rectangle [left, right] x [top, bottom], view pixels.
struct Rect {
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
};

// The part of triangle a-b-c inside `clip`, appended to `out` as whole triangles in a fan,
// wound as the triangle was: the triangle itself when it lies inside, nothing when it lies
// outside or `clip` is empty. Returns how many triangles it appended.
uint32_t Clip (const Corner& a, const Corner& b, const Corner& c, const Rect& clip, std::vector<Corner>& out);

} // namespace hudclip
} // namespace archviz
} // namespace geomsrv

#endif
