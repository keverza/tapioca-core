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

#include <cstdint>

struct ID3D11BlendState;
struct ID3D11Buffer;
struct ID3D11DepthStencilState;
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

// Four boxes, each face a 2x2 grid: 48 triangles and 54 vertices per box.
constexpr uint32_t kBoxCount = 4;
constexpr uint32_t kVerticesPerBox = 6 * 9;
constexpr uint32_t kIndicesPerBox = 6 * 4 * 6;
constexpr uint32_t kMaxVertices = kBoxCount * kVerticesPerBox; // 216
constexpr uint32_t kMaxIndices = kBoxCount * kIndicesPerBox;   // 576

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
