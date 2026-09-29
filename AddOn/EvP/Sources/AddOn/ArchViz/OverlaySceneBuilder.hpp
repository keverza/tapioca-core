#ifndef EVP_ARCHVIZ_OVERLAYSCENEBUILDER_HPP
#define EVP_ARCHVIZ_OVERLAYSCENEBUILDER_HPP

// ArchViz/OverlaySceneBuilder -- INTERNAL to OverlayScene: the draft every layer is
// turned into before each overlay writes it in its own coordinates, and the builder
// that turns it. Shared by the three files that build it -- OverlayScene.cpp (the
// model's content and the two writers), OverlaySceneScreen.cpp (what is fixed to the
// view: legends, HUD panels) and OverlaySceneMath.cpp (the pure geometry) -- and by
// nothing else: OverlayScene.hpp is the interface.

#include "ArchViz/OverlayScene.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
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
    float width = 1.0f, dash = 0.0f, duty = 0.5f, arc = 0.0f;
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
};

class Builder {
  public:
    Builder (layers::Views view, overlaytext::Engine* text, overlayhud::Engine* hud, float scale)
        : view_ (view), text_ (text), hud_ (hud), scale_ (scale > 0.0f ? scale : 1.0f)
    {
    }

    Draft Take ()
    {
        return std::move (draft_);
    }

    void AddLayer (const layers::Layer& layer);
    // Every layer's panels, laid out as one set (OverlayHud.hpp says why) and drawn
    // after everything else, over it.
    void AddPanels (const std::vector<const layers::Panel*>& panels);

  private:
    bool Plan () const;
    // The 2D overlay has no depth (finding 13): everything there is drawn whole.
    uint32_t BehindOf (layers::Behind behind, const layers::Layer& layer) const;
    void Truncated ();
    void SetRamp (FillDraw& draw, const layers::Colormap& colormap, double min, double max) const;
    void AddMesh (const layers::Layer& layer, const layers::Mesh& mesh);
    void AddPolyline (const layers::Layer& layer, const layers::Polyline& polyline);
    void AddDimension (const layers::Layer& layer, const layers::Dimension& dimension);
    // A tick, an arrowhead or a dot at one end of a dimension line, in the local
    // frame whose +x is the dimension's direction on screen.
    void AddTerminator (const Vec3& at, const Vec3& span, layers::Terminator terminator, bool first, float width,
                        uint32_t rgba, uint32_t behind);
    void AddText (const layers::Layer& layer, const layers::Text& text);
    // The label's layout mapped onto its plane: x along `direction`, the layout's y (down)
    // against the glyphs' up, `normal` x `direction`. Every corner is its own model point
    // (`kModelQuad`), so the text is foreshortened and hidden like the model.
    void AddPlanarText (const layers::Layer& layer, const layers::Text& text, const overlaytext::Label& label);
    void AddLegend (const layers::Legend& legend);
    void ScreenText (const std::string& text, double fx, double fy, float x, float y, float size, layers::Align align,
                     layers::Baseline baseline, uint32_t rgba, uint32_t halo);
    bool LayOut (const std::string& text, float size, layers::Align align, layers::Baseline baseline,
                 overlaytext::Label& label);
    // One glyph's two triangles, moved by (dx, dy) in the label's frame.
    void PushQuad (DraftGlyph glyph, const overlaytext::Quad& quad, float dx, float dy);
    bool PushGlyphVertex (const DraftGlyph& glyph, float x, float y, float u, float v);
    bool PushLine (const DraftLine& line);
    static void Assign (double (&out)[3], const Vec3& value);
    layers::Views view_;
    overlaytext::Engine* text_;
    overlayhud::Engine* hud_;
    float scale_;
    Draft draft_;
};

} // namespace build
} // namespace overlayscene
} // namespace archviz
} // namespace geomsrv

#endif
