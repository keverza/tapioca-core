#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/ViewerSyncCommands.hpp"
#include "NativeCommands/CommandRegistration.hpp"

#include "ArchViz/ArchVizPanel.hpp"
#include "ArchViz/CameraSyncMode.hpp"
#include "ArchViz/CameraWake.hpp"
#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/Dxgi/HookMarker.hpp"
#include "ArchViz/Dxgi/HostComposite.hpp"
#include "ArchViz/Dxgi/PresentHook.hpp"
#include "ArchViz/ExperimentGuard.hpp"
#include "ArchViz/ModelWatch.hpp"
#include "ArchViz/NavLog.hpp"
#include "ArchViz/SelectionBridge.hpp"
#include "Python/MainThreadGate.hpp"

namespace geomsrv {

namespace {

namespace av = archviz;

class GetArchicad3DCameraCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetArchicad3DCamera"; }
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        // ACAPI, so it must be the main thread -- which MainThreadCommand
        // already guarantees. See ArchVizPanel::ReadArchicadCamera.
        const av::CameraStart camera = ArchVizPanel::ReadArchicadCamera ();
        GS::ObjectState os;
        os.Add ("valid", camera.valid);
        os.Add ("source", GS::UniString (camera.source.c_str (), CC_UTF8));
        os.Add ("eyeX", camera.eye[0]);
        os.Add ("eyeY", camera.eye[1]);
        os.Add ("eyeZ", camera.eye[2]);
        os.Add ("targetX", camera.target[0]);
        os.Add ("targetY", camera.target[1]);
        os.Add ("targetZ", camera.target[2]);
        os.Add ("viewConeDegreesHorizontal", camera.viewConeDegreesHorizontal);
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.RefreshDiligentModel {} -> { started }
//
// Rebuild the viewer's geometry NOW. The manual half of PLAT-RE125: the watch
// timer follows what Archicad's difference generator calls a change, and this is
// for everything else -- "the picture looks wrong, rebuild it", which no change
// detector can be asked to infer.
// ---------------------------------------------------------------------------
class RefreshDiligentModelCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "RefreshDiligentModel"; }
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        const bool started = av::modelwatch::RefreshNow ();
        GS::ObjectState os;
        // ⚠️ FALSE IS NOT A FAILURE, it is "a pass is already running" -- which is
        // the answer the caller wanted anyway (the model IS being rebuilt), and
        // reporting it as an error would make a harmless double-click look broken.
        os.Add ("started", started);
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.DiligentModelWatchState {} -> { running, polls, ... }
//
// What the watch is actually doing. `intervalMs` is the cadence it SETTLED on,
// not one that was configured: it adapts to the measured cost of the difference
// generator on this project, so reading it back is the only way to know what that
// cost turned out to be.
// ---------------------------------------------------------------------------
class DiligentModelWatchStateCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "DiligentModelWatchState"; }
    bool NeedsMainThread () const override { return false; }
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        const auto stats = av::modelwatch::Get ();
        GS::ObjectState os;
        os.Add ("running", stats.running);
        os.Add ("polls", (GS::Int32) stats.polls);
        os.Add ("skippedBusy", (GS::Int32) stats.skippedBusy);
        os.Add ("refreshes", (GS::Int32) stats.refreshes);
        os.Add ("lastDiffMs", (GS::Int64) stats.lastDiffMs);
        os.Add ("worstDiffMs", (GS::Int64) stats.worstDiffMs);
        os.Add ("intervalMs", (GS::Int32) stats.intervalMs);
        os.Add ("lastError", GS::UniString (stats.lastError.c_str (), CC_UTF8));
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.SyncDiligentCameraOnce {} -> { synced, source }
//
// Put the panel viewport back where Archicad is looking, ONCE. This is the
// manual counterpart to the panel's contract (PLAT-RE124): the panel takes
// Archicad's camera at open and then navigates standalone, so the only way back
// to Archicad's viewpoint is to ask for it -- from a script, or from the HUD
// button that calls this same path.
//
// ⚠️ AdoptCamera, NOT SyncCamera. SyncCamera is refused on the panel on purpose;
// a one-shot is a different intent and must not be expressible as "arm the
// follow for one tick", which is how the panel would lose its contract by
// increments.
//
// ⚠️ MAIN THREAD: ReadArchicadCamera calls ACAPI_View_Get3DProjectionSets.
// MainThreadCommand already puts us there, so no gate hop is needed.
// ---------------------------------------------------------------------------
class SyncDiligentCameraOnceCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "SyncDiligentCameraOnce"; }
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        if (!av::DiligentViewport::Get ().IsRunning ())
            return NativeCommandResult::Failure (
                EVP_FAIL ("the Diligent viewport is not running",
                          "syncing the viewport camera from Archicad"));

        const av::CameraStart camera = ArchVizPanel::ReadArchicadCamera ();
        if (!camera.valid)
            return NativeCommandResult::Failure (EVP_FAIL (
                "Archicad's current window has no readable camera -- an axonometric 3D "
                "projection has no eye position, and a non-3D window has none at all",
                "syncing the viewport camera from Archicad"));

        av::DiligentViewport::Get ().AdoptCamera (camera);
        GS::ObjectState os;
        os.Add ("synced", true);
        os.Add ("source", GS::UniString (camera.source.c_str (), CC_UTF8));
        return os;
    }
};

class SetDiligentCameraCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "SetDiligentCamera"; }
    // No ACAPI: it hands seven floats to a thread-safe setter on the viewport.
    bool NeedsMainThread () const override { return false; }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        av::CameraStart camera;
        double value = 0.0;
        params.Get ("eyeX", value);    camera.eye[0] = float (value);
        params.Get ("eyeY", value);    camera.eye[1] = float (value);
        params.Get ("eyeZ", value);    camera.eye[2] = float (value);
        params.Get ("targetX", value); camera.target[0] = float (value);
        params.Get ("targetY", value); camera.target[1] = float (value);
        params.Get ("targetZ", value); camera.target[2] = float (value);
        params.Get ("viewConeDegreesHorizontal", value);
        camera.viewConeDegreesHorizontal = float (value);
        camera.source = "synced";
        camera.valid = true;

        if (!av::DiligentViewport::Get ().IsRunning ())
            return NativeCommandResult::Failure (
                EVP_FAIL ("the Diligent viewport is not running",
                          "pushing a camera into the Diligent viewport"));

        // ⚠️ Adopt, NOT Sync. This is a caller SAYING where to look, which is a
        // one-shot request; SyncCamera means "keep following Archicad" and is
        // refused on the panel by design (PLAT-RE124).
        av::DiligentViewport::Get ().AdoptCamera (camera);
        GS::ObjectState os;
        os.Add ("accepted", true);
        return os;
    }
};

class GetDiligentCameraCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetDiligentCamera"; }
    bool NeedsMainThread () const override { return false; }

    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        av::DiligentViewport& viewport = av::DiligentViewport::Get ();
        av::CameraStart camera;
        if (!viewport.IsRunning () || viewport.Mode () == av::SurfaceMode::Offscreen ||
            !viewport.CurrentCamera (camera)) {
            return NativeCommandResult::Failure (
                "a visible Diligent viewport with a perspective camera is required");
        }

        GS::ObjectState os;
        os.Add ("valid", camera.valid);
        os.Add ("source", GS::UniString (camera.source.c_str (), CC_UTF8));
        os.Add ("orthographic", camera.orthographic);
        os.Add ("viewMoving", camera.viewMoving);
        os.Add ("eyeX", double (camera.eye[0]));
        os.Add ("eyeY", double (camera.eye[1]));
        os.Add ("eyeZ", double (camera.eye[2]));
        os.Add ("targetX", double (camera.target[0]));
        os.Add ("targetY", double (camera.target[1]));
        os.Add ("targetZ", double (camera.target[2]));
        os.Add ("viewConeDegreesHorizontal", double (camera.viewConeDegreesHorizontal));
        return os;
    }
};

// Follow Archicad's 3D window continuously, from a main-thread Win32 timer.
//
// ⚠️ THIS REPLACES THE PYTHON POLLING LOOP AND IS NOT THE SAME THING. Driving
// the sync over the bus meant two MainThreadCommands per tick, and during a drag
// in the 3D window Archicad's main thread is inside its own modal loop and
// dispatches neither -- so the viewport only caught up ON MOUSE RELEASE. The
// timer keeps firing inside that loop. See ArchVizPanel::StartCameraSync.
//
// ⚠️ IT NOW GOES THROUGH THE MODE SWITCH (PLAT-RE81) rather than calling
// Start/StopCameraSync itself. The boolean is kept -- every existing probe and
// command sends it -- but two arm paths would mean `CameraSyncModeState` could
// report a mode the machine is not actually in, which is the one thing a switch
// built for reversibility must never do. `enabled` is exactly `legacy`/`off`.
class SetDiligentCameraSyncCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "SetDiligentCameraSync"; }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        bool enabled = false;
        params.Get ("enabled", enabled);
        GS::Int32 intervalMs = 33;
        params.Get ("intervalMs", intervalMs);

        // ⚠️ SetTimer/KillTimer MUST run on the thread that pumps the queue, and
        // MainThreadCommand already puts us there. Posting would be wrong: the
        // timer would belong to whichever thread the post happened to run on.
        std::string error;
        const bool ok = av::SetCameraSyncMode (
            enabled ? av::CameraSyncMode::Legacy : av::CameraSyncMode::Off,
            (uint32_t) intervalMs, av::CurrentPredictionScale (), error);
        if (!ok)
            return NativeCommandResult::Failure (
                EVP_FAIL (GS::UniString (error.c_str (), CC_UTF8),
                          "starting the overlay camera sync"));

        GS::ObjectState os;
        os.Add ("enabled", av::CurrentCameraSyncMode () != av::CameraSyncMode::Off);
        os.Add ("intervalMs", intervalMs);
        return os;
    }
};

// ---- the camera-sync mode switch (PLAT-RE81) --------------------------------
//
// The single seam that selects HOW the overlay follows Archicad. See
// ArchViz/CameraSyncMode.hpp for why the mechanism is a runtime choice: the
// camera-sync ladder compares mechanisms against the same gestures with a human
// judging the result, and a mechanism that costs a rebuild to try is a
// mechanism that gets compared from memory.
//
// ⚠️ IT IS ALSO THE OFF SWITCH FOR EVERY EXPERIMENT BELOW IT. `mode: "legacy"`
// retracts anything armed, in the same session, with no rebuild -- which is what
// makes the DXGI work later on the ladder safe to attempt at all.
class SetCameraSyncModeCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "SetCameraSyncMode"; }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString requested;
        params.Get ("mode", requested);
        GS::Int32 intervalMs = (GS::Int32) av::CurrentCameraSyncIntervalMs ();
        params.Get ("intervalMs", intervalMs);

        av::CameraSyncMode mode = av::CameraSyncMode::Off;
        const std::string name (requested.ToCStr (0, MaxUSize, CC_UTF8).Get ());
        if (!av::ParseCameraSyncMode (name, mode))
            return NativeCommandResult::Failure (
                EVP_FAIL ("unknown camera sync mode '" + requested +
                              "'; expected off, legacy, hideonnav, wake, predict, wakepredict, hookdiag or hookdraw",
                          "setting the camera sync mode"));

        // Defaults to whatever is already set, so a caller switching modes without
        // an opinion about the horizon does not silently reset someone's sweep.
        double predictionScale = av::CurrentPredictionScale ();
        params.Get ("predictionScale", predictionScale);

        std::string error;
        if (!av::SetCameraSyncMode (mode, (uint32_t) intervalMs, predictionScale, error))
            return NativeCommandResult::Failure (
                EVP_FAIL (GS::UniString (error.c_str (), CC_UTF8),
                          "setting the camera sync mode to '" + requested + "'"));

        GS::ObjectState os;
        os.Add ("mode", GS::UniString (av::CameraSyncModeName (av::CurrentCameraSyncMode ()), CC_UTF8));
        os.Add ("intervalMs", (GS::Int32) av::CurrentCameraSyncIntervalMs ());
        return os;
    }
};

class CameraSyncModeStateCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "CameraSyncModeState"; }
    bool NeedsMainThread () const override { return false; }
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        GS::ObjectState os;
        os.Add ("mode", GS::UniString (av::CameraSyncModeName (av::CurrentCameraSyncMode ()), CC_UTF8));
        os.Add ("intervalMs", (GS::Int32) av::CurrentCameraSyncIntervalMs ());
        os.Add ("predictionScale", av::CurrentPredictionScale ());
        os.Add ("experimentsBlocked", av::experimentguard::Blocked ());
        os.Add ("experimentsBlockedWhy",
                GS::UniString (av::experimentguard::WhyBlocked ().c_str (), CC_UTF8));
        // Reported, never hard-coded by a caller -- see ExperimentGuard.hpp.
        os.Add ("breadcrumbPath",
                GS::UniString (av::experimentguard::BreadcrumbFilePath ().c_str (), CC_UTF8));
        os.Add ("safeModePath",
                GS::UniString (av::experimentguard::SafeModeFilePath ().c_str (), CC_UTF8));

        // The wake hook's counters. `pollsCoalesced` against `pollsPosted` is the
        // number that says whether a faster wake source can still help: if
        // coalescing dominates, input is arriving faster than the ACAPI read can
        // service it and the read is the bottleneck, not the wake.
        const auto wake = av::camerawake::GetStats ();
        os.Add ("wakeInstalled", wake.installed);
        os.Add ("wakeWheelEvents", (GS::Int32) wake.wheelEvents);
        os.Add ("wakeDragEvents", (GS::Int32) wake.dragEvents);
        os.Add ("wakeKeyEvents", (GS::Int32) wake.keyEvents);
        os.Add ("pollsPosted", (GS::Int32) wake.pollsPosted);
        os.Add ("pollsCoalesced", (GS::Int32) wake.pollsCoalesced);

        // The Present hook's view of the frame clock (PLAT-RE78).
        const auto present = av::dxgi::GetPresentStats ();
        os.Add ("presentHookInstalled", present.installed);
        os.Add ("presentCalls", (GS::Int32) present.presentCalls);
        os.Add ("present1Calls", (GS::Int32) present.present1Calls);
        os.Add ("presentResizeCalls", (GS::Int32) present.resizeCalls);
        os.Add ("busiestFrameCount", (GS::Int32) present.busiestFrameCount);
        os.Add ("medianFrameUs", (GS::Int32) present.medianFrameUs);
        os.Add ("p95FrameUs", (GS::Int32) present.p95FrameUs);

        // The phase-3 marker (PLAT-RE79). `markerTarget` being 0 while the mode
        // is armed is the EXPECTED first second, not a fault: the chain is
        // nominated once enough frames have gone through the hook to identify
        // it. `markerDraws` staying 0 after that is the real failure, and
        // `markerLastError` says which D3D step refused.
        const auto marker = av::dxgi::GetMarkerStats ();
        os.Add ("markerEnabled", marker.enabled);
        os.Add ("markerTargetChosen", marker.target != 0);
        os.Add ("markerDraws", (GS::Int32) marker.draws);
        os.Add ("markerFailures", (GS::Int32) marker.failures);
        os.Add ("markerLastError", GS::UniString (marker.lastError.c_str (), CC_UTF8));

        // The phase-4 compositor. `compositeBlits` climbing with
        // `compositeFramesConsumed` well behind it is NORMAL -- Archicad
        // presents more often than the overlay finishes a frame, and the blit
        // redraws the last one rather than skipping. Both at zero with
        // `compositeReady` false means the reason is in `compositeLastError`.
        const auto composite = av::dxgi::GetHostCompositeStats ();
        os.Add ("compositeEnabled", composite.enabled);
        os.Add ("compositeReady", composite.ready);
        os.Add ("compositeBlits", (GS::Int32) composite.blits);
        os.Add ("compositeFramesConsumed", (GS::Int32) composite.framesConsumed);
        os.Add ("compositeReprojections", (GS::Int32) composite.reprojections);
        os.Add ("compositeFailures", (GS::Int32) composite.failures);
        os.Add ("compositeBackBufferFormat", (GS::Int32) composite.backBufferFormat);
        os.Add ("compositeWidth", (GS::Int32) composite.width);
        os.Add ("compositeHeight", (GS::Int32) composite.height);
        os.Add ("compositeLastError", GS::UniString (composite.lastError.c_str (), CC_UTF8));
        return os;
    }
};

// ---- the navigation comparison log (ArchViz/NavLog.hpp) ---------------------
//
// ⚠️ THIS COMMAND DID NOT EXIST UNTIL PLAT-RE73, THOUGH THE DOCS SAID IT DID.
// NavLog.hpp's own header block advertises `EvP.ViewerNavLog {enable,
// intervalMs}` as the way to turn the log on, and nothing ever registered it --
// the only caller was the panel's own teardown. Every "run the nav log" step in
// the handoffs was therefore unrunnable from a script, which is one reason the
// 2026-08-06 samples were never followed up.
class ViewerNavLogCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "ViewerNavLog"; }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        bool enable = false;
        params.Get ("enable", enable);
        GS::Int32 intervalMs = 50;
        params.Get ("intervalMs", intervalMs);
        // ⚠️ DEFAULTS TO FALSE, unlike the C++ default. A caller over the bus is
        // almost always measuring the OVERLAY, and for that the sync tick is the
        // right and only source; the independent sampler is the special case
        // (viewer-vs-Archicad with no overlay) and should be asked for by name.
        bool sampler = false;
        params.Get ("sampler", sampler);

        // ⚠️ SetTimer belongs to the thread that pumps the queue, the same reason
        // SetDiligentCameraSync does not Post. MainThreadCommand puts us there.
        if (enable) {
            if (!ArchVizPanel::StartNavLog ((uint32_t) intervalMs, sampler))
                return NativeCommandResult::Failure (
                    EVP_FAIL ("SetTimer failed; Archicad's camera cannot be sampled and only "
                              "the viewer's half would have been written, which is not a "
                              "comparison",
                              "starting the navigation comparison log"));
        } else {
            ArchVizPanel::StopNavLog ();
        }

        const auto stats = av::navlog::GetStats ();
        GS::ObjectState os;
        os.Add ("running", stats.running);
        os.Add ("intervalMs", (GS::Int32) stats.intervalMs);
        os.Add ("viewerRows", (GS::Int64) stats.viewerRows);
        os.Add ("archicadRows", (GS::Int64) stats.archicadRows);
        os.Add ("archicadFails", (GS::Int64) stats.archicadFails);
        os.Add ("maxArchicadGapMs", (GS::Int64) stats.maxArchicadGapMs);
        // ⚠️ DECLARED IN THE SCHEMA SINCE PLAT-RE102 AND NEVER ACTUALLY SENT.
        // The matrix branches on them to warn that the file on disk is
        // incomplete; absent from the response, `.get(..., 0)` read zero and the
        // warning could not fire, so a run whose log failed to write was
        // reported as trustworthy. The schema check cannot catch this direction
        // -- an optional field that is never sent is valid.
        os.Add ("writeFailures", (GS::Int64) stats.writeFailures);
        os.Add ("droppedRows", (GS::Int64) stats.droppedRows);
        return os;
    }
};

// One delimiter row, so the offline report can slice a run into matrix cells.
//
// ⚠️ NO MAIN THREAD NEEDED and that is deliberate: the probe calls this twice per
// cell, around a gesture the user is performing, and a gate hop per mark would
// put the delimiter somewhere other than where the gesture actually started.
// NavLog is mutex-protected and callable from any thread by design.
class NavLogMarkCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "NavLogMark"; }
    bool NeedsMainThread () const override { return false; }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString label;
        params.Get ("label", label);
        av::navlog::Mark (std::string (label.ToCStr (0, MaxUSize, CC_UTF8).Get ()));

        GS::ObjectState os;
        os.Add ("marked", av::navlog::IsRunning ());
        return os;
    }
};

// Which directions of the selection bridge are live (PLAT-RE34).
//
// ⚠️ IT IS TWO FLAGS, NOT ONE SWITCH, because the two directions are separately
// wanted -- see ArchVizPanel::SelectionBridgeFlags. The default when the viewport
// opens is OFF: a viewer that rewrites the user's selection the moment it appears
// is a surprise, and the overlay path in particular must be able to watch without
// touching.
//
// ⚠️ SetTimer/KillTimer MUST run on the thread that pumps the queue, and
// MainThreadCommand already puts us there -- the same reason
// SetDiligentCameraSync does not Post.
class SetDiligentSelectionBridgeCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "SetDiligentSelectionBridge"; }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        bool toArchicad = false;
        bool toViewer = false;
        params.Get ("toArchicad", toArchicad);
        params.Get ("toViewer", toViewer);

        int mode = av::selectionbridge::Off;
        if (toArchicad)
            mode |= av::selectionbridge::ToArchicad;
        if (toViewer)
            mode |= av::selectionbridge::ToViewer;

        if (mode != av::selectionbridge::Off && !av::DiligentViewport::Get ().IsRunning ())
            return NativeCommandResult::Failure (
                EVP_FAIL ("no viewer is running",
                          "arming the selection bridge"));

        const bool running = av::selectionbridge::Start (mode);
        if (mode != av::selectionbridge::Off && !running)
            return NativeCommandResult::Failure (
                EVP_FAIL ("SetTimer failed; selection cannot cross between the viewer and "
                          "Archicad",
                          "arming the selection bridge"));

        const int active = av::selectionbridge::Mode ();
        GS::ObjectState os;
        os.Add ("toArchicad", (active & av::selectionbridge::ToArchicad) != 0);
        os.Add ("toViewer", (active & av::selectionbridge::ToViewer) != 0);
        return os;
    }
};
const NativeCommandRegistration kViewerSyncCommandRegistrations[] = {
    { "SetDiligentSelectionBridge", &MakeRegisteredNativeCommand<SetDiligentSelectionBridgeCommand>, false,
      R"json({"type":"object","properties":{"toArchicad":{"type":"boolean"},"toViewer":{"type":"boolean"}},"additionalProperties":false,"required":["toArchicad","toViewer"]})json",
      R"json({"type":"object","properties":{"toArchicad":{"type":"boolean"},"toViewer":{"type":"boolean"}},"additionalProperties":false,"required":["toArchicad","toViewer"]})json" },
    { "SetDiligentCameraSync", &MakeRegisteredNativeCommand<SetDiligentCameraSyncCommand>, false,
      R"json({"type":"object","properties":{"enabled":{"type":"boolean"},"intervalMs":{"type":"integer","minimum":10,"maximum":1000}},"additionalProperties":false,"required":["enabled","intervalMs"]})json",
      R"json({"type":"object","properties":{"enabled":{"type":"boolean"},"intervalMs":{"type":"integer","minimum":10,"maximum":1000}},"additionalProperties":false,"required":["enabled","intervalMs"]})json" },
    { "SetCameraSyncMode", &MakeRegisteredNativeCommand<SetCameraSyncModeCommand>, false,
      R"json({"type":"object","properties":{"mode":{"type":"string","enum":["off","legacy","hideonnav","wake","predict","wakepredict","hookdiag","hookdraw"]},"intervalMs":{"type":"integer","minimum":10,"maximum":1000},"predictionScale":{"type":"number","minimum":0,"maximum":4}},"additionalProperties":false,"required":["mode"]})json",
      R"json({"type":"object","properties":{"mode":{"type":"string"},"intervalMs":{"type":"integer","minimum":10,"maximum":1000},"predictionScale":{"type":"number","minimum":0,"maximum":4}},"additionalProperties":false,"required":["mode","intervalMs"]})json" },
    { "ViewerNavLog", &MakeRegisteredNativeCommand<ViewerNavLogCommand>, false,
      R"json({"type":"object","properties":{"enable":{"type":"boolean"},"intervalMs":{"type":"integer","minimum":0,"maximum":5000},"sampler":{"type":"boolean"}},"additionalProperties":false,"required":["enable"]})json",
      R"json({"type":"object","properties":{"running":{"type":"boolean"},"intervalMs":{"type":"integer","minimum":0},"viewerRows":{"type":"integer","minimum":0},"archicadRows":{"type":"integer","minimum":0},"archicadFails":{"type":"integer","minimum":0},"maxArchicadGapMs":{"type":"integer","minimum":0},"writeFailures":{"type":"integer","minimum":0},"droppedRows":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["running","intervalMs","viewerRows","archicadRows","archicadFails","maxArchicadGapMs"]})json" },
    { "NavLogMark", &MakeRegisteredNativeCommand<NavLogMarkCommand>, false,
      R"json({"type":"object","properties":{"label":{"type":"string"}},"additionalProperties":false,"required":["label"]})json",
      R"json({"type":"object","properties":{"marked":{"type":"boolean"}},"additionalProperties":false,"required":["marked"]})json" },
    { "CameraSyncModeState", &MakeRegisteredNativeCommand<CameraSyncModeStateCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"mode":{"type":"string"},"intervalMs":{"type":"integer","minimum":10,"maximum":1000},"experimentsBlocked":{"type":"boolean"},"experimentsBlockedWhy":{"type":"string"},"predictionScale":{"type":"number"},"breadcrumbPath":{"type":"string"},"safeModePath":{"type":"string"},"wakeInstalled":{"type":"boolean"},"wakeWheelEvents":{"type":"integer"},"wakeDragEvents":{"type":"integer"},"wakeKeyEvents":{"type":"integer"},"pollsPosted":{"type":"integer"},"pollsCoalesced":{"type":"integer"},"presentHookInstalled":{"type":"boolean"},"presentCalls":{"type":"integer"},"present1Calls":{"type":"integer"},"presentResizeCalls":{"type":"integer"},"busiestFrameCount":{"type":"integer"},"medianFrameUs":{"type":"integer"},"p95FrameUs":{"type":"integer"},"markerEnabled":{"type":"boolean"},"markerTargetChosen":{"type":"boolean"},"markerDraws":{"type":"integer"},"markerFailures":{"type":"integer"},"markerLastError":{"type":"string"},"compositeEnabled":{"type":"boolean"},"compositeReady":{"type":"boolean"},"compositeBlits":{"type":"integer"},"compositeFramesConsumed":{"type":"integer"},"compositeReprojections":{"type":"integer"},"compositeFailures":{"type":"integer"},"compositeBackBufferFormat":{"type":"integer"},"compositeWidth":{"type":"integer"},"compositeHeight":{"type":"integer"},"compositeLastError":{"type":"string"}},"additionalProperties":false,"required":["mode","intervalMs","experimentsBlocked","experimentsBlockedWhy"]})json" },
    { "GetArchicad3DCamera", &MakeRegisteredNativeCommand<GetArchicad3DCameraCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"valid":{"type":"boolean"},"source":{"type":"string"},"eyeX":{"type":"number"},"eyeY":{"type":"number"},"eyeZ":{"type":"number"},"targetX":{"type":"number"},"targetY":{"type":"number"},"targetZ":{"type":"number"},"viewConeDegreesHorizontal":{"type":"number","minimum":0,"maximum":180}},"additionalProperties":false,"required":["valid","source","eyeX","eyeY","eyeZ","targetX","targetY","targetZ","viewConeDegreesHorizontal"]})json" },
    { "GetDiligentCamera", &MakeRegisteredNativeCommand<GetDiligentCameraCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"valid":{"type":"boolean"},"source":{"type":"string"},"orthographic":{"type":"boolean"},"viewMoving":{"type":"boolean"},"eyeX":{"type":"number"},"eyeY":{"type":"number"},"eyeZ":{"type":"number"},"targetX":{"type":"number"},"targetY":{"type":"number"},"targetZ":{"type":"number"},"viewConeDegreesHorizontal":{"type":"number","exclusiveMinimum":1,"exclusiveMaximum":179}},"additionalProperties":false,"required":["valid","source","orthographic","viewMoving","eyeX","eyeY","eyeZ","targetX","targetY","targetZ","viewConeDegreesHorizontal"]})json" },
    { "SetDiligentCamera", &MakeRegisteredNativeCommand<SetDiligentCameraCommand>, false,
      R"json({"type":"object","properties":{"eyeX":{"type":"number"},"eyeY":{"type":"number"},"eyeZ":{"type":"number"},"targetX":{"type":"number"},"targetY":{"type":"number"},"targetZ":{"type":"number"},"viewConeDegreesHorizontal":{"type":"number","minimum":0,"maximum":180}},"additionalProperties":false,"required":["eyeX","eyeY","eyeZ","targetX","targetY","targetZ","viewConeDegreesHorizontal"]})json",
      R"json({"type":"object","properties":{"accepted":{"type":"boolean"}},"additionalProperties":false,"required":["accepted"]})json" },
    { "RefreshDiligentModel", &MakeRegisteredNativeCommand<RefreshDiligentModelCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"started":{"type":"boolean"}},"additionalProperties":false,"required":["started"]})json" },
    { "DiligentModelWatchState", &MakeRegisteredNativeCommand<DiligentModelWatchStateCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"running":{"type":"boolean"},"polls":{"type":"integer"},"skippedBusy":{"type":"integer"},"refreshes":{"type":"integer"},"lastDiffMs":{"type":"integer"},"worstDiffMs":{"type":"integer"},"intervalMs":{"type":"integer"},"lastError":{"type":"string"}},"additionalProperties":false,"required":["running","polls","skippedBusy","refreshes","lastDiffMs","worstDiffMs","intervalMs","lastError"]})json" },
    { "SyncDiligentCameraOnce", &MakeRegisteredNativeCommand<SyncDiligentCameraOnceCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"synced":{"type":"boolean"},"source":{"type":"string"}},"additionalProperties":false,"required":["synced","source"]})json" },
};

}   // namespace

NativeCommandRegistrations GetViewerSyncCommandRegistrations ()
{
    return MakeRegistrationView (kViewerSyncCommandRegistrations);
}

}   // namespace geomsrv
