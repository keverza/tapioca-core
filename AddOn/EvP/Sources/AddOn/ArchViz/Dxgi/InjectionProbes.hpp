#ifndef EVP_ARCHVIZ_DXGI_INJECTIONPROBES_HPP
#define EVP_ARCHVIZ_DXGI_INJECTIONPROBES_HPP

// Three independently queried probes through ONE output path, to reduce
// "the triangle is not visible" to exactly one cause (PLAT-RE155,
// docs/architecture/api/HANDOFF-OverlayPatch.md stage 5).
//
// ⚠️ THE CAMERA IS FINISHED AND THIS FILE ASSUMES IT. Run thirty-five's
// occurrence table settled it: within the selected group, occurrences 0 and 1
// project the orbit target at **100% inside the clip volume with a median error
// of 0.003**, and occurrences 2 to 5 at **0%**. The group, the occurrence, the
// interpretation and the snapshot are frozen. What remains is not "which
// camera" but "where does the pixel go", and that has three possible answers
// which no single triangle can distinguish.
//
// So three primitives are drawn at every Present, through the SAME back-buffer
// render target, the SAME explicit raster state, and each with its OWN occlusion
// query:
//
//   A  RASTER / OUTPUT     clip-space triangle near the screen centre, generated
//                          from `SV_VertexID` alone. No vertex buffer, no input
//                          layout, no constant buffer, no camera. It exercises
//                          the render target, viewport, rasterizer, pixel shader
//                          and colour write and nothing else.
//   B  SHADER / CONSTANTS  the three world vertices generated from
//                          `SV_VertexID` inside the vertex shader, then
//                          transformed by the production camera from the
//                          production `b1`/`b2` snapshots. No vertex buffer, no
//                          input layout.
//   C  PRODUCTION          the existing path exactly: vertex buffer, input
//                          layout, camera shader, camera snapshots.
//
// ⚠️ PROBE A IS NOT THE OLD GREEN CORNER WEDGE, AND THE DIFFERENCE IS THE POINT.
// The wedge has its own pixel shader and its own vertex buffer, so its being
// visible has never proved anything about the path the magenta triangle takes.
// Probe A uses the SAME pixel shader as the world triangle and no vertex buffer
// at all.
//
// ⚠️ AND PROBE B IS WHAT MAKES `ShaderInterpretation () == 2` MORE THAN
// METADATA. That number is currently an assertion about a shader nobody has
// checked: it says the compiled HLSL reads `b2` transposed. Probe B puts known
// world coordinates through the compiled shader and the real constant-buffer
// binding, so its sample count is the first direct evidence that the compiled
// matrix semantics are what the census measured.
//
// The decision table the three sample counts produce:
//
//     A = 0                  the output path is broken -- render target,
//                            viewport, rasterizer, pixel shader, write mask
//     A > 0, B = 0           the camera shader or constant binding is broken,
//                            NOT the census and NOT the camera selection
//     A > 0, B > 0, C = 0    input assembly: vertex buffer or vertex format
//     A > 0, B > 0, C > 0    it genuinely rendered; the fault is after the draw
//                            -- Present ordering or back-buffer replacement
//
// THREAD SAFETY. Everything here runs on Archicad's render thread inside the
// Present detour, under `ScopedInjectionGuard`. The queries are polled with
// `DONOTFLUSH` and never waited for; a query whose result has not arrived is
// simply not counted yet.

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11DeviceContext1;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace injection {
namespace probes {

// ⚠️ PROOF A IS CLOSED. Probes A, B and C all rasterised and both world
// triangles composited stably through navigation, so the camera, the transform,
// the snapshot, the back-buffer selection and the input assembly are FROZEN. The
// four probes below belong to Proof B, which asks one question and no others:
// can a Tapioca primitive use ARCHICAD'S OWN depth buffer, so geometry in front
// of the model survives and geometry behind it is rejected?
//
// ⚠️ AND IT IS ANSWERED WITH A CONTROL, NOT BY LOOKING FOR AN INTERSECTION. Two
// primitives at known depths, each drawn twice -- once with depth off and once
// with depth on -- turn "does occlusion look right" into four numbers:
//
//     depth OFF   both must rasterise        (they are on screen at all)
//     depth ON    front survives, behind is rejected
//
// A pair that rasterises with depth off and behaves differently with depth on is
// depth working. Anything else is named by which of the four is wrong.
enum class Probe : uint32_t {
    RasterOutput = 0,
    CameraShader = 1,
    Production = 2,
    FrontDepthOff = 3,
    BehindDepthOff = 4,
    FrontDepthOn = 5,
    BehindDepthOn = 6,

    // ⚠️ PROOF B2: THE SAME DEPTH TEST, BUT AT PRESENT. Proof B passed inside
    // Archicad's model pass, which only runs while the model is being
    // re-rendered -- so the depth-tested primitives appear during navigation and
    // vanish at rest, and later draws in the same frame paint over them. Present
    // injection has the opposite problem: it is always visible and has no depth.
    //
    // These two ask whether we can have both: bind the model's OWN depth view
    // alongside the back buffer at Present, and test against it there. If the
    // view is still alive and still holds the model's depth at that point, the
    // overlay is both persistent and correctly occluded.
    PresentFrontDepth = 7,
    PresentBehindDepth = 8,
};
constexpr size_t kProbeCount = 9;

// MAIN THREAD. The world point probe B generates its vertices around, and the
// side length. Baked into the probe shader at compile time, which is why
// changing it forces a recompile -- there is no constant buffer of ours to bind,
// deliberately, because binding one would be another thing that could be wrong.
void SetAnchor (float x, float y, float z, float sizeMetres);

// RENDER THREAD, from the Present injection, with the back-buffer view it just
// created. Sets its own render target, viewport and raster state, draws all
// three probes with their own queries, and restores everything it touched.
void DrawAll (ID3D11DeviceContext* context, ID3D11DeviceContext1* context1,
              ID3D11RenderTargetView* targetView, float viewportX, float viewportY,
              float viewportWidth, float viewportHeight);

// RENDER THREAD, from the census, at a draw of the SELECTED camera signature
// that is predicted to be the last of its model frame -- so Archicad's depth
// buffer holds as much of the model as it is going to.
//
// ⚠️ IT USES WHATEVER RENDER TARGET AND DEPTH VIEW ARE BOUND RIGHT NOW, which
// are Archicad's own, and it neither clears depth nor writes to it. Depth test
// ON, depth WRITES OFF: the experiment can read Archicad's depth but can never
// change a pixel of it, so a failed Proof B cannot corrupt the frame it is
// measured in.
void DrawDepthProof (ID3D11DeviceContext* context, ID3D11DeviceContext1* context1);

// RENDER THREAD, from the census, at the depth proof: keeps a reference to the
// depth view the model pass was using, so Present can try to test against it.
//
// ⚠️ A REFERENCE, NOT A COPY, AND IT IS RELEASED WHEN REPLACED. Holding a view
// of a resource Archicad may resize is exactly the mistake the back-buffer path
// refuses to make, so this one is dropped at teardown and on every resize path
// that reaches `Shutdown`.
void RetainSceneDepthView (ID3D11DepthStencilView* view);

// RENDER THREAD, from the Present injection, with the back-buffer view it just
// created. Binds that target together with the RETAINED model depth view and
// draws the two test primitives with depth test on and writes off.
void DrawPresentDepth (ID3D11DeviceContext* context, ID3D11DeviceContext1* context1,
                       ID3D11RenderTargetView* targetView, float viewportX,
                       float viewportY, float viewportWidth, float viewportHeight);

// MAIN THREAD. Where the two depth test primitives sit relative to the anchor,
// along world Z. ⚠️ THESE ARE AN ASSUMPTION ABOUT THE MODEL AND ARE MEANT TO BE
// CHANGED: "in front of" and "behind" only mean anything against the geometry
// that is actually there.
void SetDepthOffsets (float frontOffsetZ, float behindOffsetZ);

// RENDER THREAD. Probe C is the production draw itself, so the renderer wraps
// its own draw with these rather than this file repeating it -- a second copy of
// the production path would not be the production path.
void BeginProductionQuery (ID3D11DeviceContext* context);
void EndProductionQuery (ID3D11DeviceContext* context);

// The production camera vertex shader's bytecode hash, handed over by the
// renderer that compiled it, so the report can print C's shader beside B's.
void SetCameraVsHash (uint64_t hash);

// MAIN THREAD, at teardown. Nothing here may outlive Archicad's device.
void Shutdown ();
void Reset ();

struct ProbeStats {
    uint64_t draws = 0;          // times the probe was issued
    uint64_t queriesIssued = 0;
    uint64_t queriesResolved = 0;
    uint64_t drawsWithSamples = 0;
    uint64_t totalSamples = 0;
};

struct Stats {
    bool     ready = false;
    ProbeStats probe[kProbeCount];

    // ⚠️ THE BYTECODE HASHES ARE EVIDENCE, NOT DECORATION. "Probe B and the
    // production triangle use the same vertex shader" is an assumption until the
    // two hashes are printed side by side -- and if probe B passes while C fails,
    // the first question is whether they were the shaders we think they were.
    uint64_t cameraVsHash = 0;      // VSMain, used by probe C
    uint64_t probeVsHash = 0;       // VSProbeB
    uint64_t rasterVsHash = 0;      // VSProbeA

    // ⚠️ THE DEPTH VIEW'S LIFETIME, TRACKED ONLY FOR THE SELECTED CANDIDATE.
    // Censusing every depth operation in the frame would be a second instrument
    // to debug; the only question is what happens to THIS view between the model
    // draw and the moment we use it.
    uint64_t depthInjections = 0;
    uint64_t depthNoView = 0;        // nothing was bound: the point is wrong
    uint64_t depthViewChanged = 0;   // a different view than the camera draw had
    uint64_t lastDepthView = 0;

    // Proof B2's own health: whether a retained view existed at Present at all.
    uint64_t presentDepthDraws = 0;
    uint64_t presentDepthNoView = 0;
    char     lastError[192] = {};
};
Stats GetStats ();

}   // namespace probes
}   // namespace injection
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv

#endif
