#ifndef EVP_ARCHVIZ_DXGI_PRELOCKHUD_HPP
#define EVP_ARCHVIZ_DXGI_PRELOCKHUD_HPP

// ArchViz/Dxgi/PrelockHud -- the 3D overlay's HUD before its camera is chosen (the user,
// 2026-10-03: the dock's circle says what the overlay is waiting for -- it blinks when the user
// must orbit the view -- so the HUD has to be on screen before there is a camera to draw with).
//
// ⚠️ READ OVERLAY-INVARIANTS.md BEFORE EDITING. §2: composed at Present, in the Present detour,
// right after `InjectAtPresent` declines for want of a camera. §11: inside a ScopedPipelineState
// and the injection guard, the back buffer fetched and released within the call, nothing of
// Archicad's held. §1 and §12: NO CAMERA IS READ, DECODED OR BOUND -- the HUD is glyph quads
// fixed to the view, and the guest fills its pipelines' camera windows with zeros of its own
// (sceneguest::DrawHudOnly). The census, the recognizer and the snapshot buffers never see it.
//
// ⚠️ ONLY WHILE THE INJECTION WAITS FOR A CAMERA (`ArmedPendingCamera`). Once one is chosen the
// composer draws the HUD with everything else, exactly as before; disabled, nothing draws.
//
// ⚠️ IT ASKS FOR NO FRAME. It draws when Archicad presents -- the cold start's redraws, the
// user's navigation -- and the HUD's heartbeat redraws only once the camera is locked: a HUD
// that redrew the 3D window to blink would spend the redraw budget behind its back.
//
// RENDER THREAD, from the Present detour. No locks, no allocation, no ACAPI.

struct ID3D11DeviceContext;
struct IDXGISwapChain;

#include <cstdint>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace prelockhud {

void DrawAtPresent (ID3D11DeviceContext* context, IDXGISwapChain* swapChain);

// ⚠️ TOTALS SINCE THE PROCESS STARTED: a question about now is two readings and their
// difference (§7). Every decline is counted by its reason.
struct Stats {
    uint64_t drawn = 0;
    uint64_t nothing = 0;   // no HUD published yet
    uint64_t reentrant = 0; // inside an injection of our own
    uint64_t noTarget = 0;  // the back buffer or its view could not be had
    uint64_t failed = 0;    // the guest could not attach, build or upload
};
Stats GetStats ();

} // namespace prelockhud
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
