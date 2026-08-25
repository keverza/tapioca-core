#ifndef EVP_PLANOVERLAY_OVERLAYWINDOW_HPP
#define EVP_PLANOVERLAY_OVERLAYWINDOW_HPP

// The click-through overlay window that sits over Archicad's floor-plan canvas.
//
// Phase 0 established what this has to be, and every part of it is load-bearing.
// Do not simplify one away without re-reading archive/docs/bgfx-archviz-plan.md §14:
//
//   WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS
//   WS_EX_LAYERED    — WITHOUT THIS Archicad's canvas overdraws the window within
//                      ~0.4 s of any redraw, and NEITHER WS_CLIPSIBLINGS nor
//                      WS_CLIPCHILDREN on the canvas prevents it. The canvas
//                      presents an accelerated surface and ignores GDI clip
//                      regions; only DWM composition beats it.
//   WS_EX_TRANSPARENT — with WM_NCHITTEST -> HTTRANSPARENT, keeps us out of the
//                      mouse's hit chain so Archicad's tools keep working.
//   WS_EX_NOACTIVATE — never steal focus, never appear in Alt-Tab.
//
// It is created as a SIBLING of the canvas under the floor-plan document window,
// covering the canvas rect. Attaching as a CHILD of the canvas does not work: it
// never receives a WM_PAINT at all (measured, paintCount 0).
//
// ⚠️ Everything here is MAIN-THREAD ONLY. The WndProc runs on Archicad's UI
// thread, which is what makes it legal for the tracking timer to call ACAPI at
// all. Nothing in this file may be called from a worker.

#include <vector>
#include <memory>

#include <windows.h>

#include "Annotation/DrawList.hpp"
#include "PlanOverlay/OverlayOwnership.hpp"

namespace geomsrv {
namespace planoverlay {

struct Style {
    bool layered = true; // see the header comment — effectively mandatory
    int alpha = 140;     // 0-255. 255 hides the plan and everything drawn on it
    bool hatch = true;   // keyed-out interior + band/cross, instead of a fill
};

struct Point2 {
    double x = 0.0;
    double y = 0.0;
};

// Model-space geometry the overlay draws, re-projected every time the view moves.
using Polyline = std::vector<Point2>;

// The affine the tracker derived, model -> client pixels, plus how it got there.
struct Transform {
    bool valid = false;
    double scaleX = 0.0, scaleY = 0.0;
    double offX = 0.0, offY = 0.0;
    // The two reference points it was derived from, as returned by
    // ACAPI_View_CoordToPoint. Reported so a wrong projection is diagnosable
    // instead of merely wrong.
    Point2 refModelA, refModelB;
    Point2 refPointA, refPointB;

    // Which window's client space does the projection speak in? The DevKit says
    // "the local coordinates of the window" without saying which one, and
    // guessing put the geometry in the wrong place once. impliedW/H come from
    // pushing the zoom box through the affine — that is the drawing window's
    // pixel size. If it matches canvasW/H, the canvas is the space.
    double impliedW = 0.0, impliedH = 0.0;
    double canvasW = 0.0, canvasH = 0.0;

    // ⚠️ DISPLAY SCALING. Archicad's API_Point is in LOGICAL pixels; our window's
    // client rect is in PHYSICAL ones. At 150% scaling everything drawn from an
    // uncorrected affine is 0.667x too small AND pans at two-thirds the speed of
    // the plan — which is what "the grid moves at a different speed than the
    // drag" was.
    //
    // The factor is MEASURED, not asked of the DPI API: canvasW / impliedW. That
    // is self-calibrating, so it is right regardless of the process's DPI
    // awareness mode, which monitor the window is on, or what Archicad does
    // internally — and it follows the window if it is dragged to another screen.
    double dpiX = 1.0, dpiY = 1.0;
    bool dpiApplied = false;
};

// One row of the calibration sweep: what the projection would look like if it
// were expressed in THIS window's client space.
struct CalibRow {
    const char* label = "";
    double clientW = 0.0, clientH = 0.0;
    double impliedW = 0.0, impliedH = 0.0;
    double kx = 0.0, ky = 0.0; // clientW/impliedW — the scale factor implied
    double disagree = 0.0;     // |kx-ky|/kx — 0 means this window is the space
};

struct TrackStats {
    bool tracking = false;
    UINT intervalMs = 0;
    long polls = 0;      // timer ticks
    long recomputes = 0; // ticks where the transform actually changed
    long repaints = 0;
    long acapiFailures = 0;
    Transform transform;
};

// --- lifecycle -------------------------------------------------------------

// `canvas` is the window the transform is expressed in — the plan canvas the
// overlay covers. NOT `parent`: the overlay is a SIBLING of the canvas, so its
// parent is the document window and using that offsets every pinned coordinate.
HWND Create (HWND parent, HWND canvas, const RECT& rectInParent, const Style& style, Owner owner);
void DestroyAll ();
bool DestroyOwned (Owner owner);
HWND Current ();
HWND Canvas ();
Owner CurrentOwner ();

// Identifies this module's overlay without repeating its window-class contract
// in every host-discovery caller.
bool IsOverlayWindow (HWND window);

// Shared Win32 descent used by the supported host opener and the native window
// inspection command. excludeOverlay reveals Archicad's underlying canvas.
std::vector<HWND> DescendWindowChain (HWND root, POINT screenPoint, bool skipTransparent, bool excludeOverlay);

// Destroy every window we created and unregister the class. MUST run from
// FreeData: a window whose WndProc lives in this DLL outliving the unload takes
// Archicad down on exit. That is not theoretical — it crashed on close once.
void Shutdown ();

// --- diagnostics -----------------------------------------------------------

LONG PaintCount ();
DWORD LastPaintTick ();

// --- content ---------------------------------------------------------------

// Replace the model-space geometry. Empty restores the band/cross test pattern,
// which is what "is the overlay alive" is measured against.
void SetGeometry (const std::vector<Polyline>& polylines);

// A watch frame is an independent layer over legacy geometry. Passing null
// clears only that layer, so Return never destroys command-owned polylines.
void SetAnnotationFrame (std::shared_ptr<const annotation::Frame> frame);

// --- tracking --------------------------------------------------------------

// Start/stop the pan+zoom poll. There is NO notification for pan or zoom in the
// DevKit — the window tree does not change and no callback fires — so a poll is
// the only mechanism available. See §15.
void SetTracking (bool enable, UINT intervalMs);
TrackStats GetTrackStats ();

// Recompute the transform once, now, and report it. Used by the probe to
// establish the projection convention before anything is built on it.
Transform ComputeTransform ();

// Sweep the candidate reference windows in ONE pass and report each one's
// implied scaling. The window the projection is really expressed in is the one
// whose horizontal and vertical factors AGREE — a wrong window disagrees between
// the two axes, because it differs from the right one by unequal insets.
std::vector<CalibRow> Calibrate ();

} // namespace planoverlay
} // namespace geomsrv

#endif
