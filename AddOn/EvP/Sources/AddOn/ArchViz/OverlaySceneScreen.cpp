// ArchViz/OverlaySceneScreen -- what the builder puts FIXED TO THE VIEW: legends, their
// panels and ticks, and the HUD panels ImGui lays out (OverlayHud.hpp). Anchored to a
// fraction of the view and moved in pixels by the vertex shader, never projected.
// See OverlayScene.hpp and OverlaySceneBuilder.hpp.

#include "ArchViz/OverlaySceneBuilder.hpp"

#include "ArchViz/OverlayHud.hpp"

#include <algorithm>
#include <cstdio>

namespace geomsrv {
namespace archviz {
namespace overlayscene {
namespace build {

// Every layer's panels, laid out as one set (OverlayHud.hpp says why) and drawn
// after everything else, over it.
void Builder::AddPanels (const std::vector<const layers::Panel*>& panels)
{
    if (panels.empty ())
        return;
    if (hud_ == nullptr || !hud_->Ready ()) {
        draft_.problems.textsNotLaidOut += uint32_t (panels.size ());
        draft_.problems.lastError = "the overlay HUD is not ready: its panels are not drawn";
        return;
    }
    std::vector<overlayhud::Built> built;
    std::string error;
    if (!hud_->Build (panels, scale_, built, error)) {
        draft_.problems.textsNotLaidOut += uint32_t (panels.size ());
        draft_.problems.lastError = "a HUD panel: " + error;
        if (built.size () != panels.size ())
            return;
    }
    for (size_t i = 0; i < panels.size (); ++i) {
        float fraction[2] = {}, offset[2] = {};
        overlayhud::Place (*panels[i], built[i].width, built[i].height, scale_, fraction, offset);
        DraftGlyph glyph;
        glyph.anchor[0] = fraction[0];
        glyph.anchor[1] = fraction[1];
        glyph.flags = kScreenAnchored | kPlainTexture | kPhysicalPixels;
        glyph.behind = kBehindShow;
        for (const overlayhud::Vertex& v : built[i].vertices) {
            glyph.rgba = v.rgba;
            glyph.page = kHudPageBase + v.page;
            if (!PushGlyphVertex (glyph, offset[0] + v.x, offset[1] + v.y, v.u, v.v))
                return;
        }
    }
}

void Builder::AddLegend (const layers::Legend& legend)
{
    const bool right = legend.corner == layers::Corner::TopRight || legend.corner == layers::Corner::BottomRight;
    const bool bottom = legend.corner == layers::Corner::BottomLeft || legend.corner == layers::Corner::BottomRight;
    // The anchor: that corner of the view, or the fraction given. The legend's own
    // `corner` sits there, `offsetPixels` inwards.
    const double fx = legend.placed ? legend.screen[0] : (right ? 1.0 : 0.0);
    const double fy = legend.placed ? legend.screen[1] : (bottom ? 1.0 : 0.0);
    const float size = legend.sizePixels;
    const float titleSize = legend.titleSizePixels > 0.0f ? legend.titleSizePixels : size * 1.15f;
    const float pad = (legend.backgroundRgba & 0xFFu) != 0 ? legend.paddingPixels : 0.0f;

    // The ticks: where, and what each says.
    const double low = legend.colormap.min, high = legend.colormap.max;
    std::vector<double> values = legend.tickValues;
    if (values.empty ()) {
        const uint32_t ticks = (std::max) (legend.ticks, 2u);
        for (uint32_t k = 0; k < ticks; ++k)
            values.push_back (low + (high - low) * double (k) / double (ticks - 1));
    }
    std::vector<std::string> labels;
    std::vector<overlaytext::Label> laid (values.size ());
    float widest = 0.0f;
    for (size_t k = 0; k < values.size (); ++k) {
        std::string label;
        if (k < legend.tickLabels.size ())
            label = legend.tickLabels[k];
        else {
            char buffer[64] = {};
            std::snprintf (buffer, sizeof (buffer), "%.*f", int (legend.decimals), values[k]);
            label = buffer;
            if (!legend.unit.empty () && k + 1 == values.size () && legend.tickLabels.empty ())
                label += " " + legend.unit;
        }
        labels.push_back (label);
        // Measured only, in the legend's font: ScreenText lays each out again to draw it.
        if (overlaytext::Engine* const engine = EngineFor (legend.font); engine != nullptr && engine->Ready ()) {
            std::string error;
            if (engine->Layout (label, size, layers::Align::Left, layers::Baseline::Middle, laid[k], error))
                widest = (std::max) (widest, laid[k].right - laid[k].left);
        }
    }
    float titleWidth = 0.0f;
    overlaytext::Label titleLabel;
    overlaytext::Engine* const titleEngine = legend.title.empty () ? nullptr : EngineFor (legend.font);
    if (titleEngine != nullptr && titleEngine->Ready ()) {
        std::string error;
        if (titleEngine->Layout (legend.title, titleSize, layers::Align::Left, layers::Baseline::Top, titleLabel,
                                 error))
            titleWidth = titleLabel.right - titleLabel.left;
    }
    const float titleRoom = legend.title.empty () ? 0.0f : titleSize * 1.5f;

    // The layout in the legend's own box, x right and y down from its top-left.
    float barLeft = 0.0f, barTop = 0.0f, barRight = 0.0f, barBottom = 0.0f, boxWidth = 0.0f, boxHeight = 0.0f;
    if (legend.horizontal) {
        const float margin = widest * 0.5f;
        barLeft = pad + margin;
        barRight = barLeft + legend.lengthPixels;
        barTop = pad + titleRoom;
        barBottom = barTop + legend.widthPixels;
        boxWidth = pad + (std::max) (margin * 2.0f + legend.lengthPixels, titleWidth) + pad;
        boxHeight = barBottom + 6.0f + size * 1.3f + pad;
    }
    else {
        barLeft = pad;
        barRight = barLeft + legend.widthPixels;
        barTop = pad + titleRoom + size * 0.6f;
        barBottom = barTop + legend.lengthPixels;
        boxWidth = pad + (std::max) (legend.widthPixels + 6.0f + widest, titleWidth) + pad;
        boxHeight = barBottom + size * 0.6f + pad;
    }
    const float ox = right ? -boxWidth - legend.offsetPixels[0] : legend.offsetPixels[0];
    const float oy = bottom ? -boxHeight - legend.offsetPixels[1] : legend.offsetPixels[1];

    auto screenFill = [&] (DraftFill& fill, float x0, float y0, float x1, float y1, uint32_t rgba, double v0,
                           double v1) {
        // v0 at the low end, v1 at the high end: left to right across, bottom to top up.
        auto corner = [&] (float x, float y, double value) {
            DraftVertex v;
            v.p[0] = fx;
            v.p[1] = fy;
            v.offset[0] = ox + x;
            v.offset[1] = oy + y;
            v.rgba = rgba;
            v.value = float (value);
            fill.vertices.push_back (v);
        };
        const double tl = legend.horizontal ? v0 : v1, tr = v1, br = legend.horizontal ? v1 : v0,
                     bl = legend.horizontal ? v0 : v0;
        corner (x0, y0, tl);
        corner (x1, y0, tr);
        corner (x1, y1, br);
        corner (x0, y0, tl);
        corner (x1, y1, br);
        corner (x0, y1, bl);
    };

    // The panel first: fills are drawn in order, and glyphs after every fill.
    if ((legend.backgroundRgba & 0xFFu) != 0) {
        DraftFill panel;
        panel.draw.screen = true;
        panel.draw.behind = kBehindShow;
        screenFill (panel, 0.0f, 0.0f, boxWidth, boxHeight, legend.backgroundRgba, 0.0, 0.0);
        draft_.fillVertices += panel.vertices.size ();
        draft_.fills.push_back (std::move (panel));
    }
    DraftFill bar;
    bar.draw.screen = true;
    bar.draw.behind = kBehindShow;
    SetRamp (bar.draw, legend.colormap, low, high);
    screenFill (bar, barLeft, barTop, barRight, barBottom, 0xFFFFFFFFu, low, high);
    draft_.fillVertices += bar.vertices.size ();
    draft_.fills.push_back (std::move (bar));

    auto solid = [&] (float x0, float y0, float x1, float y1, uint32_t rgba) {
        DraftGlyph mark;
        mark.anchor[0] = fx;
        mark.anchor[1] = fy;
        mark.flags = kScreenAnchored | kSolid;
        mark.rgba = rgba;
        const float quad[4][2] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
        const int order[6] = { 0, 1, 2, 0, 2, 3 };
        for (int i = 0; i < 6; ++i)
            PushGlyphVertex (mark, ox + quad[order[i]][0], oy + quad[order[i]][1], -1.0f, -1.0f);
    };
    if ((legend.barBorderRgba & 0xFFu) != 0) {
        solid (barLeft - 1.0f, barTop - 1.0f, barRight + 1.0f, barTop, legend.barBorderRgba);
        solid (barLeft - 1.0f, barBottom, barRight + 1.0f, barBottom + 1.0f, legend.barBorderRgba);
        solid (barLeft - 1.0f, barTop, barLeft, barBottom, legend.barBorderRgba);
        solid (barRight, barTop, barRight + 1.0f, barBottom, legend.barBorderRgba);
    }

    for (size_t k = 0; k < values.size (); ++k) {
        const double t = high > low ? (values[k] - low) / (high - low) : 0.0;
        if (t < -1e-9 || t > 1.0 + 1e-9)
            continue;
        if (legend.horizontal) {
            const float x = barLeft + float (t) * legend.lengthPixels;
            solid (x - 0.5f, barBottom, x + 0.5f, barBottom + 4.0f, legend.rgba);
            ScreenText (labels[k], fx, fy, ox + x, oy + barBottom + 6.0f, size, layers::Align::Center,
                        layers::Baseline::Top, legend.rgba, legend.haloRgba, legend.haloPixels, legend.font);
        }
        else {
            const float y = barBottom - float (t) * legend.lengthPixels;
            solid (barRight, y - 0.5f, barRight + 4.0f, y + 0.5f, legend.rgba);
            ScreenText (labels[k], fx, fy, ox + barRight + 6.0f, oy + y, size, layers::Align::Left,
                        layers::Baseline::Middle, legend.rgba, legend.haloRgba, legend.haloPixels, legend.font);
        }
    }
    if (!legend.title.empty ())
        ScreenText (legend.title, fx, fy, ox + pad, oy + pad, titleSize, layers::Align::Left, layers::Baseline::Top,
                    legend.rgba, legend.haloRgba, legend.haloPixels, legend.font);
}

void Builder::ScreenText (const std::string& text, double fx, double fy, float x, float y, float size,
                          layers::Align align, layers::Baseline baseline, uint32_t rgba, uint32_t halo,
                          float haloPixels, const std::string& font)
{
    overlaytext::Label label;
    if (!LayOut (text, size, align, baseline, label, font))
        return;
    DraftGlyph glyph;
    glyph.anchor[0] = fx;
    glyph.anchor[1] = fy;
    glyph.flags = kScreenAnchored;
    glyph.rgba = rgba;
    glyph.halo = halo;
    glyph.haloPixels = HaloOf (haloPixels, 1.0f);
    for (const overlaytext::Quad& quad : label.quads)
        PushQuad (glyph, quad, x, y);
}

} // namespace build
} // namespace overlayscene
} // namespace archviz
} // namespace geomsrv
