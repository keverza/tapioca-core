#ifndef EVP_ARCHVIZ_DXGI_PLANGUEST_HPP
#define EVP_ARCHVIZ_DXGI_PLANGUEST_HPP

// ArchViz/Dxgi/PlanGuest -- the floor plan's Diligent guest: what the 2D overlay
// draws through Diligent (§12b) -- texts, dimensions, legends, dashed polylines,
// styled and heatmap meshes -- over the walls and the plain layers, in the same
// Present, with the same transform.
//
// ⚠️ READ OVERLAY-INVARIANTS.md §11, §12, §12b AND FINDINGS 13-14 FIRST.
//
// ⚠️ THE PLAN'S PRESENT IS ON THE MAIN THREAD AND MAY NOT ALLOCATE (§11). So every
// Diligent object -- the attach, the pipelines, the buffers, the atlas pages -- is
// made by `Prepare`, which the plan runtime's tick calls OUTSIDE any Present. `Draw`
// binds what exists, writes two small constant buffers and draws. The plan's device
// is the one the plan layer learned from the canvas's chain.
//
// ⚠️ THE TRANSFORM IS THE PRESENT'S (finding 14). `Draw` takes the transform the
// plan layer read at this Present and builds the frame's constants from it with the
// walls' own arithmetic (plancontent::MakeViewConstants): a label is exactly where
// its wall is, in the frame the wall is in.
//
// ⚠️ INERT UNTIL SOMETHING NEEDS IT. With no guest content nothing attaches, nothing
// is built and `Draw` returns at its first test -- the plan overlay is then exactly
// the committed one.
//
// MAIN THREAD ONLY.

#include "ArchViz/OverlayHitMap.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayLayers.hpp"
#include "ArchViz/PlanOverlayContent.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace planguest {

// Outside any Present: attach, build and upload when `generation` moved -- the scene,
// and the HUD panels as a stream of their own with their own page cache, drawn after
// it. `changed` says the picture changed and the plan wants a redraw. False with `error` when
// something could not be made; the next call tries again.
// `input` is the plan canvas and the pointer over it, for the HUD (OverlayInput.hpp).
bool Prepare (ID3D11Device* device, const std::vector<std::shared_ptr<const overlaylayers::Layer>>& layers,
              uint64_t generation, float dpiScale, const overlayhud::Input& input, bool& changed, std::string& error);

// The HUD alone, laid out again for `input` -- the pointer did something to it. Outside
// any Present, like `Prepare`; `changed` when what it draws changed and the plan wants a
// redraw. Nothing before `Prepare` has attached.
bool RefreshHud (const std::vector<std::shared_ptr<const overlaylayers::Layer>>& layers, const overlayhud::Input& input,
                 bool& changed, std::string& error);

// Whether there is anything to draw -- the plan layer's `NoContent` test.
bool HasContent ();

// Where the legends and panels drawn now are on the plan (OverlayHitMap.hpp).
overlayinput::HitMap HitMap ();

// Inside the plan layer's Present draw, inside its ScopedPipelineState, after its
// own draws; `target` is the view that draw made of the back buffer.
void Draw (ID3D11DeviceContext* native, ID3D11RenderTargetView* target, const plancontent::PixelTransform& transform,
           uint32_t width, uint32_t height);

// Everything, device objects and attachment. The plan overlay's stop (§8).
void Release ();

struct Stats {
    bool attached = false;
    uint32_t attachMilliseconds = 0;
    uint32_t buildMilliseconds = 0;
    uint64_t uploads = 0;
    uint64_t draws = 0;
    uint64_t drawCalls = 0;
    uint64_t declinedNoTransform = 0;
    uint32_t fills = 0;
    uint32_t lines = 0;
    uint32_t glyphVertices = 0;
    uint32_t pages = 0;
    uint32_t textsNotLaidOut = 0;
    uint32_t dimensionsNotResolved = 0;
    uint32_t truncated = 0;
    std::string lastError;
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
    // The HUD's stream (overlayscene::PreparePlanHud): its uploads, what it holds now and
    // what laying it out cost.
    uint64_t hudUploads = 0;
    uint32_t hudGlyphVertices = 0;
    uint32_t hudPrepareMicroseconds = 0;
};
Stats GetStats ();

} // namespace planguest
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
