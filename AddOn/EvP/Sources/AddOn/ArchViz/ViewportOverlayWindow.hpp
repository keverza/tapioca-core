#ifndef EVP_ARCHVIZ_VIEWPORTOVERLAYWINDOW_HPP
#define EVP_ARCHVIZ_VIEWPORTOVERLAYWINDOW_HPP

// ArchViz/ViewportOverlayWindow — the transparent, click-through window the
// Diligent viewport draws into when it is running as an OVERLAY over Archicad's
// own 3D view (PLAT-RE37).
//
// ⚠️ IT IS A TOP-LEVEL POPUP, NOT A DG PALETTE CHILD, AND THAT IS AN
// ARCHITECTURE DECISION RATHER THAN A STYLE FLAG. Three things force it:
//
//   PER-PIXEL ALPHA. A DG UserItem's swap chain is presented to a window with a
//   GDI redirection bitmap, and DWM composites THAT -- a flip-model chain's
//   alpha never reaches the compositor. WS_EX_NOREDIRECTIONBITMAP plus a
//   DirectComposition visual is the supported way, and it needs a window whose
//   styles we own. TransparencyProbe's `--paths=dcomp` measured this.
//
//   CLICK-THROUGH ACROSS A PROCESS BOUNDARY. WM_NCHITTEST -> HTTRANSPARENT is
//   documented to pass the message to underlying windows *in the same thread*.
//   Archicad's 3D canvas is the same process but the rule is about hit-testing,
//   and the measured answer (archive/experiments/TransparencyProbe/InputRouting.cpp, three
//   consecutive sweeps) is that a TOP-LEVEL LAYERED window is the configuration
//   that actually passes the mouse through. Every layered arm passed the
//   synthetic click; every non-layered one ate it.
//
//   Z-ORDER OVER ANOTHER WINDOW'S CONTENT. A palette child cannot be positioned
//   over the document window's canvas at all.
//
// ⚠️ WS_EX_LAYERED IS HERE FOR INPUT, NOT FOR PIXELS. No UpdateLayeredWindow call
// is made or wanted -- DirectComposition supplies every pixel. The layered
// attribute is borrowed purely for its hit-testing rule, and it coexists with
// NOREDIRECTIONBITMAP (measured, same sweep). Removing either one breaks a
// different half of the feature.
//
// ⚠️ EVERYTHING HERE IS MAIN-THREAD ONLY. The WndProc runs on Archicad's UI
// thread, which is what makes it legal for the tracking timer to call ACAPI at
// all -- exactly the rule PlanOverlay/OverlayWindow.hpp carries, for the same
// reason. The RENDER thread only ever receives the HWND and the size.

#include <cstdint>
#include <string>

#include <windows.h>

namespace geomsrv {
namespace archviz {
// ⚠️ ITS OWN NAMESPACE, like geomsrv::planoverlay. `Create`, `Destroy` and
// `Stats` are the right names for what they do and are far too generic to sit
// next to DiligentViewport and ExtractionWorker at archviz scope.
namespace viewportoverlay {

struct OverlayTarget {
    bool valid = false;
    HWND window = nullptr;      // the Archicad window the overlay is covering
    RECT screenRect = {};       // its client area, in SCREEN pixels
    std::string windowClass;    // reported so a wrong target is diagnosable
    std::string how;            // how it was found, or why it was not
};

struct OverlayStats {
    bool  active = false;
    HWND  overlay = nullptr;
    HWND  target = nullptr;
    RECT  screenRect = {};
    uint32_t width = 0;
    uint32_t height = 0;
    long  trackPolls = 0;
    long  trackMoves = 0;       // polls where the target actually moved or resized
    std::string targetClass;
    std::string how;
    // ⚠️ REPORTED SO A NON-COMPOSITING MODE CANNOT BE RUN BLIND. The child
    // attach modes create a window and render into it and produce NOTHING on
    // screen (measured 2026-08-14, both paths, no error anywhere). A probe that
    // cannot see which mode is live will happily score an invisible overlay.
    int attach = 0;
};

// Which window the overlay should cover: the deepest child of Archicad's main
// window at the centre of its client area, skipping our own windows.
//
// ⚠️ IT IS WHATEVER DOCUMENT WINDOW IS FRONTMOST, AND THAT IS NOT CHECKED HERE.
// There is no DevKit call that hands back the 3D view's HWND, and inferring
// "this is the 3D window" from a class name would be a guess this repo has
// already been burned by. So the class name and rect are REPORTED and the
// caller (or the user, through the probe's NOW LOOK block) confirms that the 3D
// window was in front. An overlay over the floor plan is a wrong answer that
// says exactly what it did.
OverlayTarget FindOverlayTarget ();

// HOW the overlay window is attached, and it is a CHOICE because the z-order
// requirement and the visibility requirement pull against each other.
//
// ⚠️ THE REQUIREMENT (user, 2026-08-13): "No archicad callouts must be visible,
// overlay must not go over floating panels. Overlay must be just above the view
// it overlays."
//
// `Popup` cannot satisfy it. A top-level window has to be at HWND_TOP to be seen
// at all here -- inserting it one place lower made it INVISIBLE with occasional
// blips, measured -- and HWND_TOP is by definition above Archicad's callouts.
// The two cannot both be had while the overlay is top-level.
//
// A CHILD of the view window satisfies it STRUCTURALLY rather than by ordering:
// it is a sibling of the canvas so it draws above it, it is clipped to the view
// so it can never reach a docked panel, and every floating palette, tooltip and
// callout is a separate TOP-LEVEL window, which Windows always draws above the
// children of another top-level window. Nothing has to be re-asserted on a
// timer.
//
// ⚠️ ALL THREE MODES WERE CONFIRMED WORKING ON THE PLAN PATH (2026-08-13):
// overlay visible, Archicad's callouts visible, clicks reaching Archicad. That
// run settled the two things the child modes were built to bisect --
// DirectComposition DOES bind to a child HWND, and a LAYERED child DOES still
// pass the mouse through -- so both are now facts rather than open questions.
//
// ⚠️ AND IT CORRECTED THE DIAGNOSIS. The overlay hid Archicad's callouts because
// it was painting an OPAQUE 3D MODEL over the plan, not because of its z-order.
// A composition overlay is per-pixel alpha: wherever it draws nothing, Archicad
// shows through, callouts included, in ANY of these modes. Dropping the model
// (PLAT-RE65) is what fixed it. Two attempts were spent restacking the window
// when the answer was to stop drawing over it -- reach for the CONTENT before
// the window manager.
//
// The modes are kept because they are now a robustness choice with a measured
// answer behind each: a child is clipped to the view and needs no HWND_TOP
// re-assert, and the popup is the mode proven over the 3D window. See the
// handoff's "Overlay attach modes" table for which default belongs where.
// ⚠️ A CHILD attach over the 3D WINDOW is still unconfirmed; only the plan path
// was run.
enum class OverlayAttach {
    Popup = 0,              // today's behaviour: top-level, owned, HWND_TOP
    ChildLayered = 1,       // child of the view window, WS_EX_LAYERED kept
    ChildTransparent = 2,   // child of the view window, WS_EX_TRANSPARENT only
};

// Create the overlay over `target`, or return nullptr with `error` filled.
// Idempotent in the sense that it destroys any previous overlay first -- two of
// these fighting over the same rect is not a state worth supporting.
HWND Create (const OverlayTarget& target, OverlayAttach attach, std::string& error);

// Destroy the overlay and stop tracking. Safe when nothing is open.
void Destroy ();

// Unregister the window class. ⚠️ MUST RUN FROM FreeData: a window whose WndProc
// lives in this DLL outliving the unload takes Archicad down on exit. That is
// not theoretical -- PlanOverlay's did it once.
void Shutdown ();

HWND Current ();

// Show or hide the overlay window WITHOUT tearing it down (PLAT-RE79 phase 4).
//
// ⚠️ IT KEEPS RENDERING AND PRESENTING WHILE HIDDEN, and that is the point. In
// `hookdraw` the same frames are mirrored into Archicad's own back buffer, so
// leaving this window visible would draw the overlay TWICE, a few milliseconds
// apart -- which looks exactly like the ghosting the whole exercise is trying to
// remove. Blanking the viewport instead would not do: the mirror copies the
// rendered frame, so a blanked frame mirrors as nothing.
void SetVisible (bool visible);
OverlayStats Stats ();

// Follow the target window as it moves, resizes, or the user scrolls the
// document window. ⚠️ A POLL, BECAUSE THERE IS NO NOTIFICATION. Same mechanism
// and the same caveats as PlanOverlay's tracker and ArchVizPanel's camera sync:
// SetTimer(nullptr, ...) posts WM_TIMER to THIS thread, which is why it may
// touch windows at all, and WM_TIMER is LOW PRIORITY so a fast drag can starve
// it. Starvation is coarse tracking, not broken tracking.
//
// The callback is invoked with the new size whenever the target's client area
// changes, so the caller can resize its swap chain. ⚠️ IT RUNS ON THE MAIN
// THREAD; a render-thread resize must be queued, never done inline.
using ResizeCallback = void (*) (uint32_t width, uint32_t height);
void SetTracking (bool enable, UINT intervalMs, ResizeCallback onResize);

}   // namespace viewportoverlay
}   // namespace archviz
}   // namespace geomsrv

#endif
