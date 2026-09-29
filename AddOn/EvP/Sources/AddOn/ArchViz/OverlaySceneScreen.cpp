// ArchViz/OverlaySceneScreen -- what the builder puts FIXED TO THE VIEW: legends and their
// ticks, and the text that labels them. Anchored to a fraction of the view and moved in
// pixels by the vertex shader, never projected. See OverlayScene.hpp and
// OverlaySceneBuilder.hpp.

#include "ArchViz/OverlaySceneBuilder.hpp"

#include <algorithm>
#include <cstdio>

namespace geomsrv {
namespace archviz {
namespace overlayscene {
namespace build {

void Builder::AddLegend (const layers::Legend& legend)
{
    const bool right = legend.corner == layers::Corner::TopRight || legend.corner == layers::Corner::BottomRight;
    const bool bottom = legend.corner == layers::Corner::BottomLeft || legend.corner == layers::Corner::BottomRight;
    const double fx = right ? 1.0 : 0.0, fy = bottom ? 1.0 : 0.0;
    const float sx = right ? -1.0f : 1.0f, sy = bottom ? -1.0f : 1.0f;
    const float titleRoom = legend.title.empty () ? 0.0f : legend.sizePixels * 1.6f;
    // The bar's rectangle in pixels from the corner: its outer edge is the offset.
    const float barOuterX = sx * legend.offsetPixels[0];
    const float barInnerX = barOuterX + sx * legend.widthPixels;
    const float nearY = sy * legend.offsetPixels[1];
    const float farY = nearY + sy * (legend.lengthPixels + titleRoom);
    // The title sits at the legend's far end from a bottom corner and at its near
    // end from a top one -- above the bar either way -- and the value runs up it.
    const float barTop = (std::min) (nearY, farY) + titleRoom;
    const float barBottom = barTop + legend.lengthPixels;
    const float barLeft = (std::min) (barOuterX, barInnerX), barRight = (std::max) (barOuterX, barInnerX);

    DraftFill fill;
    fill.draw.screen = true;
    fill.draw.behind = kBehindShow;
    SetRamp (fill.draw, legend.colormap, legend.colormap.min, legend.colormap.max);
    auto corner = [&] (float x, float y, double value) {
        DraftVertex v;
        v.p[0] = fx;
        v.p[1] = fy;
        v.offset[0] = x;
        v.offset[1] = y;
        v.rgba = 0xFFFFFFFFu;
        v.value = float (value);
        fill.vertices.push_back (v);
    };
    const double low = legend.colormap.min, high = legend.colormap.max;
    corner (barLeft, barTop, high);
    corner (barRight, barTop, high);
    corner (barRight, barBottom, low);
    corner (barLeft, barTop, high);
    corner (barRight, barBottom, low);
    corner (barLeft, barBottom, low);
    draft_.fillVertices += fill.vertices.size ();
    draft_.fills.push_back (std::move (fill));

    // The ticks and their values on the bar's inner side; the title above it.
    const layers::Align textAlign = right ? layers::Align::Right : layers::Align::Left;
    const float textX = barInnerX + sx * 6.0f;
    const uint32_t ticks = (std::max) (legend.ticks, 2u);
    for (uint32_t k = 0; k < ticks; ++k) {
        const double t = double (k) / double (ticks - 1);
        const double value = low + (high - low) * t;
        const float y = barBottom - float (t) * legend.lengthPixels;
        char buffer[64] = {};
        std::snprintf (buffer, sizeof (buffer), "%.*f", int (legend.decimals), value);
        std::string label = buffer;
        if (!legend.unit.empty () && k + 1 == ticks)
            label += " " + legend.unit;
        ScreenText (label, fx, fy, textX, y, legend.sizePixels, textAlign, layers::Baseline::Middle, legend.rgba,
                    legend.haloRgba);
        // A 4 px tick from the bar's inner edge.
        DraftGlyph mark;
        mark.anchor[0] = fx;
        mark.anchor[1] = fy;
        mark.flags = kScreenAnchored | kSolid;
        mark.rgba = legend.rgba;
        const float x0 = barInnerX, x1 = barInnerX + sx * 4.0f;
        const float quad[4][2] = { { (std::min) (x0, x1), y - 0.5f },
                                   { (std::max) (x0, x1), y - 0.5f },
                                   { (std::max) (x0, x1), y + 0.5f },
                                   { (std::min) (x0, x1), y + 0.5f } };
        const int order[6] = { 0, 1, 2, 0, 2, 3 };
        for (int i = 0; i < 6; ++i)
            PushGlyphVertex (mark, quad[order[i]][0], quad[order[i]][1], -1.0f, -1.0f);
    }
    if (!legend.title.empty ())
        ScreenText (legend.title, fx, fy, right ? barRight : barLeft, barTop - legend.sizePixels * 0.6f,
                    legend.sizePixels, right ? layers::Align::Right : layers::Align::Left, layers::Baseline::Bottom,
                    legend.rgba, legend.haloRgba);
}

void Builder::ScreenText (const std::string& text, double fx, double fy, float x, float y, float size,
                          layers::Align align, layers::Baseline baseline, uint32_t rgba, uint32_t halo)
{
    overlaytext::Label label;
    if (!LayOut (text, size, align, baseline, label))
        return;
    DraftGlyph glyph;
    glyph.anchor[0] = fx;
    glyph.anchor[1] = fy;
    glyph.flags = kScreenAnchored;
    glyph.rgba = rgba;
    glyph.halo = halo;
    glyph.haloPixels = 1.25f;
    for (const overlaytext::Quad& quad : label.quads)
        PushQuad (glyph, quad, x, y);
}

} // namespace build
} // namespace overlayscene
} // namespace archviz
} // namespace geomsrv
