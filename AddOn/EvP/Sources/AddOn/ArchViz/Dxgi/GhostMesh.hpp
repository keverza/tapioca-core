#ifndef EVP_ARCHVIZ_DXGI_GHOSTMESH_HPP
#define EVP_ARCHVIZ_DXGI_GHOSTMESH_HPP

// The first real geometry drawn through the proven injection path (PLAT-RE155,
// docs/architecture/api/HANDOFF-OverlayPatch.md stage 7).
//
// ⚠️ THE RESEARCH IS OVER AND THIS SPENDS IT. Runs forty-three to forty-six
// settled the whole camera and depth question: Archicad's own GPU camera is
// captured per frame and re-acquired when its resources are rebuilt, Present
// injection is stable, `scene` depth lets the building occlude us, and
// `PrivateCopy` carries Archicad's depth into a texture we own so our own
// surfaces occlude each other without one write reaching Archicad's buffer.
// What was missing was geometry worth drawing with it.
//
// ⚠️ DETERMINISTIC, NOT LIVE, AND THAT IS THE POINT OF THIS STEP. The scene is
// a pure function of (anchor, size, phase) -- four boxes whose overlaps are
// KNOWN in advance -- so every acceptance test has a right answer that does not
// depend on what is in the user's project:
//
//     two cubes that intersect      -> self-occlusion has a visible seam
//     a long bar through both       -> crossing geometry, and it runs far
//                                      enough out to enter the building, so
//                                      Archicad's depth must cut it
//     an upright bar crossing that  -> a second crossing at a right angle
//
// Feeding it a real Archicad element instead would test the recogniser, the
// mesh path and the extraction at once, and a failure would not say which.
//
// ⚠️ THE VERTEX BUFFER IS REBUILT AND RE-UPLOADED EVERY INJECTED FRAME, because
// "can the geometry change while orbiting" is one of the acceptance criteria and
// a static buffer would pass it by accident. `Map` with `WRITE_DISCARD` on a
// buffer WE own; Archicad's ring is never mapped, here or anywhere.
//
// THREAD SAFETY. `Prepare` runs on Archicad's render thread inside a detour,
// under `ScopedInjectionGuard`, and creates its device objects on first use from
// the context's own device. `SetAnchor`, `SetEnabled` and `Shutdown` are main
// thread; nothing here may outlive Archicad's device.

#include "ArchViz/Dxgi/OverlayStyle.hpp"

#include <cstdint>

struct ID3D11BlendState;
struct ID3D11Buffer;
struct ID3D11DepthStencilState;
struct ID3D11DepthStencilView;
struct ID3D11DeviceContext;
struct ID3D11RasterizerState;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace ghost {

// ⚠️ POSITION AND COLOUR, AND THE COLOUR IS NOT DECORATION. The production
// triangle's pixel shader returns one constant, which made it the wrong
// instrument for this step entirely: a nearer ghost surface hiding a farther one
// of the same colour is INVISIBLE, so self-occlusion could not be seen even when
// it worked. Each face carries its own shade.
struct Vertex {
    float x, y, z;
    float r, g, b;
};

// ⚠️ THREE PARTS IN ONE BUFFER, BECAUSE THEY MUST SHARE A CAMERA
// AND A FRAME. Drawing the solid, the wireframe and the heatmap from separate
// meshes would let them drift apart -- a different upload, a different phase, a
// different transform -- and the whole question this rung asks is whether three
// overlay KINDS behave correctly against ONE scene at ONE instant.
enum class Part : uint32_t { Solid = 0, Wireframe = 1, Heatmap = 2, kCount = 3 };

// Four boxes, each face a 2x2 grid: 48 triangles and 54 vertices per box.
constexpr uint32_t kBoxCount = 4;
constexpr uint32_t kVerticesPerBox = 6 * 9;
constexpr uint32_t kIndicesPerBox = 6 * 4 * 6;

// The wireframe is the twelve edges of a box enclosing the solid scene; the
// heatmap is a subdivided quad laid where a host surface would be.
constexpr uint32_t kWireVertices = 8;
constexpr uint32_t kWireIndices = 12 * 2;
constexpr uint32_t kHeatGrid = 9; // 9x9 vertices
constexpr uint32_t kHeatVertices = kHeatGrid * kHeatGrid;
constexpr uint32_t kHeatIndices = (kHeatGrid - 1) * (kHeatGrid - 1) * 6;

constexpr uint32_t kMaxVertices = kBoxCount * kVerticesPerBox + kWireVertices + kHeatVertices;
constexpr uint32_t kMaxIndices = kBoxCount * kIndicesPerBox + kWireIndices + kHeatIndices;

// MAIN THREAD. The scene is built around this point, in world metres.
void SetAnchor (float x, float y, float z, float sizeMetres);

// MAIN THREAD. Off by default. ⚠️ SEPARATE FROM THE INJECTION SWITCH: arming
// the injection draws the frozen proof primitives, and this decides whether the
// ghost mesh is drawn as well. Turning one off must not turn the other off, or a
// regression in the proof would be indistinguishable from a mesh bug.
void SetEnabled (bool enabled);
bool Enabled ();

// MAIN THREAD. Whether the vertices are displaced per frame. ⚠️ AN ACCEPTANCE
// CRITERION, NOT AN EFFECT: it is how "the mesh can change every frame without
// stalling" is tested. The displacement is deliberately small -- a few percent
// of the anchor size -- so a camera-lock error is still obvious underneath it.
void SetAnimated (bool animated);

// MAIN THREAD. The synthetic wireframe box and the 9x9 gradient grid.
//
// ⚠️ THESE ARE STAND-INS AND ARE OFF BY DEFAULT NOW THAT THE HOST
// GEOMETRY EXISTS. They were built so the three overlay kinds could be exercised
// before anything had extracted Archicad's model; a wireframe overlay means the
// outlines of the building that IS there, and a surface heatmap means the
// building's OWN surfaces -- both of which `hostoverlay` now draws. They stay
// switchable because they are the only self-occlusion test whose correct answer
// is known before Archicad runs.
void SetStandIns (bool enabled);
bool StandIns ();
bool Animated ();

// RENDER THREAD, from the Present path, with the camera and the depth view
// already bound. Rebuilds this frame's scene, uploads it and draws it; does
// nothing when the mesh is off or its device objects could not be made.
//
// ⚠️ THE PIPELINE BEHIND THIS BELONGS HERE, NOT TO THE RENDERER, so a
// mesh bug cannot break the proof primitives that would diagnose it. The states
// are passed in because they are the CALLER'S decision -- which depth mode is
// armed, whether culling is on -- and the mesh must not get to differ from the
// triangle on any of them.
//
// `interpretation` is the census's variant. Anything at or above 4 names a
// reversed multiplication order that no declaration can express; this falls back
// to 0 rather than drawing something nobody chose.
void Draw (ID3D11DeviceContext* context, uint32_t interpretation, ID3D11DepthStencilState* depthState,
           ID3D11RasterizerState* raster, ID3D11BlendState* blend);

// RENDER THREAD. Draw ONE part under ONE style.
//
// ⚠️ THE STYLE DECIDES, NOT THIS FILE. `OverlayStyle` carries whether
// the part occludes itself, whether it writes depth, how opaque it is, what it
// becomes behind an occluder and how far it is biased towards the camera --
// because solid ghosts, wireframes and heatmaps want three different answers and
// compiling any one of them in here makes the other two impossible.
//
// ⚠️ ONE DEPTH VIEW, BECAUSE D3D11 BINDS ONE. Host occlusion and
// self-occlusion cannot be two live buffers, so the overlay depth is SEEDED from
// HostOccluderDepth and then written into: the inherited values hide the overlay
// behind opaque host surfaces, and our own writes hide it behind itself. The
// rest is draw ORDER -- solid first so it writes, then the wireframe testing
// against those writes without adding its own, then the heatmap. That ordering
// is why a wireframe is correctly hidden by a ghost cube and not by its own far
// edges, and it is a property of the sequence rather than of any one state.
//
// `depthView` may be null, which is `HostOcclusionMode::None`: nothing occludes.
// Returns the index count drawn, or 0.
// RENDER THREAD. Rebuild and upload this frame's mesh. Returns the total index
// count, or 0. Call once per frame before the parts are drawn.
uint32_t Prepare (ID3D11DeviceContext* context);

uint32_t DrawPart (ID3D11DeviceContext* context, uint32_t interpretation, Part part, const overlay::OverlayStyle& style,
                   ID3D11DepthStencilView* depthView);

// RENDER THREAD. Draw the mesh ALREADY UPLOADED this frame, without rebuilding
// or re-uploading it.
//
// ⚠️ THE DEPTH DIAGNOSTIC DRAWS THIS ONCE PER ARCHICAD DRAW, AND
// REBUILDING EACH TIME WOULD MEASURE THE WRONG THING. It would advance the
// animation phase twenty-eight times within one frame, so every checkpoint would
// be testing a slightly different mesh and the differences between them would
// stop meaning "the depth buffer changed". The geometry has to be identical
// across the whole comparison, which is what this exists to guarantee.
//
// Returns the index count drawn, or 0.
uint32_t DrawCurrent (ID3D11DeviceContext* context, uint32_t interpretation, ID3D11DepthStencilState* depthState,
                      ID3D11RasterizerState* raster, ID3D11BlendState* blend);

// MAIN THREAD, at teardown.
void Shutdown ();

struct Stats {
    uint64_t builds = 0;
    uint64_t uploads = 0;
    uint64_t uploadFailures = 0;
    uint64_t draws = 0; // incremented by the renderer after DrawIndexed
    uint32_t vertices = 0;
    uint32_t indices = 0;
    uint32_t triangles = 0;
    uint32_t phase = 0;

    // ⚠️ THE TWO NUMBERS THAT SEPARATE CLIPPING FROM OCCLUSION, and
    // the user asked for exactly these rather than another diagnostic system.
    // Both zero means the geometry never reached the raster -- near plane,
    // transform or culling. The first non-zero and the second zero means it
    // rasterised and something occluded it, which is a depth question and a
    // different investigation entirely.
    uint64_t pixelsHostDepthOff = 0;
    uint64_t pixelsHostDepthOn = 0;
    uint64_t pixelRounds = 0;

    uint32_t solidIndices = 0;
    uint32_t wireIndices = 0;
    uint32_t heatIndices = 0;
    bool created = false;
    bool enabled = false;
    bool animated = false;
    char lastError[160] = {};
};
Stats GetStats ();

} // namespace ghost
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
