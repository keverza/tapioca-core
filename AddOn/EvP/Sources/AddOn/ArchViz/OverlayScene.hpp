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

#include "ArchViz/OverlayHitMap.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayLayers.hpp"
#include "ArchViz/OverlayText.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace overlayhud {
class Engine;
} // namespace overlayhud
namespace overlayscene {

// A glyph quad's flags, as the vertex shader tests them.
constexpr uint32_t kScreenAnchored = 1u;   // the anchor is a fraction of the view, not a model point
constexpr uint32_t kAlongDirection = 2u;   // the quad turns with its direction's projection
constexpr uint32_t kKeepUpright = 4u;      // ...and never reads upside down
constexpr uint32_t kSolid = 8u;            // a panel, a tick or an arrowhead: no atlas sample
constexpr uint32_t kHideShortSpan = 16u;   // hidden while the direction projects shorter than minSpan
constexpr uint32_t kModelQuad = 32u;       // each corner is its own model point: text lying on a plane
constexpr uint32_t kPlainTexture = 64u;    // the texture times the colour: an ImGui panel (OverlayHud.hpp)
constexpr uint32_t kPhysicalPixels = 128u; // the offset is in the view's pixels already, not logical ones
constexpr uint32_t kZoomLabel = 256u;      // dir is one world-size em; hide below minSpan pixels

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
    float arcStart;    // model metres along the polyline to A: where its dash pattern stands
    uint32_t dashes;   // the pattern in `dashes`, 255 solid (the plan draws every line whole)
    uint32_t unused[2];
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
    uint32_t hiddenRgba; // behind the building; alpha 0 is `rgba`, faint where it fades
    float widthPixels;
    float hiddenWidthPixels; // 0 is `widthPixels`
    float arcStart;          // model metres along the polyline to `a`
    uint32_t dashes;         // visible pattern | hidden pattern << 8; 255 solid
    uint32_t behind;
    uint32_t unused;
};
static_assert (sizeof (SceneLine) == 56, "the guest's 3D line instance is 56 bytes");

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
    float occludedOpacity = 0.3f;
    bool hatched = false;
    float hatchDirection = 45.0f;
    float hatchDensity = 1.0f;
    float hatchPhase = 0.0f; // Plan origin's stripe phase, keeps hatches anchored as content changes.
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
    uint32_t layer = 0; // a heatmap's layer (LayerKey): what a highlight names
};

// A band of one layer's heatmap values shown, the rest of them dimmed: the legend or ramp
// the pointer is on (OverlayHud.hpp `Layout::Highlight`). `layer` 0: none.
struct Highlight {
    uint32_t layer = 0;
    float low = 0.0f;
    float high = 0.0f;
};

// A layer's name as its fills carry it; never 0.
uint32_t LayerKey (const std::string& name);

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

// What preparing the layers cost, on the main thread: how many were built and how many
// were reused as they stood (the cache below PreparePlan), and how long it took.
struct Cost {
    uint32_t layersBuilt = 0;
    uint32_t layersReused = 0;
    uint32_t microseconds = 0;
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
    // The lines' dash patterns: kMaxDashEntries lengths in metres each, a line's `dashes`
    // indexing them (the guest's GuestDraw.Dashes).
    std::vector<float> dashes;
    // Where its legends and panels are on the view, in draw order: what the HUD's input
    // tests a pointer against (OverlayHitMap.hpp).
    std::vector<overlayinput::Region> regions;
    // The HUD stream's: the band of a layer's heatmaps the pointer shows, and whether the
    // pointer is on something it can press (overlayhud::Layout `hand`).
    Highlight highlight;
    bool hand = false;
    uint8_t cursor = 0;  // overlayinput::Cursor
    bool locked = false; // the dock's lock (overlayhud::Layout `locked`)
    uint64_t generation = 0;
    Problems problems;
    Cost cost;

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
    // The lines' dash patterns: kMaxDashEntries lengths in metres each, a line's `dashes`
    // indexing them (the guest's GuestDraw.Dashes).
    std::vector<float> dashes;
    // Where its legends and panels are on the view, in draw order: what the HUD's input
    // tests a pointer against (OverlayHitMap.hpp).
    std::vector<overlayinput::Region> regions;
    // The HUD stream's: the band of a layer's heatmaps the pointer shows, and whether the
    // pointer is on something it can press (overlayhud::Layout `hand`).
    Highlight highlight;
    bool hand = false;
    uint8_t cursor = 0;  // overlayinput::Cursor
    bool locked = false; // the dock's lock (overlayhud::Layout `locked`)
    uint64_t generation = 0;
    Problems problems;
    Cost cost;

    bool Empty () const
    {
        return fills.empty () && lines.empty () && glyphs.empty ();
    }
};

// The text engine for a font file a text names (guesttext::EngineFor); null leaves that
// text in the bundled font, with the reason in `problems`.
using FontResolver = std::function<overlaytext::Engine*(const std::string& font)>;

// ⚠️ ONLY WHAT CHANGED IS BUILT AGAIN. A layer is immutable once set (OverlayLayers.hpp),
// so the same layer object is the same content: what it was turned into -- normals
// welded, feature edges found, text shaped -- is kept per view and reused while it
// stands. A Watch trace ticking, or slices following an edit, rebuild their own layer,
// not a 200k-triangle heatmap beside it. The HUD panels are not part of it: they are a
// stream of their own (PrepareSceneHud). MAIN THREAD, as the text engine is; the cache
// holds only the layers of the last call, and `ForgetDrafts` empties it -- one view's, or
// both.
void ForgetDrafts ();
void ForgetDrafts (overlaylayers::Views view);

// What the guest draws of these layers in each view, the HUD panels apart. `text` may
// be null or not ready: labels are then counted in `problems` and skipped, everything
// else drawn. `fonts` finds the engine for a text in a font of its own; without it
// every text is in `text`'s.
Plan PreparePlan (const std::vector<std::shared_ptr<const overlaylayers::Layer>>& layers, overlaytext::Engine* text,
                  const FontResolver& fonts = FontResolver ());
Scene PrepareScene (const std::vector<std::shared_ptr<const overlaylayers::Layer>>& layers, overlaytext::Engine* text,
                    const FontResolver& fonts = FontResolver ());

// ⚠️ THE HUD IS A STREAM OF ITS OWN. The panels of the layers drawn in each view, laid
// out by `hud` at `scale` (the view's DPI scale) into glyphs only, which the guests
// upload and draw apart from the scene and after it, over everything. A panel that
// changes -- a hover, a collapse -- lays out and sends a few thousand vertices again,
// never the heatmap beside it. Null or not ready: the panels are counted in
// `problems` and not drawn.
//
// `input` is the view and the pointer over it (OverlayInput.hpp); `legends` the regions
// the scene's legends were drawn in (Scene.regions), whose bars the pointer is tested
// against for a value tooltip. The engine is one view's: each keeps its own state.
Plan PreparePlanHud (const std::vector<std::shared_ptr<const overlaylayers::Layer>>& layers, overlayhud::Engine* hud,
                     float scale, const overlayhud::Input& input = overlayhud::Input (),
                     const std::vector<overlayinput::Region>* legends = nullptr);
Scene PrepareSceneHud (const std::vector<std::shared_ptr<const overlaylayers::Layer>>& layers, overlayhud::Engine* hud,
                       float scale, const overlayhud::Input& input = overlayhud::Input (),
                       const std::vector<overlayinput::Region>* legends = nullptr);

// What a HUD stream puts on screen, in one number: two layouts with the same one draw
// the same pixels, so the second need not be uploaded or redrawn.
uint64_t Fingerprint (const Plan& hud);
uint64_t Fingerprint (const Scene& hud);

// ---- pure helpers, exposed for their tests -----------------------------------

// Area-weighted vertex normals, one x, y, z per vertex; a vertex no triangle uses
// gets +z.
std::vector<double> VertexNormals (const std::vector<double>& points, const std::vector<uint32_t>& indices);

// One normal per triangle CORNER (x, y, z for each index): the area-weighted mean of
// the faces round that corner's vertex that turn from its own face by at most
// `creaseDegrees`. Smooth across a curved surface, flat on each face of a box -- where
// per-vertex normals averaged three faces into every corner and bent each flat face
// into a bulge (the ghost box, the first live run). Coincident vertices are welded
// first, as for FeatureEdges.
std::vector<double> CornerNormals (const std::vector<double>& points, const std::vector<uint32_t>& indices,
                                   float creaseDegrees);

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
