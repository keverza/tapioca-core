#ifndef EVP_ARCHVIZ_OVERLAYSCENEBUILDER_HPP
#define EVP_ARCHVIZ_OVERLAYSCENEBUILDER_HPP

// ArchViz/OverlaySceneBuilder -- INTERNAL to OverlayScene: the draft every layer is
// turned into before each overlay writes it in its own coordinates, and the builder
// that turns it. Shared by the three files that build it -- OverlayScene.cpp (the
// model's content and the two writers), OverlaySceneScreen.cpp (what is fixed to the
// view: legends, HUD panels) and OverlaySceneMath.cpp (the pure geometry) -- and by
// nothing else: OverlayScene.hpp is the interface.

#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayScene.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <utility>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace overlayscene {
namespace build {

namespace layers = overlaylayers;

// Budgets on what one set of layers may turn into. A caller past them sees the
// excess counted in `truncated`, never an allocation that takes Archicad with it.
inline constexpr size_t kMaxFillVertices = 6000000;
inline constexpr size_t kMaxLines = 2000000;
inline constexpr size_t kMaxGlyphVertices = 1200000;

// Dash patterns one set of layers may hold (the guest's constant buffer has room for these);
// a line's two patterns -- where it is visible, where it is hidden -- are packed into one
// word, 255 in either byte meaning solid.
inline constexpr size_t kMaxDashPatterns = 16;
inline constexpr uint32_t kSolidPattern = 255u;
inline constexpr uint32_t kSolidDashes = kSolidPattern | (kSolidPattern << 8);

inline constexpr double kPi = 3.14159265358979323846;

// A glyph vertex's halo: fixed logical pixels, or -- negative -- an automatic halo's
// scale, which the pixel shader sizes by the text as drawn (HaloReach).
inline float HaloOf (float pixels, float scale)
{
    return pixels >= 0.0f ? pixels : -scale;
}
// A HUD panel's glyphs name their atlas page from here up until the pages are composed:
// the text pages first, the HUD's after them.
inline constexpr uint32_t kHudPageBase = 1u << 24;
// Faces meeting at more than this are shaded apart when a mesh brings no normals.
inline constexpr float kSmoothingCreaseDegrees = 45.0f;

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
};

inline Vec3 Sub (const Vec3& a, const Vec3& b)
{
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}
inline Vec3 Cross (const Vec3& a, const Vec3& b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
inline double Dot (const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline double Length (const Vec3& a)
{
    return std::sqrt (Dot (a, a));
}
inline Vec3 At (const std::vector<double>& points, uint32_t index)
{
    const size_t i = size_t (index) * 3;
    return { points[i], points[i + 1], points[i + 2] };
}
inline Vec3 Scaled (const Vec3& a, double k)
{
    return { a.x * k, a.y * k, a.z * k };
}
inline Vec3 Plus (const Vec3& a, const Vec3& b)
{
    return { a.x + b.x, a.y + b.y, a.z + b.z };
}
inline Vec3 Unit (const Vec3& a)
{
    const double length = Length (a);
    return length > 1e-30 ? Scaled (a, 1.0 / length) : Vec3 {};
}

// ---- the draft: one builder, two writers -------------------------------------

struct DraftVertex {
    double p[3] = {};
    float n[3] = { 0.0f, 0.0f, 1.0f };
    float offset[2] = {};
    uint32_t rgba = 0;
    float value = 0.0f;
};

struct DraftFill {
    FillDraw draw;
    std::vector<DraftVertex> vertices;
};

struct DraftLine {
    double a[3] = {}, b[3] = {};
    uint32_t rgba = 0;
    uint32_t hiddenRgba = 0; // alpha 0: `rgba`, faint where it fades
    float width = 1.0f, hiddenWidth = 0.0f, arc = 0.0f;
    uint32_t dashes = kSolidDashes;
    uint32_t behind = kBehindShow;
};

struct DraftGlyph {
    double anchor[3] = {};
    double dir[3] = {};
    float offset[2] = {};
    float uv[2] = {};
    uint32_t rgba = 0, halo = 0;
    float haloPixels = 0.0f;
    uint32_t flags = 0;
    float minSpan = 0.0f;
    uint32_t page = 0;
    uint32_t behind = kBehindShow;
};

struct Draft {
    std::vector<DraftFill> fills;
    std::vector<DraftLine> lines;
    std::vector<DraftGlyph> glyphs;
    size_t fillVertices = 0;
    Problems problems;
    // The text pages the glyphs sample, from every font's engine: a glyph's `page`
    // indexes this, below kHudPageBase.
    std::vector<std::shared_ptr<const overlaytext::Page>> textPages;
    // The dash patterns the lines name, kMaxDashEntries lengths each, zero-padded.
    std::vector<std::array<float, layers::kMaxDashEntries>> dashes;
    // Where its legends and panels are on the view (OverlayHitMap.hpp).
    std::vector<overlayinput::Region> regions;
    // A HUD draft's: the band of a layer's heatmaps the pointer shows, and the hand.
    Highlight highlight;
    bool hand = false;
    uint8_t cursor = 0;
    bool locked = false;
};

// A panel, and whose it is: its layer and its place among that layer's panels -- what
// the HUD's input names it by.
struct PanelRef {
    const layers::Panel* panel = nullptr;
    std::string layer;
    uint32_t index = 0;
};

class Builder {
  public:
    Builder (layers::Views view, overlaytext::Engine* text, overlayhud::Engine* hud, float scale,
             const FontResolver& fonts)
        : view_ (view), text_ (text), hud_ (hud), scale_ (scale > 0.0f ? scale : 1.0f), fonts_ (fonts)
    {
    }

    Draft Take ()
    {
        return std::move (draft_);
    }

    void AddLayer (const layers::Layer& layer);
    // Every layer's panels, laid out as one set (OverlayHud.hpp says why) and drawn
    // after everything else, over it.
    // `input` is the view and the pointer (OverlayHud.hpp); `legends` the legends' bars
    // there, hovered for their values.
    void AddPanels (const std::vector<PanelRef>& panels, const overlayhud::Input& input,
                    const std::vector<overlayhud::LegendBar>& legends);
    // The HUD stream of the layers drawn in this view: their panels, and the scene's
    // legends' bars (`legends`, Scene.regions) where they are on `input`'s view.
    void AddHud (const std::vector<std::shared_ptr<const layers::Layer>>& all, const overlayhud::Input& input,
                 const std::vector<overlayinput::Region>* legends);

  private:
    bool Plan () const;
    // The 2D overlay has no depth (finding 13): everything there is drawn whole.
    uint32_t BehindOf (layers::Behind behind, const layers::Layer& layer) const;
    void Truncated ();
    void SetRamp (FillDraw& draw, const layers::Colormap& colormap, double min, double max) const;
    void AddMesh (const layers::Layer& layer, const layers::Mesh& mesh);
    // Hover mode's tint (OverlayHud.hpp `Hover::tintModel`): the item's triangles in model
    // metres, one flat fill in `kHoverTintRgba`, never hidden -- drawn by the view's renderer
    // with its camera or transform, so it stays on the item as the view moves.
    void AddHoverTint (const std::vector<double>& corners);
    void AddPolyline (const layers::Layer& layer, const layers::Polyline& polyline);
    void AddDimension (const layers::Layer& layer, const layers::Dimension& dimension);
    void AddDimensionText (const layers::Layer& layer, const layers::Dimension& dimension, double measurement,
                           const Vec3& middle, const Vec3& span, const Vec3& normal);
    // A tick, an arrowhead or a dot `size` pixels long at one end of a line, in the
    // local frame whose +x is the line's direction on screen; `first` is its start.
    void AddTerminator (const Vec3& at, const Vec3& span, layers::Terminator terminator, bool first, float width,
                        float size, uint32_t rgba, uint32_t behind);
    void AddText (const layers::Layer& layer, const layers::Text& text);
    // The label's layout mapped onto its plane: x along `direction`, the layout's y (down)
    // against the glyphs' up, `normal` x `direction`. Every corner is its own model point
    // (`kModelQuad`), so the text is foreshortened and hidden like the model.
    void AddPlanarText (const layers::Layer& layer, const layers::Text& text, const overlaytext::Label& label);
    // `index` is the legend's place among its layer's, which its region names.
    void AddLegend (const layers::Layer& layer, const layers::Legend& legend, uint32_t index);
    void ScreenText (const std::string& text, double fx, double fy, float x, float y, float size, layers::Align align,
                     layers::Baseline baseline, uint32_t rgba, uint32_t halo, float haloPixels,
                     const std::string& font);
    // `text` shaped in `font`'s engine, its quads' pages renumbered into the draft's.
    bool LayOut (const std::string& text, float size, layers::Align align, layers::Baseline baseline,
                 overlaytext::Label& label, const std::string& font);
    // The engine for a font file; the bundled font's for none, or for one that failed.
    overlaytext::Engine* EngineFor (const std::string& font);
    // A pattern's index in the draft's table, kSolidPattern for none.
    uint32_t DashOf (const float* lengths, size_t count);
    // A line's two patterns packed: where it is visible, and -- for "dash" and "fade" --
    // behind the building, a "dash" line defaulting to kDefaultHiddenDash.
    uint32_t DashesOf (const std::vector<float>& visible, const layers::HiddenLine* hidden, uint32_t behind);
    // One glyph's two triangles, moved by (dx, dy) in the label's frame.
    void PushQuad (DraftGlyph glyph, const overlaytext::Quad& quad, float dx, float dy);
    bool PushGlyphVertex (const DraftGlyph& glyph, float x, float y, float u, float v);
    bool PushLine (const DraftLine& line);
    static void Assign (double (&out)[3], const Vec3& value);
    layers::Views view_;
    overlaytext::Engine* text_;
    overlayhud::Engine* hud_;
    float scale_;
    const FontResolver& fonts_;
    Draft draft_;
    // Each (engine, page) a glyph used, and its index in `draft_.textPages`.
    std::map<std::pair<const overlaytext::Engine*, uint32_t>, uint32_t> pageSlots_;
};

} // namespace build
} // namespace overlayscene
} // namespace archviz
} // namespace geomsrv

#endif
