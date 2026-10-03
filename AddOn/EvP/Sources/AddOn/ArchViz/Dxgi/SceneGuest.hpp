#ifndef EVP_ARCHVIZ_DXGI_SCENEGUEST_HPP
#define EVP_ARCHVIZ_DXGI_SCENEGUEST_HPP

// ArchViz/Dxgi/SceneGuest -- the 3D window's Diligent guest: what the 3D overlay
// draws through Diligent (§12b) -- texts, dimensions, legends, dashed and
// depth-styled polylines, styled and heatmap meshes, the storey slices -- after the
// reference wireframe and the plain layers, in the same Present, with the same camera.
//
// ⚠️ READ OVERLAY-INVARIANTS.md §1 (FINDINGS 1-2), §11, §12 AND §12b FIRST.
//
// ⚠️ THE CAMERA IS THE CENSUS'S COPY, READ BY `ArchicadClip` (finding 1). The
// pipelines are composed per camera slot by `camerashader::Compose`, exactly as the
// wireframe's are, and they read the injection's own snapshot buffers -- wrapped for
// Diligent, bound by name. Nothing here decodes, latches or chooses a camera.
//
// ⚠️ THE MAIN THREAD PREPARES, THE RENDER THREAD UPLOADS AND DRAWS. `Publish` hands
// over one prepared scene with one atomic exchange -- the host occluder's pattern,
// never a lock. The render thread takes it at its next Present, attaches once (the
// frame it costs is counted), composes and builds the pipelines for the slot in use
// once, uploads once per change, and otherwise only binds and draws.
//
// ⚠️ DEPTH IS THE HOST OCCLUDER'S, TESTED AND NEVER WRITTEN, and the depth view the
// composer settled on is bound natively beside Archicad's target. No depth view: every
// item is drawn whole -- the wireframe's own rule when no occluder ran.
//
// ⚠️ INERT UNTIL SOMETHING NEEDS IT: with nothing published, `Draw` is one atomic
// exchange and a return.

#include "ArchViz/OverlayScene.hpp"

#include <cstdint>

struct ID3D11DepthStencilView;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace sceneguest {

// MAIN THREAD. What to draw next; an empty scene clears it. `dpiScale` is the view's.
void Publish (overlayscene::Scene scene, float dpiScale);

// MAIN THREAD. The HUD's stream (overlayscene::PrepareSceneHud): uploaded apart from the
// scene and drawn after it, so a panel that changes never sends the scene again.
void PublishHud (overlayscene::Scene hud, float dpiScale);

// RENDER THREAD, from the composer, inside the injection's ScopedPipelineState, with
// the scene viewport bound and the composer's target and depth view.
void Draw (ID3D11DeviceContext* context, uint32_t interpretation, ID3D11RenderTargetView* target,
           ID3D11DepthStencilView* depth);

// ⚠️ RENDER THREAD, AT PRESENT, BEFORE A CAMERA IS CHOSEN (Dxgi/PrelockHud.hpp): the HUD's stream
// alone -- glyph quads fixed to the view, which read no camera -- into `target`, `width` x
// `height`, inside the caller's ScopedPipelineState with the viewport bound. A neutral camera of
// the guest's own fills the pipelines' camera windows; nothing of the census is read or bound.
enum class HudOnly : uint8_t { Drawn, Nothing, NoTarget, Failed };
HudOnly DrawHudOnly (ID3D11DeviceContext* context, ID3D11RenderTargetView* target, float width, float height);
// Any thread: the HUD was drawn alone within the last `withinMilliseconds` -- it is on screen.
bool HudOnlyRecently (uint32_t withinMilliseconds);

// Once no Present can reach it (the composer's Shutdown). Keeps what was published.
void ReleaseDeviceObjects ();

struct Stats {
    bool attached = false;
    uint32_t attachMilliseconds = 0;
    uint32_t buildMilliseconds = 0;
    uint32_t slot = 0;
    uint64_t draws = 0;
    uint64_t drawCalls = 0;
    uint64_t uploads = 0;
    uint64_t declinedNoCamera = 0;
    uint64_t declinedNoViewport = 0;
    uint64_t declinedFailed = 0; // attach or build failed: nothing guest-drawn this session
    uint32_t fills = 0;
    uint32_t lines = 0;
    uint32_t glyphVertices = 0;
    uint32_t pages = 0;
    const char* failure = ""; // a static string: what failed, when something did
    const char* adapter = ""; // the GPU Archicad's device is on, once the guest has attached
    // What the content costs: preparing it on the main thread (layers built and reused,
    // OverlayScene.hpp), the bytes it holds on the GPU, and drawing it at Present on the
    // render thread -- the last draw, and a running mean over about the last sixteen.
    uint32_t prepareMicroseconds = 0;
    uint32_t layersBuilt = 0;
    uint32_t layersReused = 0;
    uint64_t vertexBytes = 0;
    uint64_t pageBytes = 0;
    uint32_t lastDrawMicroseconds = 0;
    uint32_t drawMicroseconds = 0;
    // The HUD's stream: its uploads, what it holds now and what laying it out cost.
    uint64_t hudUploads = 0;
    uint64_t hudOnlyDraws = 0; // the HUD drawn alone, before a camera was chosen
    uint32_t hudGlyphVertices = 0;
    uint32_t hudPrepareMicroseconds = 0;
};
Stats GetStats ();

} // namespace sceneguest
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
