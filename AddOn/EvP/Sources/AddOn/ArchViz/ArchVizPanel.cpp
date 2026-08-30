#include "ArchViz/ArchVizPanel.hpp"
#include "ArchViz/ArchVizLog.hpp"     // ArchVizLog — one log for the whole viewer
#include "ArchViz/CameraSyncMode.hpp" // the ONE way the camera sync is armed
#include "ArchViz/D3D12FeasibilityProbe.hpp"
#include "ArchViz/DiligentProbe.hpp"
#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/ExtractionThread.hpp"
#include "ArchViz/ExperimentGuard.hpp"
#include "ArchViz/InputRingBuffer.hpp"
#include "ArchViz/SceneCmdQueue.hpp"
#include "ArchViz/ModelWatch.hpp"
#include "ArchViz/ViewportCursor.hpp"
#include "ArchViz/SelectionBridge.hpp"
#include "ArchViz/ViewportOverlayWindow.hpp"
#include "ArchViz/ViewerHost.hpp"
#include "ArchViz/ViewerSettings.hpp" // SceneRenderMode -- the overlay starts in wireframe
#include "ResourceIds.hpp"

#include "DGWin.h" // DGGetDialogItemWindow / DGGetDPIForDialogItem

#include <cstdio>
#include <string>

// The viewer palette's identity. ACAPI keys the modeless window on its hash, so
// it must differ from ControlPalette's and must not change between sessions
// (placement and dock state are remembered against it).
extern const GS::Guid archVizPaletteGuid;
const GS::Guid archVizPaletteGuid ("{2C9D4E71-3A85-41F0-9B62-7D0E8A15C3B4}");

GS::Ref<ArchVizPanel> ArchVizPanel::instance;

namespace {

// The status line's margin, and the strip it occupies at the bottom. Local: the
// palette metrics in Palette/PaletteMetrics.hpp belong to the command palette's
// bands, and a constant moves to a shared header on its SECOND consumer, never
// speculatively (CLAUDE.md).
constexpr short kMargin = 8;
constexpr short kStatusHeight = 16;

// Main-thread only. The shared experiment guard must only be disarmed by the
// mechanism that armed it.
bool s_d3d12ProbeGuardArmed = false;

GS::UniString FromStd (const std::string& s)
{
    return GS::UniString (s.c_str (), CC_UTF8);
}

// Is the Diligent renderer up as the transparent OVERLAY (as opposed to the
// palette child)?
//
// ⚠️ TWO THINGS HANG OFF THIS AND BOTH WERE LIVE FAULTS (PLAT-RE57). The palette
// must not be SHOWN while an overlay runs -- an empty floating 3D panel beside a
// working overlay reads as the viewer having failed -- and PanelResized must not
// push the palette child's size into a swap chain that is sized to Archicad's
// window. Asked of the renderer rather than tracked in a member, because the
// renderer is the thing that knows, and a second copy of the answer is a second
// thing to get out of step.
bool OverlayRunning ()
{
    const geomsrv::archviz::DiligentViewportStats stats = geomsrv::archviz::DiligentViewport::Get ().Stats ();
    return stats.running && stats.overlay;
}

} // namespace

// ---------------------------------------------------------------------------
ArchVizPanel::ArchVizPanel ()
    : DG::Palette (ACAPI_GetOwnResModule (), ArchVizPaletteResId, ACAPI_GetOwnResModule (), archVizPaletteGuid),
      statusText (GetReference (), ArchVizStatusTextId)
{
    Attach (*this);
    BeginEventProcessing ();

    // The viewport. Created at runtime for the three reasons in the .grc comment;
    // the one that matters here is FrameType::NoFrame — a DG frame is drawn by DG,
    // over our surface.
    //
    // UserItemType::Normal (not Partial): we never ask DG for a partial update
    // because we never let DG paint at all.
    viewport =
        std::make_unique<DG::UserItem> (*this, DG::Rect (0, 0, 100, 100), DG::UserItem::Normal, DG::UserItem::NoFrame);
    viewport->Attach (*this);
    // A dynamically created DG item starts HIDDEN. This one has to be shown
    // BEFORE its HWND is asked for and handed to a renderer — a hidden item's
    // window is not a surface anything can present to.
    viewport->Show ();

    // Idle is opt-in; without it the status line never updates and an init
    // failure that happens a few hundred ms after Show() is invisible.
    EnableIdleEvent ();

    Layout ();
}

ArchVizPanel::~ArchVizPanel ()
{
    // ⚠️ ORDER. The render thread must be gone before DG destroys the viewport
    // item, or it presents to a freed HWND. StopRenderer JOINS.
    StopRenderer ();
    geomsrv::archviz::DiligentProbe::Get ().Stop ();
    EndEventProcessing ();
}

bool ArchVizPanel::HasInstance ()
{
    return instance != nullptr;
}

void ArchVizPanel::CreateInstance ()
{
    if (!HasInstance ()) {
        instance = new ArchVizPanel ();
        ACAPI_KeepInMemory (true);
    }
}

ArchVizPanel& ArchVizPanel::GetInstance ()
{
    return *instance;
}

void ArchVizPanel::DestroyInstance ()
{
    if (HasInstance ())
        GetInstance ().StopRenderer (); // before the DG objects go
    instance = nullptr;
}

// ⚠️ NOT WHILE AN OVERLAY IS RUNNING (PLAT-RE57). In overlay mode the palette's
// viewport child is empty BY CONSTRUCTION -- the picture goes to the overlay
// window -- so putting it on screen shows a blank floating 3D panel next to a
// working overlay, and the user reads that as the viewer having failed. Worse,
// the panel can then be RESIZED, which is how run 2 broke the overlay's aspect
// ratio with no way back.
//
// It is refused HERE rather than at each call site because the calls that
// surprise are the ones nobody writes: `APIPalMsg_HidePalette_End` re-shows every
// palette after any modal dialog closes, and the menu item that opens the viewer
// shows it too. The palette still EXISTS throughout -- it owns the status line,
// the DG events and the teardown that joins the render thread.
void ArchVizPanel::Show ()
{
    if (OverlayRunning ()) {
        geomsrv::archviz::ArchVizLog ("ArchViz palette: not shown -- the Diligent OVERLAY is "
                                      "running and its viewport child would be empty (PLAT-RE57)");
        return;
    }
    DG::Palette::Show ();
}

void ArchVizPanel::Hide ()
{
    DG::Palette::Hide ();
}

// ---------------------------------------------------------------------------
// The 3D viewer menu item, and the palette's own APIPalMsg_OpenPalette.
//
// ⚠️ IT STARTS THE DILIGENT VIEWPORT NOW. It used to call StartRenderer, which
// was bgfx; bgfx is gone (PLAT-RE66). Keeping this entry point rather than
// deleting it matters -- it is the menu item and the palette callback, so it is
// how a user opens a viewer without running a probe, and with the overlay's
// close box unreachable (PLAT-RE56) it is the only such route left.
void ArchVizPanel::OpenViewer ()
{
    OpenDiligentViewport ();
}

void ArchVizPanel::OpenDiligentProbe ()
{
    if (!HasInstance ())
        CreateInstance ();
    ArchVizPanel& panel = GetInstance ();
    if (s_d3d12ProbeGuardArmed) {
        panel.statusText.SetText ("RE51.D1 D3D12 probe is running; wait for it to stop.");
        return;
    }
    panel.Show ();
    if (geomsrv::archviz::DiligentProbe::Get ().Start (panel.ViewportWindow ()))
        panel.statusText.SetText ("Diligent Probe 1c running; check logs after it completes.");
    else
        panel.statusText.SetText ("Diligent Probe 1c did not start; check logs.");
}

bool ArchVizPanel::OpenD3D12FeasibilityProbe (std::string& error)
{
    namespace av = geomsrv::archviz;
    namespace vo = geomsrv::archviz::viewportoverlay;

    if (av::DiligentViewport::Get ().IsRunning ()) {
        error = "the production D3D11 viewport or overlay is running; close it before RE51.D1";
        av::D3D12FeasibilityProbe::Get ().SetRefusal (error);
        return false;
    }
    if (av::DiligentProbe::Get ().Stats ().running) {
        error = "the D3D11 device probe is running; wait for it before RE51.D1";
        av::D3D12FeasibilityProbe::Get ().SetRefusal (error);
        return false;
    }
    if (vo::Stats ().active) {
        error = "an overlay window is already active; close it before RE51.D1";
        av::D3D12FeasibilityProbe::Get ().SetRefusal (error);
        return false;
    }

    if (!HasInstance ())
        CreateInstance ();
    ArchVizPanel& panel = GetInstance ();

    // Find the document target before showing the palette can change which
    // window Windows considers frontmost.
    const vo::OverlayTarget target = vo::FindOverlayTarget ();
    std::string overlayError;
    HWND overlay = nullptr;
    if (target.valid)
        overlay = vo::Create (target, vo::OverlayAttach::Popup, overlayError);
    else
        overlayError = target.how.empty () ? "no frontmost document target was found" : target.how;

    panel.Show ();
    void* child = panel.ViewportWindow ();
    uint32_t childWidth = 0;
    uint32_t childHeight = 0;
    if (child == nullptr || !panel.ViewportPixelSize (childWidth, childHeight)) {
        vo::Destroy ();
        error = "the DG child HWND or its physical client size is unavailable";
        av::D3D12FeasibilityProbe::Get ().SetRefusal (error);
        return false;
    }

    uint32_t overlayWidth = 0;
    uint32_t overlayHeight = 0;
    if (overlay != nullptr) {
        const vo::OverlayStats overlayStats = vo::Stats ();
        overlayWidth = overlayStats.width;
        overlayHeight = overlayStats.height;
    }

    if (!av::experimentguard::Arm ("re51-d1-d3d12", error)) {
        vo::Destroy ();
        av::D3D12FeasibilityProbe::Get ().SetRefusal (error);
        return false;
    }
    s_d3d12ProbeGuardArmed = true;

    if (!av::D3D12FeasibilityProbe::Get ().Start (child, childWidth, childHeight, overlay, overlayWidth, overlayHeight,
                                                  overlayError, error)) {
        vo::Destroy ();
        av::experimentguard::Disarm ();
        s_d3d12ProbeGuardArmed = false;
        return false;
    }

    panel.statusText.SetText ("RE51.D1 D3D12 feasibility probe running; follow the command instructions.");
    return true;
}

void ArchVizPanel::CloseD3D12FeasibilityProbe ()
{
    geomsrv::archviz::D3D12FeasibilityProbe::Get ().Stop ();
    if (s_d3d12ProbeGuardArmed) {
        geomsrv::archviz::viewportoverlay::Destroy ();
        geomsrv::archviz::experimentguard::Disarm ();
        s_d3d12ProbeGuardArmed = false;
        if (HasInstance ())
            GetInstance ().statusText.SetText ("RE51.D1 D3D12 feasibility probe stopped.");
    }
}

void ArchVizPanel::OpenDiligentViewport ()
{
    if (!HasInstance ())
        CreateInstance ();
    ArchVizPanel& panel = GetInstance ();
    if (s_d3d12ProbeGuardArmed) {
        panel.Show ();
        panel.statusText.SetText ("RE51.D1 D3D12 probe is running; wait for it to stop.");
        return;
    }
    panel.Show ();

    if (geomsrv::archviz::DiligentViewport::Get ().IsRunning ())
        return;

    geomsrv::archviz::Surface surface;
    surface.nwh = panel.ViewportWindow ();
    if (surface.nwh == nullptr || panel.viewport == nullptr) {
        panel.statusText.SetText ("Diligent viewport has no usable HWND; see archviz.log.");
        return;
    }
    // The window's OWN client size -- the same space the cursor is measured in.
    // See ViewportPixelSize: sizing the swap chain by a second computation is
    // what made picking drift towards the edges of the view (PLAT-RE139).
    if (!panel.ViewportPixelSize (surface.width, surface.height)) {
        const double scale = panel.DisplayScale ();
        surface.width = uint32_t (panel.viewport->GetWidth () * scale);
        surface.height = uint32_t (panel.viewport->GetHeight () * scale);
        geomsrv::archviz::ArchVizLog (
            "viewport: the client rect could not be read; falling back to logical size x DPI, "
            "which may make picking drift towards the edges of the view");
    }

    // ⚠️ ACAPI ON THE MAIN THREAD, WHICH IS WHERE WE ALREADY ARE. This handler
    // runs through MainThreadGate::Post, so the 3D window's projection can be
    // read here directly; the render thread may never ask for it.
    const geomsrv::archviz::CameraStart cameraStart = ReadArchicadCamera ();

    if (geomsrv::archviz::DiligentViewport::Get ().Start (surface, cameraStart)) {
        panel.statusText.SetText ("Starting Diligent D3D11 viewport...");
        // The pointer keeps whatever shape it had when it crossed in -- entering
        // over the splitter leaves a resize arrow on the 3D view. See
        // ViewportCursor: nobody answers WM_SETCURSOR for a DG::UserItem.
        geomsrv::archviz::viewportcursor::Attach (surface.nwh);
        // The geometry the user actually wants to look at. LIVE, not a one-shot
        // pass. The producer side (ExtractionWorker -> SceneCmdQueue) is
        // renderer-free by design, which is what let the viewer change underneath
        // it without the extraction path being touched at all.
        geomsrv::archviz::ExtractionWorker::Get ().StartLive ();

        // ---- THE PANEL'S THREE DEFAULTS, ALL SET IN ONE PLACE ---------------
        //
        // ⚠️ THIS IS THE PANEL'S CONTRACT, AND IT IS DELIBERATELY DIFFERENT FROM
        // THE OVERLAY'S (PLAT-RE124/RE125). The overlay exists to AGREE with
        // Archicad, so it follows Archicad's camera continuously and lives or
        // dies on register. The panel exists to be LOOKED THROUGH: it takes
        // Archicad's viewpoint once as a starting position and is then a
        // standalone viewer the user navigates themselves.
        //
        //   camera     one sync at open (the CameraStart above), never after.
        //              Enforced in DiligentViewport::SyncCamera, not here.
        //   geometry   follows edits through the difference generator, which
        //              writes NOTHING to the project -- unlike the per-element
        //              observers that had to be turned off (PLAT-RE68).
        //   selection  ToViewer ONLY. Archicad's selection tints in the viewer
        //              so the user can see what they have picked, and a click in
        //              the viewer NEVER writes back: picking here is for
        //              inspection. SelectionBridge's own header anticipated
        //              exactly this pairing, which is why the two directions are
        //              separate flags rather than one on/off.
        geomsrv::archviz::modelwatch::Start ();
        geomsrv::archviz::selectionbridge::Start (geomsrv::archviz::selectionbridge::ToViewer);
    }
    else {
        panel.statusText.SetText ("Diligent viewport did not start; see archviz.log.");
    }
}

// ---------------------------------------------------------------------------
// The overlay (PLAT-RE37).
namespace {

// The overlay's swap chain, resized from the tracking timer. ⚠️ A FREE FUNCTION
// BECAUSE THE TRACKER TAKES A PLAIN FUNCTION POINTER, deliberately: a
// std::function capturing the panel would be a main-thread object reachable from
// a timer that outlives it, which is the bug class StopRenderer's unconditional
// KillTimer exists for.
void OnOverlayResized (uint32_t width, uint32_t height)
{
    geomsrv::archviz::ArchVizLog ("ArchViz overlay resized to " + std::to_string (width) + "x" +
                                  std::to_string (height));
    geomsrv::archviz::DiligentViewport::Get ().RequestResize (width, height);
}

// How often the overlay re-reads the window it covers, and how often the camera
// is pushed. ⚠️ THE SAME ORDER OF MAGNITUDE ON PURPOSE: a camera that follows at
// 60 Hz over a rectangle that follows at 4 Hz would swim against the window
// during a pan of the application itself.
constexpr uint32_t kOverlayTrackMs = 33;
// ⚠️ 15, NOT 16, AND THE DIFFERENCE IS A FACTOR OF TWO. Windows generates
// WM_TIMER on the system tick, 15.625 ms. A 16 ms request overshoots the first
// tick by 0.4 ms and waits for the second: 31.25 ms. Measured over three matrix
// runs (2026-08-13): 16 ms -> p50 ~31 ms, 15 ms and 10 ms -> p50 ~16 ms. Asking
// for less than 15 buys nothing (and SetTimer clamps below USER_TIMER_MINIMUM,
// 10 ms); asking for 16 halves the sample rate for nothing.
constexpr uint32_t kOverlayCameraSyncMs = 15;

} // namespace

void ArchVizPanel::OpenDiligentOverlay (int attach)
{
    if (!HasInstance ())
        CreateInstance ();
    ArchVizPanel& panel = GetInstance ();
    if (s_d3d12ProbeGuardArmed) {
        panel.Show ();
        panel.statusText.SetText ("RE51.D1 D3D12 probe is running; wait for it to stop.");
        return;
    }
    // ⚠️ THE PALETTE IS *NOT* SHOWN IN OVERLAY MODE, and the first live run is
    // why. Showing it puts an EMPTY floating 3D panel next to the overlay --
    // empty because the picture is going to the overlay window instead, which
    // reads as the viewer having failed while the overlay works perfectly.
    // The palette still EXISTS (it owns the status line, the DG wheel and button
    // events, and the teardown that joins the render thread); it is simply not
    // put on screen. `CloseDiligentOverlay` is how the overlay is closed.

    if (geomsrv::archviz::DiligentViewport::Get ().IsRunning ()) {
        panel.Show ();
        panel.statusText.SetText ("The Diligent viewport is already running; close it first.");
        return;
    }

    namespace vo = geomsrv::archviz::viewportoverlay;
    const vo::OverlayTarget target = vo::FindOverlayTarget ();
    std::string overlayError;
    const vo::OverlayAttach attachMode = attach == 1   ? vo::OverlayAttach::ChildLayered
                                         : attach == 2 ? vo::OverlayAttach::ChildTransparent
                                                       : vo::OverlayAttach::Popup;
    HWND overlay = vo::Create (target, attachMode, overlayError);
    if (overlay == nullptr) {
        // ⚠️ SHOWN ONLY ON FAILURE. The status line is the only place a user
        // learns why nothing appeared, and a hidden palette makes that invisible.
        panel.Show ();
        panel.statusText.SetText (FromStd ("Overlay window: " + overlayError));
        geomsrv::archviz::ArchVizLog ("ArchViz overlay did not open: " + overlayError);
        return;
    }

    geomsrv::archviz::Surface surface;
    surface.mode = geomsrv::archviz::SurfaceMode::Overlay;
    surface.nwh = overlay;
    // ⚠️ THE OVERLAY'S CLIENT RECT IS ALREADY IN PHYSICAL PIXELS. It was derived
    // from GetClientRect/ClientToScreen on Archicad's own window, and those speak
    // physical -- so the DisplayScale multiply the palette child needs would
    // OVERSIZE this one by the scaling factor on a 150% display. Two windows,
    // two coordinate spaces; the difference is the whole reason
    // PlanOverlay/OverlayWindow.hpp carries its DISPLAY SCALING note.
    const vo::OverlayStats stats = vo::Stats ();
    surface.width = stats.width;
    surface.height = stats.height;

    // ⚠️ ACAPI ON THE MAIN THREAD, WHICH IS WHERE WE ALREADY ARE (this handler
    // runs through MainThreadGate::Post).
    //
    // ⚠️ AND *AFTER* vo::Create, NOT BEFORE. Over a floor plan this measures the
    // plan's own pixel-to-model mapping against the OVERLAY'S rectangle, so the
    // overlay has to exist and know its size first.
    const geomsrv::archviz::CameraStart cameraStart = ReadArchicadOverlayCamera ();

    if (!geomsrv::archviz::DiligentViewport::Get ().Start (surface, cameraStart)) {
        vo::Destroy ();
        panel.Show ();
        panel.statusText.SetText ("The Diligent overlay did not start; see archviz.log.");
        return;
    }

    // ⚠️ WIREFRAME BY DEFAULT, AND IT IS THE POINT OF THE MODE RATHER THAN A
    // STYLE. A shaded overlay covers Archicad's 3D window completely, so "do the
    // two agree" -- the only question an overlay exists to answer -- stops being
    // answerable at exactly the moment it is asked. The HUD's combo can switch it
    // back for a look.
    geomsrv::archviz::DiligentViewport::Get ().SetRenderMode (int (geomsrv::archviz::SceneRenderMode::Wireframe));

    // ⚠️ AND IF IT WAS ALREADY ON SCREEN, TAKE IT OFF. `Show` refuses while an
    // overlay runs, but a palette opened BEFORE the overlay started is already
    // visible and nothing would have hidden it -- which is the empty floating
    // panel the user reported, arriving by the one route the Show guard cannot
    // cover.
    panel.Hide ();

    geomsrv::archviz::ExtractionWorker::Get ().StartLive ();

    // ⚠️ THE OVERLAY NEEDS THIS MORE THAN THE PANEL DOES, not less. An overlay
    // exists to answer "do the two pictures agree", and a viewer showing
    // yesterday's geometry over a live 3D window answers it WRONGLY while
    // looking perfectly healthy -- the failure mode the anchors were built to
    // expose, arriving through the content instead of the camera.
    //
    // A SLOWER FLOOR THAN THE PANEL'S, because this path's frame budget belongs
    // to Archicad: the overlay is drawn over the window the user is navigating,
    // and the watch backs off further on its own if the difference generator
    // turns out to be expensive on this project.
    geomsrv::archviz::modelwatch::Start (/*floorMs*/ 1500);

    vo::SetTracking (true, kOverlayTrackMs, &OnOverlayResized);
    // ⚠️ THROUGH THE MODE SWITCH, NOT StartCameraSync DIRECTLY. Calling the timer
    // straight was a SECOND ARM PATH, and it made the switch lie: opening the
    // overlay started the sync while `CameraSyncModeState` still reported `off`,
    // so the matrix probe read `off` as the mode to restore and turned a working
    // sync OFF when it finished (measured, 2026-08-13 12:16). A switch built for
    // reversibility must be the only way in, or its idea of the current mode is
    // fiction.
    //
    // ⚠️ `wakepredict`, NOT `legacy`. The overlay is not an experiment rig -- other
    // features open it and are shown whatever it decides to arm, so the default
    // has to be the best configuration that is safe to hand an ordinary user, not
    // the oldest one. `wakepredict` is that: it samples on the input instead of
    // waiting for a WM_TIMER Windows serves last, and extrapolates across what is
    // left, which the 2026-08-13 runs measured at 22-26 ms of lag falling to
    // 14-16 ms. It installs a thread-local hook and nothing else; the DXGI detour
    // in `hookdraw` stays opt-in because it costs Archicad frame time (PLAT-RE118)
    // and hides the overlay's own window.
    //
    // ⚠️ AND IT FALLS BACK, because `wakepredict` goes through the experiment
    // guard: one unrelated crash leaves a breadcrumb that refuses every hook mode
    // next session, and a dependent that opened the overlay would then get NO
    // camera sync at all -- a far worse regression than the lag this replaces.
    // `legacy` needs no guard and cannot be refused for that reason.
    std::string syncError;
    if (!geomsrv::archviz::SetCameraSyncMode (geomsrv::archviz::CameraSyncMode::WakePredict, kOverlayCameraSyncMs, 1.0,
                                              /*hideOnNav*/ false, syncError)) {
        geomsrv::archviz::ArchVizLog ("overlay: wakepredict did not arm -- " + syncError + "; falling back to legacy");
        if (!geomsrv::archviz::SetCameraSyncMode (geomsrv::archviz::CameraSyncMode::Legacy, kOverlayCameraSyncMs, 1.0,
                                                  /*hideOnNav*/ false, syncError)) {
            geomsrv::archviz::ArchVizLog ("overlay: camera sync did not arm -- " + syncError);
        }
    }

    panel.statusText.SetText (FromStd ("Diligent overlay over " + stats.targetClass + " (" +
                                       std::to_string (stats.width) + "x" + std::to_string (stats.height) + ")"));
}

void ArchVizPanel::CloseDiligentOverlay ()
{
    // ⚠️ THE RENDER THREAD JOINS BEFORE THE WINDOW GOES, unconditionally and in
    // that order. Presenting a composition swap chain whose target HWND has been
    // destroyed is a crash blamed on Archicad -- the same rule the palette child
    // has always had, and the reason StopRenderer joins.
    //
    // Through the mode switch for the same reason the open does: it is the one
    // thing that knows what is armed, and a teardown that bypasses it leaves the
    // switch reporting a mode nothing is running.
    geomsrv::archviz::ShutDownCameraSync ();
    // ⚠️ EXPLICITLY, EVEN THOUGH THE WATCH STOPS ITSELF when it notices the
    // viewport has gone. Noticing costs it one more tick, and that tick calls
    // ACAPI from a ::SetTimer during a teardown -- the exact shape the selection
    // bridge's own "must not outlive the add-on" rule exists to forbid. A
    // self-healing guard is the backstop, not the mechanism.
    geomsrv::archviz::modelwatch::Stop ();
    geomsrv::archviz::DiligentViewport::Get ().Stop ();
    geomsrv::archviz::viewportoverlay::Destroy ();
    if (HasInstance ())
        GetInstance ().statusText.SetText ("Diligent overlay closed.");
}

void ArchVizPanel::CloseViewer ()
{
    // Stop D3D12 before camera-sync teardown clears the shared experiment
    // breadcrumb. A crash during the D3D12 join must survive into safe mode.
    CloseD3D12FeasibilityProbe ();
    geomsrv::archviz::ShutDownCameraSync ();
    if (!HasInstance ())
        return;
    GetInstance ().StopRenderer ();
    geomsrv::archviz::DiligentProbe::Get ().Stop ();
    GetInstance ().Hide ();
}

// ---------------------------------------------------------------------------
void ArchVizPanel::Layout ()
{
    const short width = GetWidth ();
    const short height = GetHeight ();

    const short statusTop = (short) (height - kMargin - kStatusHeight);
    statusText.SetRect (DG::Rect (kMargin, statusTop, (short) (width - kMargin), (short) (statusTop + kStatusHeight)));

    if (viewport != nullptr) {
        // Flush to the panel edges except for the status strip: the viewport is
        // the point of this palette, and a border of DG background around a 3D
        // view is wasted pixels.
        viewport->SetRect (DG::Rect (0, 0, width, (short) (statusTop - 4)));
    }
}

namespace {

// Everything Windows will tell us about a candidate handle. A raw number proves
// nothing — 0x36000032 and 0x000522B6 look equally like HWNDs — but a class
// name, a rect and a parent identify a window, and a failed IsWindow() rules one
// out outright.
std::string DescribeWindow (const char* label, HWND hwnd)
{
    std::string out = std::string (label) + "=0x";
    char buf[32] = {};
    std::snprintf (buf, sizeof (buf), "%p", (void*) hwnd);
    out += buf;

    if (hwnd == nullptr)
        return out + " (null)";
    if (::IsWindow (hwnd) == FALSE)
        return out + " NOT A WINDOW";

    char cls[128] = {};
    ::GetClassNameA (hwnd, cls, (int) sizeof (cls) - 1);
    RECT r = {};
    ::GetClientRect (hwnd, &r);
    char detail[256] = {};
    std::snprintf (detail, sizeof (detail), " class='%s' client=%ldx%ld parent=%p visible=%d", cls, r.right - r.left,
                   r.bottom - r.top, (void*) ::GetParent (hwnd), ::IsWindowVisible (hwnd) ? 1 : 0);
    return out + detail;
}

} // namespace

void* ArchVizPanel::ViewportWindow () const
{
    if (viewport == nullptr)
        return nullptr;

    HWND const hwnd = DGGetDialogItemWindow (GetId (), viewport->GetId ());

    // ⚠️ VALIDATE BEFORE ANY RENDERER SEES IT, AND THE REASON OUTLIVED bgfx.
    // bgfx marked a renderer permanently unsupported the first time its createFn
    // failed, process-wide, so one init on a bad handle cost the user D3D11 until
    // they restarted Archicad. Diligent has its own version of the same hazard
    // (docs/architecture/diligent/SPEC.md: a failed init may be sticky and process-wide),
    // and PLAT-RE39 -- the viewport opening once per session -- is what it looks
    // like here. A handle checked before the attempt costs nothing.
    if (hwnd == nullptr || ::IsWindow (hwnd) == FALSE) {
        geomsrv::archviz::ArchVizLog ("viewport window — " + DescribeWindow ("DGGetDialogItemWindow", hwnd) +
                                      "  (panel " + std::to_string ((int) GetId ()) + ", item " +
                                      std::to_string ((int) viewport->GetId ()) + ")");
        return nullptr;
    }

    geomsrv::archviz::ArchVizLog ("viewport window — " + DescribeWindow ("DGGetDialogItemWindow", hwnd));
    return (void*) hwnd;
}

// ⚠️ `StartRenderer` IS GONE, AND ONLY THE NAME WAS bgfx'S. It initialised
// bgfx on the palette child; that renderer no longer exists (PLAT-RE66) and
// `OpenDiligentViewport` above is what starts a viewer on the same HWND. The
// two lessons its body carried are not lost: the HWND is still validated in
// `ViewportWindow` before any renderer sees it, and the DPI correction it
// documented is still in `DisplayScale`.
//
// `StopRenderer` KEEPS ITS NAME, because it was never only bgfx's -- it is the
// palette's whole teardown, and the ORDER in it is what the header, the
// destructor and AddOnMain's unload path all depend on.

void ArchVizPanel::StopRenderer ()
{
    // ⚠️ UNCONDITIONALLY, AND BEFORE THE EARLY RETURN. The timer is a
    // process-wide Win32 resource that outlives this palette if it is not
    // killed, and it calls ACAPI — leaving it armed after teardown is the exact
    // bug class CLAUDE.md flags for the change observer ("unwatch on teardown,
    // unconditionally"), with the same consequence.
    StopNavLog ();

    // RE51.D1 owns another render thread and may be presenting to both windows.
    // Join it before either the DG child or overlay HWND can be destroyed.
    CloseD3D12FeasibilityProbe ();

    // ⚠️ THE PRODUCER GOES BEFORE THE CONSUMER, and unconditionally, for the
    // same reason as the timer above. An extraction pass in flight is submitting
    // gate jobs and queueing geometry for a render thread that is about to be
    // joined; every one of those uploads would then be leaked in the queue, and
    // on unload the worker would be poking a main thread that no longer has an
    // add-on behind it. Stop() JOINS — bounded by the worker's short slice
    // timeout (ExtractionThread.hpp).
    // The selection bridge, before anything it talks to goes away. Same
    // unconditional rule as the nav timer above: a ::SetTimer that calls ACAPI
    // must not outlive the add-on.
    geomsrv::archviz::selectionbridge::Stop ();

    // The model watch, for the same reason and under the same rule: it is a
    // ::SetTimer that calls ACAPI, so it must not outlive the add-on. It also has
    // to go BEFORE the extraction worker, or its next tick could start a pass
    // into a worker that is being torn down.
    geomsrv::archviz::modelwatch::Stop ();

    // ⚠️ BEFORE THE DG ITEM CAN GO. A subclass left installed over a destroyed
    // window calls into freed code -- the same rule as the timers above.
    geomsrv::archviz::viewportcursor::Detach ();

    geomsrv::archviz::ExtractionWorker::Get ().Stop ();
    // Nothing will consume what is left; holding it would mean the next viewer
    // open replays a batch from the previous session, half-applied.
    geomsrv::archviz::SceneCmdQueue::Get ().Clear ();

    // The Diligent smoke-test thread also presents to this HWND. It must be
    // joined before the DG child can disappear, regardless of which renderer
    // the caller believes is active.
    geomsrv::archviz::DiligentViewport::Get ().Stop ();
    // ⚠️ AFTER THE JOIN, AND UNCONDITIONALLY. If the viewport was running as an
    // OVERLAY it was presenting into this window; destroying it first is a
    // present to a dead HWND. Unconditional because "was it an overlay" is not
    // worth tracking here -- Destroy on nothing is free.
    geomsrv::archviz::viewportoverlay::Destroy ();

    // ⚠️ AFTER THE RENDERER HAS JOINED, NEVER BEFORE. The wheel button is
    // polled globally, so a buffer left holding a press outlives the viewer and
    // the NEXT one latches that drag on its first frame.
    geomsrv::archviz::InputRingBuffer::Get ().Reset ();
    statusText.SetText ("Renderer stopped.");
    lastStatus = GS::UniString ();
}

void ArchVizPanel::RefreshStatus ()
{
    const geomsrv::archviz::DiligentProbeStats diligent = geomsrv::archviz::DiligentProbe::Get ().Stats ();
    const geomsrv::archviz::DiligentViewportStats viewportStats = geomsrv::archviz::DiligentViewport::Get ().Stats ();

    GS::UniString text;
    // ⚠️ PROBE 1c CREATES A DEVICE AND NOTHING ELSE -- no swap chain, no frame
    // loop -- so it has to be reported separately or a successful probe reads as
    // a failed viewer.
    if (viewportStats.running || viewportStats.initialized || viewportStats.failed) {
        if (viewportStats.failed)
            text = GS::UniString ("Diligent viewport failed: ") + FromStd (viewportStats.error);
        else if (!viewportStats.initialized)
            text = GS::UniString ("Starting Diligent D3D11 viewport...");
        else if (!viewportStats.clearChecked)
            text = GS::UniString ("Diligent D3D11 viewport running (clear/present smoke test).");
        else if (viewportStats.diligentClearMatched && viewportStats.nativeClearMatched)
            text = viewportStats.sceneReady
                       ? GS::UniString ("Diligent viewport: clear A/B PASS, drawing.")
                       : GS::UniString (
                             "Diligent viewport: clear A/B PASS, but the scene did not build - see archviz.log.");
        else if (viewportStats.nativeClearMatched)
            text = GS::UniString ("Diligent viewport: only the raw D3D11 clear landed - see archviz.log.");
        else if (viewportStats.diligentClearMatched)
            text = GS::UniString ("Diligent viewport: only the Diligent clear landed - see archviz.log.");
        else
            text = GS::UniString ("Diligent viewport: BOTH clears failed - the fault is below Diligent.");
    }
    else if (diligent.attempted) {
        if (diligent.running)
            text = GS::UniString ("Diligent Probe 1c: creating D3D11 device...");
        else if (diligent.succeeded)
            text = GS::UniString ("Diligent Probe 1c: PASS - device and context created.");
        else
            text = GS::UniString ("Diligent Probe 1c: did not pass - see archviz.log.");
    }
    else {
        text = GS::UniString ("Viewer not running.");
    }

    // ⚠️ NO fps AND NO FRAME COUNT IN THIS STRING, DELIBERATELY, AND IT IS A
    // FIX. They used to be here -- which changes every second, so SetText fired
    // every second, which is a DG PAINT into the palette hosting the render
    // surface. `PanelIdle` fires when Archicad goes IDLE, i.e. A MOMENT AFTER
    // NAVIGATION STOPS, which is exactly when the reported flicker appeared.
    // Nothing is lost: they are live in the ImGui HUD's always-on badge, drawn by
    // the render thread into the surface itself (PLAT-RE61).
    if (text != lastStatus) {
        statusText.SetText (text);
        lastStatus = text;
    }
}

// ---------------------------------------------------------------------------
bool ArchVizPanel::ViewportPixelSize (uint32_t& width, uint32_t& height) const
{
    // ⚠️ THE CLIENT RECT, NOT `logical size * DPI scale`, AND THE DIFFERENCE IS A
    // PICKING BUG (PLAT-RE139).
    //
    // The cursor arrives from `ScreenToClient` on this HWND, i.e. in CLIENT
    // pixels, and it is bounds-checked against `GetClientRect`. If the swap chain
    // is sized by a SECOND computation -- DG's logical item size multiplied by
    // DPI/96 and truncated to an integer -- then the two agree only when that
    // arithmetic happens to land on the same number. At 125% or 150% scaling the
    // truncation can differ by a pixel or more, and a pick then maps the cursor
    // through a viewport size that is not the one the cursor was measured in.
    //
    // The resulting error is ZERO AT THE CENTRE OF THE VIEW AND GROWS TOWARD THE
    // EDGES, because it is a scale error rather than an offset -- which is
    // exactly how "picking is imprecise" is reported, and why it survives an
    // aim that is otherwise correct.
    //
    // Two computations of one quantity is the fault; asking the window is the
    // fix. This is the SAME source `PollHardwareInput` measures against, so they
    // agree by construction rather than by arithmetic.
    HWND const hwnd = DGGetDialogItemWindow (GetId (), viewport != nullptr ? viewport->GetId () : 0);
    if (hwnd == nullptr || ::IsWindow (hwnd) == FALSE)
        return false;
    RECT client = {};
    if (::GetClientRect (hwnd, &client) == FALSE)
        return false;
    if (client.right <= 0 || client.bottom <= 0)
        return false;
    width = uint32_t (client.right);
    height = uint32_t (client.bottom);
    return true;
}

double ArchVizPanel::DisplayScale () const
{
    if (viewport == nullptr)
        return 1.0;
    // ⚠️ DG SPEAKS LOGICAL PIXELS; the backbuffer is in PHYSICAL ones. At 150%
    // scaling an uncorrected cursor lands two-thirds of the way to where the user
    // is pointing — the same class of error that made the plan overlay pan at
    // two-thirds speed (PlanOverlay/OverlayWindow.hpp).
    // ⚠️ ASKED OF THE ITEM'S REAL WINDOW. Passing GetControl()'s bogus handle
    // here returned 96 while the item was genuinely on a 144-dpi monitor, so the
    // backbuffer came out two-thirds the size of the window.
    // ⚠️ THE FALLBACK IS THE LAST GOOD VALUE, NOT 1.0, AND THAT IS A FIX.
    // Returning 1.0 when the item's window cannot be asked produces a backbuffer
    // 2/3 the size of the window on a 150% display — from a transient failure,
    // for a monitor that has not changed. Archicad hides every palette when a
    // modal comes up and re-lays them out on a window switch, so "cannot be
    // asked right now" is an ordinary, momentary state, not an error. 1.0 is a
    // MEASUREMENT here; the last good value is a memory, and a memory is right
    // far more often. A first call that fails still gets 1.0, because there is
    // nothing else to say.
    HWND const hwnd = DGGetDialogItemWindow (GetId (), viewport->GetId ());
    if (hwnd == nullptr || ::IsWindow (hwnd) == FALSE)
        return lastGoodScale;
    const UInt32 dpi = DGGetDPIForDialogItem (hwnd);
    if (dpi == 0)
        return lastGoodScale;
    lastGoodScale = double (dpi) / 96.0;
    return lastGoodScale;
}

// ⚠️ NO MOUSE POSITION IS TAKEN FROM DG ANY MORE, AND THE ABSENCE IS THE FIX.
// `UserItemMouseMoved` fires while HOVERING and stops for the whole duration of
// a drag, so a camera driven by it sees nothing between press and release and
// then one large jump when the release finally pushes a position — the reported
// "clicks RANDOMLY rotate the cube" and the ImGui panel thrown off screen, one
// bug (plan §8.4). The render thread polls GetCursorPos + ScreenToClient
// instead, which has no such gap and is already in physical pixels.
//
// The wheel BUTTON is polled too, so `IsWheelButton` is deliberately not
// consulted below: the navigation gesture must not depend on an unverified DG
// behaviour, and two sources for one quantity is the bug above in a new coat.
//
// What is left here is what DG is genuinely good at and nothing else: the left
// and right buttons, for ImGui, and the wheel.

void ArchVizPanel::UserItemMouseDown (const DG::UserItemMouseDownEvent& ev, bool* processed)
{
    (void) ev;
    // The child HWND subclass owns this input. DG is only the palette shell.
    *processed = true;
}

void ArchVizPanel::UserItemMouseUp (const DG::UserItemMouseUpEvent& ev, bool* processed)
{
    (void) ev;
    *processed = true;
}

void ArchVizPanel::PanelWheelTracked (const DG::PanelWheelTrackEvent& ev, bool* processed)
{
    (void) ev;
    // WM_MOUSEWHEEL is consumed by the native child HWND subclass.
    *processed = false;
}

void ArchVizPanel::PanelResized (const DG::PanelResizeEvent& /*ev*/)
{
    Layout ();
    if (viewport == nullptr)
        return;

    // ⚠️ QUEUED, NOT APPLIED HERE. A swap chain is resized at a frame boundary on
    // the render thread — never from a DG callback (plan section 4, risk 3).
    const double scale = DisplayScale ();
    uint32_t pw = 0;
    uint32_t ph = 0;
    if (!ViewportPixelSize (pw, ph)) {
        pw = uint32_t (viewport->GetWidth () * scale);
        ph = uint32_t (viewport->GetHeight () * scale);
    }

    // ⚠️ LOGGED, BECAUSE A TRANSIENT CANNOT BE WATCHED FOR. The reported "in a 2D
    // view the geometry momentarily scales down" happened while the viewer's
    // camera was provably STILL — `archviz_nav.log` shows its eye constant to
    // four decimals and no navigation for the whole session — so whatever
    // changed was the viewport, not the camera. A resize is rare, so logging
    // every one costs nothing and turns a glimpse into a record: two entries a
    // few ms apart, with a size that goes away and comes back, IS the bug.
    geomsrv::archviz::ArchVizLog ("resize: item " + std::to_string (viewport->GetWidth ()) + "x" +
                                  std::to_string (viewport->GetHeight ()) + " logical, scale " +
                                  std::to_string (scale) + " -> " + std::to_string (pw) + "x" + std::to_string (ph) +
                                  " physical" + (IsVisible () ? "" : "   [PALETTE HIDDEN]"));

    // ⚠️ NOT WHEN IT IS AN OVERLAY (PLAT-RE57). When the renderer is running as an
    // OVERLAY its surface is sized to ARCHICAD'S
    // window, and this handler carries the PALETTE CHILD's size -- pushing it
    // resizes the composition swap chain while the overlay window keeps its own
    // dimensions, so the aspect ratio and therefore the projection stop matching.
    // It is unrecoverable from the UI, because the overlay's tracker only pushes
    // a resize when the TARGET window changes size and it had not. Live report,
    // run 2: "scaling the floating viewport panel made overlay not match
    // perspective and position, and no way to reset."
    //
    // The overlay's size belongs to ViewportOverlayWindow's tracker and to
    // nothing else.
    if (!OverlayRunning ())
        geomsrv::archviz::DiligentViewport::Get ().RequestResize (pw, ph);
}

void ArchVizPanel::PanelIdle (const DG::PanelIdleEvent& /*ev*/)
{
    RefreshStatus ();
}

void ArchVizPanel::PanelCloseRequested (const DG::PanelCloseRequestEvent&, bool* accepted)
{
    // The window is about to go; the renderer must be gone FIRST and this joins.
    StopRenderer ();
    Hide ();
    *accepted = true;
}

void ArchVizPanel::UserItemUpdate (const DG::UserItemUpdateEvent& /*ev*/)
{
    // NOTHING. Deliberately. See the header: DG painting anything here — a
    // background fill included — fights the renderer's present.
}

// ---------------------------------------------------------------------------
// Modeless-window registration. Same shape as ControlPalette's (which lives in
// its own PaletteRegistration.cpp for size reasons this panel does not have).
GSErrCode ArchVizPanel::PaletteControlCallBack (Int32, API_PaletteMessageID messageID, GS::IntPtr param)
{
    switch (messageID) {
        case APIPalMsg_OpenPalette:
            OpenViewer ();
            break;
        case APIPalMsg_ClosePalette:
            CloseViewer ();
            break;
        case APIPalMsg_HidePalette_Begin:
            // Archicad is hiding every palette (a modal is coming up). The
            // renderer keeps running: this is temporary, the HWND stays alive, and
            // tearing the device down and back up for a modal would cost seconds.
            if (HasInstance () && GetInstance ().IsVisible ())
                GetInstance ().Hide ();
            break;
        case APIPalMsg_HidePalette_End:
            if (HasInstance () && !GetInstance ().IsVisible ())
                GetInstance ().Show ();
            break;
        case APIPalMsg_IsPaletteVisible:
            *(reinterpret_cast<bool*> (param)) = HasInstance () && GetInstance ().IsVisible ();
            break;
        case APIPalMsg_GetPaletteDeactivationMethod:
            *(reinterpret_cast<API_PaletteDeactivationMethod*> (param)) = APIPaletteDeactivationMethod_Default;
            break;
        default:
            break;
    }
    return NoError;
}

GSErrCode ArchVizPanel::RegisterPaletteControlCallBack ()
{
    return ACAPI_RegisterModelessWindow (GS::CalculateHashValue (archVizPaletteGuid), PaletteControlCallBack,
                                         API_PalEnabled_FloorPlan + API_PalEnabled_Section + API_PalEnabled_Elevation +
                                             API_PalEnabled_InteriorElevation + API_PalEnabled_3D +
                                             API_PalEnabled_Detail + API_PalEnabled_Worksheet + API_PalEnabled_Layout +
                                             API_PalEnabled_DocumentFrom3D,
                                         GSGuid2APIGuid (archVizPaletteGuid));
}

GSErrCode ArchVizPanel::UnregisterPaletteControlCallBack ()
{
    return ACAPI_UnregisterModelessWindow (GS::CalculateHashValue (archVizPaletteGuid));
}
