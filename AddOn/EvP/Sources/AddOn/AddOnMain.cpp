// ============================================================================
// GeometryServer — Archicad add-on entry points (M1).
// A menu command opens a floating palette (Start/Stop buttons + status). The
// HTTP server is owned by the palette. Model status is tracked on the main
// thread via project notifications and published to ServerState for the
// ACAPI-free /health route.  See archicad-geometry-addon-plan.md (M2+).
// ============================================================================

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ResourceIds.hpp"
#include "Palette/AboutDialog.hpp"
#include "AddOnVersion.hpp"
#include "Palette/ControlPalette.hpp"
#include "ArchViz/ArchVizPanel.hpp"     // the Diligent 3D viewer palette
#include "ArchViz/ExtractionThread.hpp" // its geometry producer, joined on teardown
#include "ArchViz/CameraSyncMode.hpp"   // camera-sync mechanism switch — torn down on exit
#include "ArchViz/CameraWake.hpp"       // the input hook — must never outlive the DLL
#include "ArchViz/Dxgi/PresentHook.hpp" // the DXGI detour — same rule, worse failure
#include "ArchViz/ExperimentGuard.hpp"  // crash-loop guard — consulted before anything arms
#include "Notebook/NotebookPalette.hpp"
#include "Palette/WebUIPalette.hpp"
#include "AddOnCommands.hpp"
#include "Server/ServerState.hpp"
#include "Server/HttpServer.hpp"
#include "NativeCommands/PlanOverlayCommands.hpp" // ShutdownPlanOverlay — Win32 windows we own
#include "ArchViz/ViewportOverlayWindow.hpp"      // the 3D overlay, same hazard
#include "Notify/ChangeTracker.hpp"               // E25 — the model change token
#include "Notify/BackgroundArm.hpp"               // E25 — its background arming thread
#include "Python/MainThreadGate.hpp"
#include "Python/ApiCommandCatalog.hpp"
#include "Python/PathUtils.hpp"     // EvpDataDir / AppendTextLine — ACAPI-free, safe this early
#include "Diagnostics/ApiError.hpp" // DescribeErr — never print a bare GSErrCode

// ---- Startup trail ---------------------------------------------------------
// RegisterInterface and Initialize are the two places where a failure is COMPLETELY
// invisible: there is no palette yet to show a status line, no command running to
// carry an error, and Archicad reports a non-zero return by quietly not loading the
// add-on. That is how a menu item can be "registered" and simply not appear with
// nothing anywhere saying why — which cost a round trip to diagnose.
//
// So every registration result, success included, is written to logs\startup.log.
// Success matters as much as failure here: it is the only thing that distinguishes
// "we never registered it" from "we registered it and Archicad did not show it",
// and those have completely different fixes.
//
// Uses PathUtils (Win32 only, no ACAPI) so it is safe before Archicad is ready, and
// AppendTextLine, which rotates at the shared 5 MiB cap — no new uncapped log.
// AppendTextLine does not create parents, and on a fresh install logs\ does not
// exist — so the very first startup would silently log nothing at all.
static bool StartupLogPath (GS::UniString& path)
{
    const GS::UniString dataDir = evp::EvpDataDir ();
    if (dataDir.IsEmpty ())
        return false; // no %LOCALAPPDATA%; nothing to do this early, not worth failing over
    const GS::UniString logs (dataDir + GS::UniString ("\\logs"));
    if (!evp::CreateDirectoryChain (logs))
        return false;
    path = logs + GS::UniString ("\\startup.log");
    return true;
}

static void RecordStartup (const char* acapiFunction, GSErrCode err, short resId, const char* what)
{
    GS::UniString path;
    if (!StartupLogPath (path))
        return;

    evp::AppendTextLine (path, GS::UniString::Printf ("  %s (resId %d, %s): %s", acapiFunction, (int) resId, what,
                                                      (err == NoError) ? GS::UniString ("ok").ToPrintf ()
                                                                       : evp::DescribeErr (err).ToPrintf ()));
}

// One header per launch, so a log with several sessions in it can be read at all.
static void RecordStartupHeader (const char* phase)
{
    GS::UniString path;
    if (!StartupLogPath (path))
        return;
    evp::AppendTextLine (path,
                         GS::UniString::Printf ("===== " EVP_PRODUCT_NAME " " ADDON_VERSION " - %s =====", phase));
}

static void RecordStartupEvent (const char* event)
{
    GS::UniString path;
    if (StartupLogPath (path))
        evp::AppendTextLine (path, GS::UniString (event));
}

// ---- Main-thread model-status tracking -------------------------------------
static void RefreshModelOpen ()
{
    API_ProjectInfo pi {};
    const bool ok = (ACAPI_ProjectOperation_Project (&pi) == NoError);
    geomsrv::ServerState::Get ().modelOpen.store (ok);
}

static GSErrCode ProjectEventHandler (API_NotifyEventID notifID, Int32 /*param*/)
{
    switch (notifID) {
        case APINotify_New:
        case APINotify_NewAndReset:
        case APINotify_Open:
            geomsrv::ServerState::Get ().modelOpen.store (true);
            // E25: a different document is the largest change there is, and every
            // element observer just went with the old one. Bump the token (so a
            // watcher refreshes) and drop the watch (so it knows to re-arm).
            geomsrv::ChangeTracker::Get ().OnProjectEvent ();
            // Any arming pass in flight is attaching to a database that is being
            // replaced; let it finish and it would attach to nothing, slowly.
            // RequestStop, NOT Stop: this is the main thread, and joining here
            // would wait on a worker that is itself waiting for this thread.
            geomsrv::ArmWorker::Get ().RequestStop ();
            // Same for the viewer's extraction pass, and it matters more: it
            // HOLDS A ModelerAPI::Model of the document being replaced, and
            // every slice steps it one element further. RequestStop, not Stop —
            // this is the main thread and the worker is waiting on it.
            geomsrv::archviz::ExtractionWorker::Get ().RequestStop ();
            break;
        case APINotify_Close:
            geomsrv::ServerState::Get ().modelOpen.store (false);
            geomsrv::ChangeTracker::Get ().OnProjectEvent ();
            geomsrv::ArmWorker::Get ().RequestStop ();
            geomsrv::archviz::ExtractionWorker::Get ().RequestStop ();
            break;
        case APINotify_Quit:
            geomsrv::ServerState::Get ().modelOpen.store (false);
            geomsrv::ArmWorker::Get ().Stop ();
            geomsrv::archviz::ExtractionWorker::Get ().Stop ();
            // Tear the palette (and its server) down before Archicad exits.
            if (ControlPalette::HasInstance ())
                ControlPalette::DestroyInstance ();
            // The viewer holds a RENDER THREAD and a live D3D11 device. Its
            // destructor joins the thread and shuts the renderer down; letting Archicad
            // exit around a running one is a crash on close.
            if (ArchVizPanel::HasInstance ())
                ArchVizPanel::DestroyInstance ();
            if (NotebookPalette::HasInstance ())
                NotebookPalette::DestroyInstance ();
            if (WebUIPalette::HasInstance ())
                WebUIPalette::DestroyInstance ();
            break;
        default:
            break;
    }
    return NoError;
}

// ---- Menu: show the control palette, or the About box ----------------------
// Routing is on menuResID, NOT itemIndex: each menu item is its own one-item
// resource, so every itemIndex is 1. See ResourceIds.hpp for why.
static GSErrCode MenuCommandHandler (const API_MenuParams* menuParams)
{
    switch (menuParams->menuItemRef.menuResID) {
        case GeometryServerMenuResId:
            if (menuParams->menuItemRef.itemIndex == GeometryServerMenuItemIndex) {
                if (!ControlPalette::HasInstance ())
                    ControlPalette::CreateInstance ();
                if (ControlPalette::HasInstance ())
                    ControlPalette::GetInstance ().Show (true);
            }
            break;
        case ArchVizMenuResId:
            if (menuParams->menuItemRef.itemIndex == ArchVizMenuItemIndex)
                ArchVizPanel::OpenViewer ();
            break;
        case NotebookMenuResId:
            if (menuParams->menuItemRef.itemIndex == NotebookMenuItemIndex) {
                RecordStartupEvent ("Notebook: menu command received");
                NotebookPalette::Open ();
                RecordStartupEvent ("Notebook: menu command completed");
            }
            break;
        case WebUIMenuResId:
            if (menuParams->menuItemRef.itemIndex == WebUIMenuItemIndex) {
                RecordStartupEvent ("WebUI: menu command received");
                WebUIPalette::Open ();
                RecordStartupEvent ("WebUI: menu command completed");
            }
            break;
        case AboutMenuResId:
            if (menuParams->menuItemRef.itemIndex == AboutMenuItemIndex) {
                AboutDialog aboutDialog;
                aboutDialog.Invoke ();
            }
            break;
        default:
            break;
    }
    return NoError;
}

// ---- Required add-on exports (AC29: plain signatures) ----------------------
API_AddonType CheckEnvironment (API_EnvirParams* envir)
{
    RSGetIndString (&envir->addOnInfo.name, InfoStringsResId, 1, ACAPI_GetOwnResModule ());
    RSGetIndString (&envir->addOnInfo.description, InfoStringsResId, 2, ACAPI_GetOwnResModule ());
    return APIAddon_Preload; // modeless palette add-ons must be preloaded
}

GSErrCode RegisterInterface (void)
{
    RecordStartupHeader ("RegisterInterface");

    // One call per menu resource, in the order the items should appear. They share the
    // title "Tapioca", so Archicad merges them into a single main menu.
    //
    // ⚠️ A menu registration failure must NOT abort this function. Everything below is
    // load-bearing — MainThreadGate::RegisterServices is what lets a worker reach the
    // main thread at all — and trading command execution for a missing menu item is a
    // bad deal. So each menu result is recorded and registration continues.
    RecordStartup ("ACAPI_MenuItem_RegisterMenu",
                   ACAPI_MenuItem_RegisterMenu (GeometryServerMenuResId, 0, MenuCode_UserDef, MenuFlag_Default),
                   GeometryServerMenuResId, "palette item");
    RecordStartup ("ACAPI_MenuItem_RegisterMenu",
                   ACAPI_MenuItem_RegisterMenu (ArchVizMenuResId, 0, MenuCode_UserDef, MenuFlag_Default),
                   ArchVizMenuResId, "3D viewer item");
    RecordStartup ("ACAPI_MenuItem_RegisterMenu",
                   ACAPI_MenuItem_RegisterMenu (NotebookMenuResId, 0, MenuCode_UserDef, MenuFlag_Default),
                   NotebookMenuResId, "notebook item");
    RecordStartup ("ACAPI_MenuItem_RegisterMenu",
                   ACAPI_MenuItem_RegisterMenu (WebUIMenuResId, 0, MenuCode_UserDef, MenuFlag_Default), WebUIMenuResId,
                   "WebUI item");
    RecordStartup ("ACAPI_MenuItem_RegisterMenu",
                   ACAPI_MenuItem_RegisterMenu (AboutMenuResId, 0, MenuCode_UserDef, MenuFlag_SeparatorBefore),
                   AboutMenuResId, "About item");

    // Our own ModulCommands, so a worker can marshal onto the main thread via
    // CallFromEventLoop. Must be advertised here, not in Initialize.
    GSErrCode err = evp::MainThreadGate::RegisterServices ();
    if (err != NoError)
        return err;

    return NoError;
}

GSErrCode Initialize (void)
{
    RecordStartupHeader ("Initialize");

    // One install per registered menu resource — the same handler, which routes on
    // menuResID. A resource registered but not installed here is a dead menu item.
    // Recorded, not aborted on, for the same reason as the registrations above.
    RecordStartup ("ACAPI_MenuItem_InstallMenuHandler",
                   ACAPI_MenuItem_InstallMenuHandler (GeometryServerMenuResId, MenuCommandHandler),
                   GeometryServerMenuResId, "palette item");
    RecordStartup ("ACAPI_MenuItem_InstallMenuHandler",
                   ACAPI_MenuItem_InstallMenuHandler (ArchVizMenuResId, MenuCommandHandler), ArchVizMenuResId,
                   "3D viewer item");
    RecordStartup ("ACAPI_MenuItem_InstallMenuHandler",
                   ACAPI_MenuItem_InstallMenuHandler (NotebookMenuResId, MenuCommandHandler), NotebookMenuResId,
                   "notebook item");
    RecordStartup ("ACAPI_MenuItem_InstallMenuHandler",
                   ACAPI_MenuItem_InstallMenuHandler (WebUIMenuResId, MenuCommandHandler), WebUIMenuResId,
                   "WebUI item");
    RecordStartup ("ACAPI_MenuItem_InstallMenuHandler",
                   ACAPI_MenuItem_InstallMenuHandler (AboutMenuResId, MenuCommandHandler), AboutMenuResId,
                   "About item");

    // ⚠️ BEFORE ANYTHING CAN ARM. The experiment guard reads the crash-loop
    // breadcrumb left by the previous session and the user's SAFE_MODE file, and
    // latches its verdict for the whole run (PLAT-RE81). Placed here, ahead of
    // the palettes, so no code path can request an experimental camera-sync mode
    // before the guard has had its say -- and so its line in archviz.log is the
    // first thing in the file when a user is diagnosing a bad launch.
    geomsrv::archviz::experimentguard::CheckAtStartup ();

    GSErrCode err = NoError;

    GS::UniString catalogError;
    if (!evp::ValidateTapiocaCommandCatalog (catalogError)) {
        GS::UniString path;
        if (StartupLogPath (path))
            evp::AppendTextLine (path, "Tapioca catalog invalid: " + catalogError);
        return APIERR_GENERAL;
    }

    // Register the modeless palette (must be done from Initialize, not RegisterInterface).
    err = ControlPalette::RegisterPaletteControlCallBack ();
    if (err != NoError)
        return err;

    // The 3D viewer's own modeless window, registered on its own GUID so
    // Archicad remembers its placement and dock state separately.
    err = ArchVizPanel::RegisterPaletteControlCallBack ();
    if (err != NoError)
        return err;

    err = NotebookPalette::RegisterPaletteControlCallBack ();
    RecordStartup ("ACAPI_RegisterModelessWindow", err, NotebookPaletteResId, "notebook palette");
    if (err != NoError)
        return err;

    err = WebUIPalette::RegisterPaletteControlCallBack ();
    RecordStartup ("ACAPI_RegisterModelessWindow", err, WebUIPaletteResId, "WebUI palette");
    if (err != NoError)
        return err;

    // Archicad JSON API commands — the control plane. This is the channel Archicad
    // dispatches on the main thread even while a Python palette script is running
    // (ACAPI_AddOnAddOnCommunication_CallFromEventLoop is NOT: verified, it is
    // posted but never dispatched from the palette, so that bridge was removed).
    err = geomsrv::InstallAddOnCommands ();
    if (err != NoError)
        return err;

    // RecordMainThread must run HERE: Initialize is main-thread, and the gate
    // decides inline-vs-marshal by comparing against it.
    evp::MainThreadGate::Get ().RecordMainThread ();
    err = evp::MainThreadGate::InstallHandlers ();
    if (err != NoError)
        return err;

    // Track model open/close on the main thread for /health.
    ACAPI_ProjectOperation_CatchProjectEvent (
        APINotify_New | APINotify_NewAndReset | APINotify_Open | APINotify_Close | APINotify_Quit, ProjectEventHandler);
    RefreshModelOpen (); // seed current state

    return NoError;
}

GSErrCode FreeData (void)
{
    // RE51.D1's worker is guarded by the same crash breadcrumb as experimental
    // camera sync. Join it before ShutDownCameraSync clears that breadcrumb, so
    // a crash during D3D12 teardown still blocks an automatic retry next session.
    ArchVizPanel::CloseD3D12FeasibilityProbe ();
    // ⚠️ FIRST, AND AHEAD OF EVERY WINDOW AND THREAD BELOW. The camera-sync
    // mechanisms are timers, Windows hooks and (later on the ladder) a detour on
    // Archicad's own present path, all of whose callbacks live in this DLL --
    // the same hazard as the raw Win32 windows further down, arriving sooner
    // because a timer can fire during the teardown itself. It also clears the
    // crash-loop breadcrumb, which is what makes THIS exit count as clean and
    // leaves the next session's experiments available (PLAT-RE81).
    geomsrv::archviz::ShutDownCameraSync ();
    // Belt and braces: ShutDownCameraSync removes the hook via the mode teardown,
    // but only if the mode switch still believes a hook mode is armed. A hook that
    // outlives this DLL is Windows calling into freed code, so it is taken off
    // unconditionally as well.
    geomsrv::archviz::camerawake::Remove ();
    // Same reasoning, one step worse: a DXGI vtable entry still pointing into
    // this module after it unloads is a crash on Archicad's NEXT frame, not on
    // ours, and it would look like a graphics driver fault. RemovePresentHook
    // also waits for any detour call still in flight on a render thread.
    geomsrv::archviz::dxgi::RemovePresentHook ();
    if (ControlPalette::HasInstance ())
        ControlPalette::DestroyInstance ();
    ControlPalette::UnregisterPaletteControlCallBack ();
    // Same hazard as the raw Win32 windows below, one layer worse: the viewer
    // owns a thread that is INSIDE this DLL. DestroyInstance joins it and shuts
    // the renderer down; unloading around a running render thread is not survivable.
    if (ArchVizPanel::HasInstance ())
        ArchVizPanel::DestroyInstance ();
    ArchVizPanel::UnregisterPaletteControlCallBack ();
    if (NotebookPalette::HasInstance ())
        NotebookPalette::DestroyInstance ();
    NotebookPalette::UnregisterPaletteControlCallBack ();
    if (WebUIPalette::HasInstance ())
        WebUIPalette::DestroyInstance ();
    WebUIPalette::UnregisterPaletteControlCallBack ();
    geomsrv::ShutdownSharedHttpServer ();
    // E25: stop the background arming thread FIRST, then take the observer off.
    // Order matters both ways round: a worker still submitting gate jobs into an
    // unloading add-on, or a callback still registered into one, each take
    // Archicad down with them. Stop() joins, so this returns only once the
    // worker is genuinely gone.
    geomsrv::ArmWorker::Get ().Stop ();
    // The viewer's producer, on the same terms. Usually already stopped by the
    // panel teardown above — but EvP.ViewerRefresh can be called with no palette
    // instance at all, and then this is the only thing that joins it.
    geomsrv::archviz::ExtractionWorker::Get ().Stop ();
    geomsrv::UninstallChangeObserver ();
    // Raw Win32 windows of our own, same hazard as the observer above: a window
    // whose WndProc lives in this DLL must not survive the unload. Archicad
    // destroys its window tree at quit and would call into freed code.
    geomsrv::ShutdownPlanOverlay ();
    // The 3D overlay's window and class, on exactly the same terms -- its
    // WndProc lives in this DLL too, and PlanOverlay's crashed Archicad on close
    // once before this rule was written down.
    geomsrv::archviz::viewportoverlay::Shutdown ();
    // Detach the project-event handler so the add-on can unload cleanly.
    ACAPI_ProjectOperation_CatchProjectEvent (
        APINotify_New | APINotify_NewAndReset | APINotify_Open | APINotify_Close | APINotify_Quit, nullptr);
    return NoError;
}
