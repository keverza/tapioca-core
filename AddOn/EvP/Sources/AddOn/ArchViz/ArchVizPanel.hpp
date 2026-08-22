#ifndef EVP_ARCHVIZ_ARCHVIZPANEL_HPP
#define EVP_ARCHVIZ_ARCHVIZPANEL_HPP

// The 3D viewer's palette: a DG shell whose only job is to own a window the
// renderer can draw into, and to say what the renderer is doing.
//
// It is a DG::Palette so the viewer docks and floats like every other Archicad
// palette (plan section 4). It is NOT ControlPalette's sibling in any other
// sense — it hosts no commands, no parameters and no results.
//
// The viewport is a DG::UserItem created at RUNTIME with FrameType::NoFrame.
// Its HWND is what the swap chain is created against -- see ViewportWindow for
// why GetControl() is NOT where that handle comes from.
//
// ⚠️ UserItemUpdate IS OVERRIDDEN TO DO NOTHING. That override is not a stub, it
// is the feature: UserItemUpdate is DG asking us to paint, and anything painted
// there — a background fill included — fights the renderer's present and the
// viewport flickers or goes blank (plan section 4, risk 1). A flickering
// rectangle rather than a solid picture is the first thing to check here.
//
// ⚠️ EVERYTHING HERE IS MAIN-THREAD ONLY, like every DG object. The render
// thread is reached through ArchViz/DiligentViewport's atomics and never the
// other way round: DiligentViewport has no idea this class exists.

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "DGModule.hpp"

// For CameraStart, returned by value from ReadArchicadCamera below. Light:
// DiligentViewport.hpp pulls no Diligent headers, only <thread>/<mutex>.
#include "ArchViz/DiligentViewport.hpp"

#include <memory>
#include <string>

class ArchVizPanel final : public DG::Palette, public DG::PanelObserver, public DG::UserItemObserver {
  public:
    static bool HasInstance ();
    static void CreateInstance ();
    static ArchVizPanel& GetInstance ();
    static void DestroyInstance ();

    void Show ();
    void Hide ();

    // Bring the viewer up: create the palette if needed, show it, and start the
    // Diligent viewport on the palette child's HWND. Idempotent.
    //
    // ⚠️ IT IS THE MENU ITEM AND THE PALETTE CALLBACK, which is why it survived
    // bgfx (PLAT-RE66) rather than being folded into OpenDiligentViewport: it is
    // the only way to open a viewer without running a probe.
    //
    // ⚠️ Q1 IS ANSWERED (2026-08-06) AND THE ANSWER IS DGGetDialogItemWindow.
    // A DG::UserItem IS a real child window — class DGUserItemClass, correctly
    // sized, visible — but only that call finds it. `DG::UserItem::GetControl ()`
    // is documented as "hwnd on windows" and returns 0xFFFFFFFF99014CE1, which
    // ::IsWindow rejects; handing it to a renderer gets CreateSwapChain HRESULT
    // 0x80070005 (E_ACCESSDENIED). There is no `hwndSource` knob any more,
    // because there is no longer a question — and because ASKING it is
    // destructive, see ViewportWindow.
    static void OpenViewer ();
    static void OpenDiligentProbe ();
    static bool OpenD3D12FeasibilityProbe (std::string& error);
    static void CloseD3D12FeasibilityProbe ();
    static void OpenDiligentViewport ();

    // The OVERLAY (PLAT-RE37): the same Diligent viewport, drawing into a
    // transparent click-through window pinned over Archicad's own 3D view
    // instead of into the palette's child.
    //
    // ⚠️ IT STILL NEEDS THE PALETTE, and not for the picture. The palette is
    // where the status line lives and where DG delivers the wheel and the mouse
    // buttons; the overlay window is deliberately WS_EX_TRANSPARENT, so it
    // receives no input at all and could not host them. What the overlay gives
    // up in exchange is exactly that: it CANNOT be navigated with the mouse, and
    // it is not supposed to be -- Archicad's own 3D window is driven, and the
    // camera sync makes the overlay follow.
    //
    // ⚠️ AND IT MUST BE OPENED WITH THE 3D WINDOW IN FRONT. The target is the
    // frontmost document canvas; there is no DevKit call that hands back the 3D
    // view's HWND, so an overlay opened over the floor plan lands on the floor
    // plan. `DiligentViewportState.overlayTargetClass` says which it got.
    // `attach` selects how the overlay window is parented -- see
    // ViewportOverlayWindow.hpp's OverlayAttach for why that is a runtime choice
    // and not a constant. 0 = popup (visible, covers callouts), 1 = child
    // layered, 2 = child transparent-only.
    static void OpenDiligentOverlay (int attach = 0);
    static void CloseDiligentOverlay ();
    // Stop the render thread (JOINING it) and hide the palette. Idempotent, and
    // safe to call when nothing is open.
    static void CloseViewer ();

    // ---- the overlay path --------------------------------------------------
    // Archicad's 3D window's own camera, as a CameraStart.
    //
    // ⚠️ MAIN THREAD ONLY: it calls ACAPI_View_Get3DProjectionSets. It is public
    // so a native command can read it through MainThreadGate rather than
    // duplicating the projection-to-camera conversion, which is where the
    // horizontal-vs-vertical field of view would drift between two copies.
    //
    // Never fails: a projection that has no camera (axonometric) or a failed
    // read both come back with `valid == false` and the reason in `source`.
    // ⚠️ FULLY QUALIFIED: ArchVizPanel is at GLOBAL scope, not inside geomsrv.
    static geomsrv::archviz::CameraStart ReadArchicadCamera ();

    // What the OVERLAY should be looking through: the 3D window's camera, or --
    // when the frontmost window is the floor plan -- a top-down orthographic
    // camera matching that plan's zoom and scroll.
    //
    // ⚠️ THE DISPATCH IS THE POINT, AND IT FIXES A SILENT WRONG ANSWER.
    // `ACAPI_View_Get3DProjectionSets` returns the 3D WINDOW's settings whatever
    // is in front (the same trap NavTimerProc documents), so an overlay over the
    // floor plan used to draw a perspective view of the model at the 3D window's
    // camera, over a plan drawing, with nothing anywhere saying why.
    //
    // ⚠️ MAIN THREAD ONLY, and it needs the OVERLAY'S PIXEL SIZE -- the plan's
    // camera is measured against the rectangle the picture goes into. See
    // ArchViz/PlanCameraMath.hpp.
    static geomsrv::archviz::CameraStart ReadArchicadOverlayCamera ();

    // Make the Diligent viewport FOLLOW Archicad's 3D window, continuously.
    //
    // ⚠️ IT IS A WIN32 TIMER AND IT HAS TO BE. Driving this over the bus from
    // Python -- one command to read, one to push -- looks equivalent and is not:
    // both are MainThreadCommands, and during a drag in the 3D window Archicad's
    // main thread is inside its own modal loop and dispatches neither, so the
    // viewport only caught up ON MOUSE RELEASE. A WM_TIMER keeps being
    // dispatched inside that loop. Same mechanism and the same caveats as
    // StartNavLog below; read those.
    //
    // Idempotent, and stopped automatically by CloseViewer and by the timer
    // itself once the viewport is no longer running.
    static bool StartCameraSync (uint32_t intervalMs);
    static void StopCameraSync ();

    // One poll: read Archicad's camera, publish it to the viewport, log the row.
    // MAIN THREAD ONLY -- it calls ACAPI.
    //
    // Public because `wake` mode drives it from a posted message rather than
    // from the timer, and BOTH paths must run exactly this, so that comparing
    // the modes measures the wake source and not two copies of the tick that
    // drifted apart.
    static void PollCameraOnce ();

    // The selection bridge lives in ArchViz/SelectionBridge — same Win32-timer
    // reasoning as StartCameraSync above, but it is not the palette's business
    // beyond arming it with the renderer and killing it in StopRenderer.

    // ---- the navigation comparison log (ArchViz/NavLog.hpp) ----------------
    // Archicad's own camera can only be read through ACAPI, i.e. on the MAIN
    // thread; our camera lives on the render thread. This is the main-thread
    // half, and it is a Win32 timer rather than DG's idle event for one
    // measured reason: DG idle fires when ARCHICAD IS IDLE, which is precisely
    // not when the user is dragging in the 3D window — the only moment the
    // comparison is interesting.
    //
    // ⚠️ ACAPI FROM A TIMER CALLBACK IS LEGAL ONLY BECAUSE IT IS DISPATCHED ON
    // THE UI THREAD. `SetTimer (nullptr, ...)` posts WM_TIMER to the thread that
    // called it and DispatchMessage invokes the callback there — so this must be
    // started from the main thread and nowhere else. Same reasoning, and the
    // same warning, as PlanOverlay/OverlayWindow.cpp's tracking timer.
    //
    // ⚠️ AND WM_TIMER IS LOW PRIORITY: Windows generates it only when the queue
    // is otherwise empty, so a fast drag inside Archicad can starve it. That is
    // not worked around, it is MEASURED — every row carries its gap, so a
    // starved poll is visible instead of being mistaken for a still camera.
    // `sampler=false` opens the log with NO poll of its own -- the camera sync
    // tick writes the rows instead, so the file describes the poll that
    // actually moves the overlay. See the definition for why that is the
    // correct measurement and not merely a cheaper one.
    static bool StartNavLog (uint32_t intervalMs, bool sampler = true);
    static void StopNavLog ();

    static GSErrCode RegisterPaletteControlCallBack ();
    static GSErrCode UnregisterPaletteControlCallBack ();

    ~ArchVizPanel () override;

  private:
    ArchVizPanel ();

    void Layout ();
    // ⚠️ Called before the viewport window can go away, and it JOINS. Presenting
    // to a destroyed HWND is a crash blamed on Archicad.
    //
    // ⚠️ ITS NAME PREDATES THE RENDERER IT NOW STOPS, and it is the palette's
    // WHOLE teardown rather than one renderer's: the extraction worker, the
    // selection bridge, the nav timer, the scene queue and the overlay window all
    // go down here, in an order the destructor and AddOnMain's unload both rely
    // on.
    void StopRenderer ();

    // The status line, refreshed from the render thread's stats on idle. This is
    // where an init failure is visible without opening a log.
    void RefreshStatus ();

    // The viewport's native window handle, or nullptr — from
    // DGGetDialogItemWindow, validated with ::IsWindow before any renderer sees
    // it.
    //
    // ⚠️ THE VALIDATION IS NOT BELT-AND-BRACES, IT IS THE POINT, AND THE REASON
    // OUTLIVED bgfx. bgfx marked a renderer permanently unsupported the first
    // time its createFn failed (`s_rendererCreator[r].supported = false`,
    // process-wide, no way back), so ONE init on a bad handle cost the user
    // D3D11 until they restarted Archicad. The same class of stickiness is live
    // on the Diligent path as PLAT-RE39 — the viewport opens once per Archicad
    // session — so a handle Windows does not recognise must still never reach a
    // renderer.
    void* ViewportWindow () const;

    // DG input, pushed onto ArchViz/InputRingBuffer and consumed by the render
    // thread (DiligentViewport::Run). ⚠️ EVERY ONE OF THESE CROSSES A THREAD BOUNDARY; that is the price
    // of DG hosting (plan §4 risk 2), so they do the least possible work: push
    // and return.
    //
    // ⚠️ THERE IS NO MOUSE-MOVE HANDLER, AND THAT IS THE FIX, NOT AN OMISSION.
    // DG stops delivering UserItemMouseMoved for the duration of a drag, which
    // gave the camera one huge delta on release instead of a stream — the
    // "clicks randomly rotate the cube" report (plan §8.4). The cursor, the
    // Shift key and the WHEEL BUTTON are polled by the render thread instead
    // (ArchViz/HardwareInput -> PollHardwareInput). Do not add a move handler back:
    // two sources for one quantity is the same bug wearing a different coat.
    void UserItemMouseDown (const DG::UserItemMouseDownEvent& ev, bool* processed) override;
    void UserItemMouseUp (const DG::UserItemMouseUpEvent& ev, bool* processed) override;
    // The wheel does NOT arrive on UserItemObserver — it comes from the panel,
    // and has to be filtered to the viewport by hand.
    void PanelWheelTracked (const DG::PanelWheelTrackEvent& ev, bool* processed) override;

    // The item's current logical->physical factor, asked of DG each time so it
    // follows the palette between monitors. ⚠️ STILL NEEDED FOR THE BACKBUFFER
    // SIZE, and only for that: DG rects are logical and the backbuffer is
    // physical. The cursor no longer passes through here at all, because
    // ScreenToClient already answers in physical pixels.
    double DisplayScale () const;

    // The viewport's size in PHYSICAL pixels, read from its own client rect --
    // the same space PollHardwareInput measures the cursor in. False when the
    // window cannot be asked. See the .cpp: deriving this a second way (logical
    // size x DPI) is what made picking drift towards the edges (PLAT-RE139).
    bool ViewportPixelSize (uint32_t& width, uint32_t& height) const;

    void PanelResized (const DG::PanelResizeEvent& ev) override;
    void PanelIdle (const DG::PanelIdleEvent& ev) override;
    void PanelCloseRequested (const DG::PanelCloseRequestEvent& ev, bool* accepted) override;
    // DG asking us to paint. Does nothing, ON PURPOSE — see the header comment.
    void UserItemUpdate (const DG::UserItemUpdateEvent& ev) override;

    static GSErrCode PaletteControlCallBack (Int32 referenceID, API_PaletteMessageID messageID, GS::IntPtr param);

    static GS::Ref<ArchVizPanel> instance;

    DG::LeftText statusText;
    std::unique_ptr<DG::UserItem> viewport;
    // So the idle poll does not rewrite an unchanged status line every tick.
    GS::UniString lastStatus;
    // ⚠️ THE LAST DPI SCALE DG ACTUALLY ANSWERED WITH. `DisplayScale()` is asked
    // during window switches and palette hide/show, when the item's window may
    // momentarily not be askable — and answering 1.0 there sizes the backbuffer
    // at 2/3 on a 150% display for a monitor that has not moved. Mutable because
    // DisplayScale is const and this is a cache, not state anyone can observe.
    mutable double lastGoodScale = 1.0;
};

#endif
