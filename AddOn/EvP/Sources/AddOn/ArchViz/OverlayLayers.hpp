#ifndef EVP_ARCHVIZ_OVERLAYLAYERS_HPP
#define EVP_ARCHVIZ_OVERLAYLAYERS_HPP

// ArchViz/OverlayLayers -- arbitrary geometry a caller puts on the overlays: named
// layers of polylines, points and triangle meshes in MODEL METRES, drawn by the 2D
// overlay (the floor plan, at its Present) and the 3D overlay (the 3D window, at its
// Present) through each one's own transform. `Tapioca.SetOverlayLayer` fills it.
//
// ⚠️ ONE VOCABULARY, TWO PROJECTIONS, NO CAMERA LOGIC SHARED (§12). A layer is
// geometry and style only. The 2D overlay projects it with the transform ACAPI
// reads at the plan's Present, dropping z; the 3D overlay projects it with the
// camera copied at the model's draw. Neither projection lives here.
//
// ⚠️ DOUBLE UNTIL IT IS UPLOADED. The 2D preparation splits every coordinate into
// hi/lo floats relative to a centre, exactly as the wall outlines are
// (PlanOverlayContent.hpp): a georeferenced project keeps its millimetres. The 3D
// preparation hands world floats to the same camera the building's own geometry
// is drawn with, because that camera expects nothing else (HostOccluders.hpp).
//
// THREADS. `Set`/`Clear` and the preparations are MAIN THREAD; the 3D overlay
// receives what it draws through its own lock-free hand-over, never this store.

#include "ArchViz/PlanOverlayContent.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace overlaylayers {

enum class Views : uint32_t {
    TwoD = 1,
    ThreeD = 2,
    Both = 3,
};

bool DrawnIn (Views views, Views view);

struct Polyline {
    std::vector<double> points; // x, y, z model metres; at least two points
    bool closed = false;
    uint32_t rgba = 0xFF3B30FFu; // 0xRRGGBBAA
    float widthPixels = 2.0f;
};

// A marker at each point: a square `sizePixels` wide in 2D, an axis cross with arms
// `sizeMetres` long in 3D -- a 3D marker has no pixel size until a camera gives it
// one, and the 3D camera lives on the GPU.
struct PointSet {
    std::vector<double> points; // x, y, z
    uint32_t rgba = 0xFF3B30FFu;
    float sizePixels = 6.0f;
    float sizeMetres = 0.1f;
};

struct Mesh {
    std::vector<double> points;       // x, y, z
    std::vector<uint32_t> indices;    // three per triangle
    uint32_t rgba = 0xFF3B3080u;      // one colour for the mesh
    std::vector<uint32_t> vertexRgba; // optional, one per vertex; overrides `rgba`
};

struct Layer {
    std::string name;
    Views views = Views::Both;
    // 3D only: hidden behind the building's opaque surfaces (the host occluder), as
    // the reference wireframe is. False draws it over everything.
    bool occluded = true;
    std::vector<Polyline> polylines;
    std::vector<PointSet> points;
    std::vector<Mesh> meshes;
};

// What `Validate` refused, in a sentence a caller can act on. Empty when valid.
std::string Validate (const Layer& layer);

struct Summary {
    std::string name;
    Views views = Views::Both;
    bool occluded = true;
    uint32_t polylines = 0;
    uint32_t lineVertices = 0;
    uint32_t points = 0;
    uint32_t meshes = 0;
    uint32_t triangles = 0;
};
Summary Summarise (const Layer& layer);

// ---- the store, MAIN THREAD --------------------------------------------------

// Replace (or add) the layer of this name. Returns the store's new generation.
uint64_t Set (Layer layer);
// Remove one layer; false when there was none of that name.
bool Clear (const std::string& name);
void ClearAll ();
// Every layer, in the order they were first set -- the draw order.
std::vector<std::shared_ptr<const Layer>> Layers ();
// Moves on every change; a renderer rebuilds when it differs from what it holds.
uint64_t Generation ();

// ---- preparation for the 2D overlay (pure) ------------------------------------

// One stroke instance: the segment's hi/lo halves (PlanOverlayContent's layout),
// then its colour and width. Six corners are made of it in the vertex shader.
struct StrokeInstance {
    plancontent::Segment segment;
    uint32_t rgba = 0; // R in the low byte: DXGI_FORMAT_R8G8B8A8_UNORM
    float widthPixels = 0.0f;
};
static_assert (sizeof (StrokeInstance) == 40, "the 2D layer stroke is 40 bytes");

// One filled vertex: its hi/lo halves and its colour.
struct FillVertex {
    float x = 0.0f, y = 0.0f;     // hi
    float xLo = 0.0f, yLo = 0.0f; // lo
    uint32_t rgba = 0;
};
static_assert (sizeof (FillVertex) == 20, "the 2D layer fill vertex is 20 bytes");

struct Prepared2D {
    double originX = 0.0; // model metres: the centre every half is relative to
    double originY = 0.0;
    std::vector<StrokeInstance> strokes; // polylines and point markers
    std::vector<FillVertex> fills;       // triangle list
};

// The layers drawn in 2D, in draw order: fills first, strokes over them. A point
// marker is four strokes of its square.
Prepared2D Prepare2D (const std::vector<std::shared_ptr<const Layer>>& layers);

// ---- preparation for the 3D overlay (pure) ------------------------------------

// World float position and colour: the building's own coordinates (HostOccluders).
struct ColourVertex {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    uint32_t rgba = 0; // R in the low byte
};
static_assert (sizeof (ColourVertex) == 16, "the 3D layer vertex is 16 bytes");

struct Prepared3D {
    std::vector<ColourVertex> occludedLines; // line list, hidden behind the building
    std::vector<ColourVertex> overLines;     // line list, over everything
    std::vector<ColourVertex> occludedFills; // triangle list
    std::vector<ColourVertex> overFills;
    uint64_t generation = 0;
};

// The layers drawn in 3D. A point marker becomes three axis segments through it,
// each `sizeMetres` long.
Prepared3D Prepare3D (const std::vector<std::shared_ptr<const Layer>>& layers);

// 0xRRGGBBAA (how callers write it) -> R in the low byte (how D3D reads it).
uint32_t ToUnorm (uint32_t rgba);

} // namespace overlaylayers
} // namespace archviz
} // namespace geomsrv

#endif
