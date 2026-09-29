#ifndef EVP_ARCHVIZ_OVERLAYSCENE_HPP
#define EVP_ARCHVIZ_OVERLAYSCENE_HPP

// ArchViz/OverlayScene -- what the Diligent guest draws, prepared from the layers
// (ArchViz/OverlayLayers.hpp): vertex arrays for three pipelines -- fills, lines and
// glyph quads -- in each overlay's own coordinates, and the draw runs over them.
//
// ⚠️ EVERYTHING THAT DEPENDS ON THE VIEW IS LEFT TO THE VERTEX SHADER. A label, a
// dimension's text and its ticks, a legend and a marker are laid out HERE in pixels
// RELATIVE TO AN ANCHOR -- a model point, or a fraction of the view -- and the
// shader adds the anchor's projected pixel each frame (OverlayText.hpp gives the
// reason). A pixel-wide line is two model points widened in the shader. Nothing
// prepared here goes stale when the user pans, zooms or orbits; it is rebuilt only
// when the layers change.
//
// ⚠️ TWO COORDINATE SYSTEMS, NO CAMERA LOGIC (§12). The 2D arrays carry every model
// coordinate as hi/lo floats relative to a centre -- the walls' layout
// (PlanOverlayContent.hpp), which keeps a georeferenced project's millimetres. The
// 3D arrays carry world floats, as the reference wireframe does, because the camera
// they meet expects nothing else (HostOccluders.hpp). The projections themselves are
// in ArchViz/Dxgi/GuestShaderSources.hpp.
//
// ⚠️ THE LAYOUTS BELOW ARE THE SHADERS' INPUT LAYOUTS, BYTE FOR BYTE. The
// static_asserts pin the sizes and test_overlayshaders.cpp reads each vertex
// shader's inputs back by reflection; a field moved here without the shader is a
// picture of the wrong thing, never an error.
//
// MAIN THREAD (the text engine). Pure otherwise, so tests/cpp builds the real source.

#include "ArchViz/OverlayLayers.hpp"
#include "ArchViz/OverlayText.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace overlayscene {

// A glyph quad's flags, as the vertex shader tests them.
constexpr uint32_t kScreenAnchored = 1u; // the anchor is a fraction of the view, not a model point
constexpr uint32_t kAlongDirection = 2u; // the quad turns with its direction's projection
constexpr uint32_t kKeepUpright = 4u;    // ...and never reads upside down
constexpr uint32_t kSolid = 8u;          // a panel, a tick or an arrowhead: no atlas sample
constexpr uint32_t kHideShortSpan = 16u; // hidden while the direction projects shorter than minSpan

// `Behind`, resolved, as the 3D shaders read it.
constexpr uint32_t kBehindHide = 0u;
constexpr uint32_t kBehindFade = 1u;
constexpr uint32_t kBehindDash = 2u;
constexpr uint32_t kBehindShow = 3u;
uint32_t BehindCode (overlaylayers::Behind resolved);

// ---- the floor plan -----------------------------------------------------------

struct PlanFillVertex {
    float hi[2];     // model x, y relative to the origin: the float nearest...
    float lo[2];     // ...and what it dropped; for a screen fill, the view fraction and 0
    float offset[2]; // logical pixels after projection
    uint32_t rgba;   // R in the low byte
    float value;     // a heatmap's value
};
static_assert (sizeof (PlanFillVertex) == 32, "the guest's plan fill vertex is 32 bytes");

struct PlanLine {
    float hiA[2], loA[2], hiB[2], loB[2];
    uint32_t rgba;
    float widthPixels; // logical
    float dashPixels;  // logical; 0 is solid
    float dashDuty;
    float arcStart; // model metres along the polyline to A: the dash's phase
    float unused;
};
static_assert (sizeof (PlanLine) == 56, "the guest's plan line instance is 56 bytes");

struct PlanGlyph {
    float hi[2], lo[2]; // the anchor, or the view fraction and 0
    float offset[2];    // logical pixels from the anchor, x right, y down
    float uv[2];
    float dir[2]; // model direction the quad turns with
    uint32_t rgba;
    uint32_t halo;
    float haloPixels;
    uint32_t flags;
    float minSpan; // logical pixels
    float unused;
};
static_assert (sizeof (PlanGlyph) == 64, "the guest's plan glyph vertex is 64 bytes");

// ---- the 3D window ------------------------------------------------------------

struct SceneFillVertex {
    float position[3]; // world metres; for a screen fill, the view fraction
    float normal[3];
    float offset[2];
    uint32_t rgba;
    float value;
};
static_assert (sizeof (SceneFillVertex) == 40, "the guest's 3D fill vertex is 40 bytes");

struct SceneLine {
    float a[3], b[3];
    uint32_t rgba;
    float widthPixels;
    float dashPixels;
    float dashDuty;
    float arcStart;
    uint32_t behind;
};
static_assert (sizeof (SceneLine) == 48, "the guest's 3D line instance is 48 bytes");

struct SceneGlyph {
    float position[3];
    float dir[3];
    float offset[2];
    float uv[2];
    uint32_t rgba;
    uint32_t halo;
    float haloPixels;
    uint32_t flags;
    float minSpan;
    uint32_t behind;
};
static_assert (sizeof (SceneGlyph) == 64, "the guest's 3D glyph vertex is 64 bytes");

// ---- the draws ----------------------------------------------------------------

// One run of fill vertices with one look: a mesh, or a legend's bar.
struct FillDraw {
    uint32_t first = 0;
    uint32_t count = 0; // vertices, a triangle list
    uint32_t shading = 0;
    float opacity = 1.0f;
    uint32_t behind = kBehindShow;
    bool cullBack = false;
    bool screen = false;
    bool heatmap = false;
    uint32_t stopCount = 0;
    float stopAt[overlaylayers::kMaxStops] = {};
    uint32_t stopRgba[overlaylayers::kMaxStops] = {}; // 0xRRGGBBAA
    float min = 0.0f;
    float max = 1.0f;
    uint32_t bands = 0;
    float isolineStep = 0.0f;
    uint32_t isolineRgba = 0;
    float isolineWidthPixels = 1.0f;
};

// Glyph quads that sample one atlas page with one depth policy.
struct GlyphDraw {
    uint32_t first = 0;
    uint32_t count = 0; // vertices, six per quad
    uint32_t page = 0;  // index into `pages`
    uint32_t behind = kBehindShow;
};

struct Problems {
    uint32_t textsNotLaidOut = 0;
    uint32_t dimensionsNotResolved = 0;
    uint32_t truncated = 0; // primitives past the size budget
    std::string lastError;
};

struct Plan {
    double originX = 0.0;
    double originY = 0.0;
    std::vector<PlanFillVertex> fills;
    std::vector<FillDraw> fillDraws;
    std::vector<PlanLine> lines;
    std::vector<PlanGlyph> glyphs;
    std::vector<GlyphDraw> glyphDraws;
    std::vector<std::shared_ptr<const overlaytext::Page>> pages;
    uint64_t generation = 0;
    Problems problems;

    bool Empty () const
    {
        return fills.empty () && lines.empty () && glyphs.empty ();
    }
};

struct Scene {
    std::vector<SceneFillVertex> fills;
    std::vector<FillDraw> fillDraws;
    std::vector<SceneLine> lines;
    std::vector<SceneGlyph> glyphs;
    std::vector<GlyphDraw> glyphDraws;
    std::vector<std::shared_ptr<const overlaytext::Page>> pages;
    uint64_t generation = 0;
    Problems problems;

    bool Empty () const
    {
        return fills.empty () && lines.empty () && glyphs.empty ();
    }
};

// What the guest draws of these layers in each view. `text` may be null or not
// ready: labels are then counted in `problems` and skipped, everything else drawn.
Plan PreparePlan (const std::vector<std::shared_ptr<const overlaylayers::Layer>>& layers, overlaytext::Engine* text);
Scene PrepareScene (const std::vector<std::shared_ptr<const overlaylayers::Layer>>& layers, overlaytext::Engine* text);

// ---- pure helpers, exposed for their tests -----------------------------------

// Area-weighted vertex normals, one x, y, z per vertex; a vertex no triangle uses
// gets +z.
std::vector<double> VertexNormals (const std::vector<double>& points, const std::vector<uint32_t>& indices);

// Boundary edges, and edges whose two faces meet at more than `angleDegrees`, as
// vertex index pairs. Coincident vertices are welded first, so a seam where a mesh
// repeats its vertices is not mistaken for a boundary.
std::vector<std::pair<uint32_t, uint32_t>> FeatureEdges (const std::vector<double>& points,
                                                         const std::vector<uint32_t>& indices, float angleDegrees);

// "5.00", "500.0 cm", "5000 mm".
std::string FormatLength (double metres, uint32_t decimals, overlaylayers::LengthUnit unit, bool showUnit);

// A ramp's colour at `t` in 0..1, by the same interpolation the pixel shader makes.
uint32_t RampAt (const std::vector<overlaylayers::ColourStop>& stops, float t);

} // namespace overlayscene
} // namespace archviz
} // namespace geomsrv

#endif
