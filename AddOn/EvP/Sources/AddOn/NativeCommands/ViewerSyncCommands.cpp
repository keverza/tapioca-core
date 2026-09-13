#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/ViewerSyncCommands.hpp"
#include "NativeCommands/CommandRegistration.hpp"

#include "ArchViz/ArchVizPanel.hpp"
#include "ArchViz/CameraSyncMode.hpp"
#include "ArchViz/CameraWake.hpp"
#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/Dxgi/ContextHook.hpp"
#include "ArchViz/Dxgi/DeviceIdentity.hpp"
#include "ArchViz/Dxgi/HookMarker.hpp"
#include "ArchViz/Dxgi/HostComposite.hpp"
#include "ArchViz/Dxgi/PresentHook.hpp"
#include "ArchViz/Dxgi/RenderStateCapture.hpp"
#include "ArchViz/Dxgi/ViewMatrixCandidates.hpp"
#include "ArchViz/ExperimentGuard.hpp"
#include "ArchViz/PatchProfile.hpp"
#include "ArchViz/ViewportOverlayWindow.hpp"
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
        // `CurrentHideOnNav ()` and not a literal: this command has no opinion
        // about blanking and never had one, so passing anything else would let a
        // caller that only wanted the timer on silently reset somebody's switch.
        const bool ok = av::SetCameraSyncMode (
            enabled ? av::CameraSyncMode::Legacy : av::CameraSyncMode::Off,
            (uint32_t) intervalMs, av::CurrentPredictionScale (), av::CurrentHideOnNav (),
            av::CurrentGpuState (), error);
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

        // Same rule, and the reason `hideOnNav` is a parameter here at all: it
        // composes with every mode now (PLAT-RE116), so it must be settable
        // WITHOUT choosing a mode and survivable when a mode is chosen without
        // it. Omitting it keeps whatever is set.
        bool hideOnNav = av::CurrentHideOnNav ();
        params.Get ("hideOnNav", hideOnNav);

        // The GPU-state discovery hooks (PLAT-RE153..RE155).
        //
        // ⚠️ AN EXPLICIT REQUEST IS REFUSED ON A MODE THAT CANNOT HOST IT; AN
        // INHERITED ONE IS SILENTLY DROPPED, AND THE DIFFERENCE IS NOT
        // COSMETIC. This used to inherit the current value and let the mode
        // switch refuse -- so after a `hookdiag` run had turned it on,
        // `{mode: "legacy"}` inherited `gpuState: true` and was REFUSED. That is
        // the teardown path, and `CameraSyncReset` sends exactly that call: the
        // mode became undisarmable at the one moment anybody needed to disarm
        // it. Only the caller layer can tell "asked for" from "said nothing", so
        // this is where the distinction has to live.
        bool gpuState = false;
        const bool gpuStateRequested = params.Get ("gpuState", gpuState);
        if (!gpuStateRequested)
            gpuState = av::CurrentGpuState ();
        if (gpuStateRequested && gpuState && mode != av::CameraSyncMode::HookDiag)
            return NativeCommandResult::Failure (
                EVP_FAIL ("the GPU-state discovery hooks compose with 'hookdiag' only -- "
                          "without the Present detour there is no frame boundary, no "
                          "identified swap chain and no context to record, so they would "
                          "install and see nothing",
                          "setting the camera sync mode to '" + requested + "'"));

        std::string error;
        if (!av::SetCameraSyncMode (mode, (uint32_t) intervalMs, predictionScale, hideOnNav,
                                    gpuState, error))
            return NativeCommandResult::Failure (
                EVP_FAIL (GS::UniString (error.c_str (), CC_UTF8),
                          "setting the camera sync mode to '" + requested + "'"));

        GS::ObjectState os;
        os.Add ("mode", GS::UniString (av::CameraSyncModeName (av::CurrentCameraSyncMode ()), CC_UTF8));
        os.Add ("intervalMs", (GS::Int32) av::CurrentCameraSyncIntervalMs ());
        os.Add ("hideOnNav", av::CurrentHideOnNav ());
        // Echoed for the same reason `hideOnNav` is: it is a switch the mode
        // does not name, and a caller that omitted it has to be able to see what
        // it inherited.
        os.Add ("gpuState", av::CurrentGpuState ());
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
        // ⚠️ REPORTED SEPARATELY FROM `mode`, because it no longer lives in it.
        // A caller reading `mode == "wakepredict"` learns nothing about whether
        // the overlay blanks during motion, and a blanked overlay looks exactly
        // like a broken one to anything that only samples pixels.
        os.Add ("hideOnNav", av::CurrentHideOnNav ());
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

        // ---- the GPU-state discovery path (PLAT-RE153..RE155) --------------
        //
        // ⚠️ READ `gpuStateLastError` FIRST WHEN NOTHING IS HAPPENING. The most
        // likely reason `gpuStateInstalled` is false with `gpuState` true is not
        // a bug: it is the patch profile refusing an unpinned or changed build,
        // which is the designed behaviour and says so in one sentence.
        os.Add ("gpuState", av::CurrentGpuState ());
        const auto context = av::dxgi::GetContextHookStats ();
        os.Add ("gpuStateInstalled", context.installed);
        // ⚠️ `wanted` WITHOUT `installed` IS THE NORMAL FIRST SECOND OF A RUN, not
        // a fault. The hook cannot go up until the present detour has identified
        // Archicad's swap chain and read its context off it, which takes about
        // sixty of Archicad's frames -- so a caller that reads these two the
        // moment after arming must not conclude anything from the gap.
        os.Add ("gpuStateWanted", context.wanted);
        os.Add ("gpuStatePinned", context.pinned);
        os.Add ("gpuStateContextFound", context.discoveredContext != 0);
        os.Add ("gpuStateContextChosen", context.archicadContext != 0);
        // ⚠️ HOW WELL THE ELEVEN VTABLE INDICES ARE PROVEN, and it is not a
        // yes/no. The COM ABI fixes the method order and that is what carries the
        // weight; the call-based self-test is belt-and-braces and is only
        // available when a throwaway device lands on the same table, which on
        // 2026-09-13 it did not even with Archicad's own creation flags.
        os.Add ("gpuStateProof", GS::UniString (context.proof.c_str (), CC_UTF8));
        os.Add ("gpuStateCallSpanMs", (GS::Int32) (context.callSpanUs / 1000));
        os.Add ("gpuStateSlotsStillPatched", (GS::Int32) context.slotsStillPatched);
        os.Add ("gpuStateRepairs", (GS::Int32) context.repairs);
        os.Add ("gpuStateCalls", (GS::Int32) context.calls);
        os.Add ("gpuStateOtherContextCalls", (GS::Int32) context.otherContextCalls);
        os.Add ("gpuStateEventsDropped", (GS::Int32) context.eventsDropped);
        os.Add ("gpuStateLastError", GS::UniString (context.lastError.c_str (), CC_UTF8));

        // Stage 2. `gpuFrames` climbing with a zero-width scene viewport means
        // the depth-clear heuristic found nothing this frame -- expected on the
        // floor plan, a finding in the 3D window.
        const auto capture = av::dxgi::renderstate::GetCaptureStats ();
        os.Add ("gpuFrames", (GS::Int32) capture.framesClosed);
        os.Add ("gpuFramesDropped", (GS::Int32) capture.framesDropped);
        os.Add ("gpuViewportWidth", (double) capture.sceneCandidate.width);
        os.Add ("gpuViewportHeight", (double) capture.sceneCandidate.height);
        os.Add ("gpuViewportX", (double) capture.sceneCandidate.x);
        os.Add ("gpuViewportY", (double) capture.sceneCandidate.y);
        os.Add ("gpuLargestWidth", (double) capture.largest.width);
        os.Add ("gpuLargestHeight", (double) capture.largest.height);
        os.Add ("gpuDistinctViewports", (GS::Int32) capture.distinctCount);

        // Stage 3. `gpuBestMaxPixelError` is the number the whole rung turns on:
        // under half a pixel at rest means the captured matrix IS Archicad's.
        const auto candidates = av::dxgi::viewmatrix::GetCandidateStats ();
        os.Add ("gpuReferenceValid", candidates.referenceValid);
        os.Add ("gpuBuffersTracked", (GS::Int32) candidates.buffersTracked);
        os.Add ("gpuBuffersDropped", (GS::Int32) candidates.buffersDropped);
        os.Add ("gpuWritesCaptured", (GS::Int32) candidates.writesCaptured);
        // Why a Map was refused. Only interesting when `gpuWritesCaptured` is 0,
        // which is exactly when the report used to have nothing to say.
        os.Add ("gpuMapsSeen", (GS::Int32) candidates.mapsSeen);
        os.Add ("gpuMapsNotConstantBuffer", (GS::Int32) candidates.mapsNotConstantBuffer);
        os.Add ("gpuMapsTooLarge", (GS::Int32) candidates.mapsTooLarge);
        os.Add ("gpuMapsNoSlot", (GS::Int32) candidates.mapsNoSlot);
        os.Add ("gpuLargestConstantBytes", (GS::Int32) candidates.largestConstantBytes);
        os.Add ("gpuBufferCacheFull", candidates.bufferCacheFull);
        os.Add ("gpuRegionsScored", (GS::Int32) candidates.regionsScored);
        os.Add ("gpuBestMaxPixelError", candidates.bestMaxPixelError);
        os.Add ("gpuBestByteOffset", (GS::Int32) candidates.bestByteOffset);
        os.Add ("gpuBestVariant", (GS::Int32) candidates.bestVariant);

        // The pin, reported rather than spelled out by a caller -- the rule
        // ExperimentGuard learned when the recovery instruction named a file the
        // code had never written.
        os.Add ("patchProfilePath",
                GS::UniString (av::patchprofile::PinFilePath ().c_str (), CC_UTF8));
        os.Add ("patchProfilePinned", av::patchprofile::HasPin ());
        return os;
    }
};

// ---- the patch profile (PLAT-RE153, stage 1a) -------------------------------
//
// ⚠️ THIS IS THE ONLY WAY TO PIN A BUILD, AND IT IS DELIBERATELY A DECISION
// SOMEBODY MAKES. See ArchViz/PatchProfile.hpp: the GPU-state hooks read
// Archicad's own GPU buffers, whose layout moves with an Archicad update, so
// they refuse to install on anything but a build that was explicitly pinned
// after being tested. There is no compiled-in table of blessed hashes because
// there could not be an honest one -- this add-on is built on a machine that has
// no idea which Archicad the user runs.
//
// Called with no arguments it REPORTS: what is running, what is pinned, and
// where the file is. `pin: true` records the running build as the pinned one.
class ViewerPatchProfileCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "ViewerPatchProfile"; }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        bool pin = false;
        params.Get ("pin", pin);

        GS::ObjectState os;
        if (pin) {
            // ⚠️ THE TARGETS ARE FINGERPRINTED FIRST. A profile written without
            // them would verify the executable and check nothing about the slots
            // being patched -- `patchprofile::Pin` refuses that outright, and
            // this is where the discovery that prevents it happens.
            std::string error;
            if (!av::dxgi::FingerprintContextTargets (error))
                return NativeCommandResult::Failure (
                    EVP_FAIL (GS::UniString (error.c_str (), CC_UTF8),
                              "reading Archicad's own D3D11 context vtable to fingerprint "
                              "the hook targets"));
            if (!av::patchprofile::Pin (error))
                return NativeCommandResult::Failure (
                    EVP_FAIL (GS::UniString (error.c_str (), CC_UTF8),
                              "writing the patch profile"));
            // ⚠️ PINNING IS THE THING THAT CHANGES THE ANSWER, so it clears the
            // install latch. Without this the camera tick would keep returning
            // the refusal it cached before the profile existed, and the only way
            // to pick up a fresh pin would be to disarm and arm again -- which
            // reads as "pinning did not work".
            av::dxgi::RetryContextHookInstall ();
        }

        const auto& current = av::patchprofile::Current ();
        os.Add ("path", GS::UniString (av::patchprofile::PinFilePath ().c_str (), CC_UTF8));
        os.Add ("pinned", av::patchprofile::HasPin ());
        os.Add ("pinnedSummary",
                GS::UniString (av::patchprofile::PinnedSummary ().c_str (), CC_UTF8));
        os.Add ("currentSummary",
                GS::UniString (av::patchprofile::CurrentSummary ().c_str (), CC_UTF8));
        os.Add ("hostPath", GS::UniString (current.hostPath.c_str (), CC_UTF8));
        os.Add ("hostVersion", GS::UniString (current.hostVersion.c_str (), CC_UTF8));
        os.Add ("hostSha256", GS::UniString (current.hostSha256.c_str (), CC_UTF8));
        os.Add ("targets", (GS::Int32) current.targets.size ());

        // ⚠️ THE VERDICT IS REPORTED EVEN WHEN NOTHING IS ARMED, because "why is
        // it not syncing" has to be answerable on a machine nobody can attach a
        // debugger to, before anyone tries to arm anything.
        std::string verifyError;
        os.Add ("verifies", av::patchprofile::Verify (verifyError));
        os.Add ("verifyError", GS::UniString (verifyError.c_str (), CC_UTF8));
        return os;
    }
};

// ---- the GPU-state discovery slots (PLAT-RE153, stage 1) --------------------
//
// ⚠️ WITHOUT THIS COMMAND STAGE 3 CAN NEVER SCORE ANYTHING. The three slots that
// carry constant-buffer CONTENTS -- Map, Unmap and UpdateSubresource -- default
// to OFF at every arm, because reading a mapped upload buffer is an uncached
// read of write-combined memory on Archicad's render thread and PLAT-RE118
// measured that thread to be sensitive to added work. The cheap slots answer
// stages 1 and 2 on their own; stage 3 needs the expensive ones, and turning
// them on has to be a deliberate act taken AFTER the frame clock has been
// checked without them.
//
// ⚠️ THE DEFAULTS ARE REAPPLIED AT EVERY ARM, so this is something a run does
// AFTER `SetCameraSyncMode`, never before. Reporting the live state rather than
// the intent is what makes that safe to get wrong.
class ViewerGpuStateSlotsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "ViewerGpuStateSlots"; }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        // The shorthand, and the one a run actually wants: the three slots that
        // capture buffer contents, named for what they do rather than for three
        // D3D method names a caller should not have to know.
        bool constantBuffers = false;
        if (params.Get ("constantBuffers", constantBuffers)) {
            av::dxgi::SetContextSlotEnabled (av::dxgi::ContextSlot::Map, constantBuffers);
            av::dxgi::SetContextSlotEnabled (av::dxgi::ContextSlot::Unmap, constantBuffers);
            av::dxgi::SetContextSlotEnabled (av::dxgi::ContextSlot::UpdateSubresource,
                                             constantBuffers);
        }

        // One slot by name, for isolating a cost to a single hook -- the header's
        // "enable one at a time" advice, made reachable.
        GS::UniString slotName;
        if (params.Get ("slot", slotName)) {
            bool enabled = true;
            params.Get ("enabled", enabled);
            const std::string wanted (slotName.ToCStr (0, MaxUSize, CC_UTF8).Get ());
            bool matched = false;
            for (uint32_t i = 0; i < uint32_t (av::dxgi::ContextSlot::Count); ++i) {
                const av::dxgi::ContextSlot slot = av::dxgi::ContextSlot (i);
                if (wanted == av::dxgi::ContextSlotName (slot)) {
                    av::dxgi::SetContextSlotEnabled (slot, enabled);
                    matched = true;
                    break;
                }
            }
            if (!matched)
                return NativeCommandResult::Failure (
                    EVP_FAIL ("unknown GPU-state slot '" + slotName +
                                  "'; ask with no arguments to list them",
                              "gating a GPU-state discovery slot"));
        }

        const auto stats = av::dxgi::GetContextHookStats ();
        GS::ObjectState os;
        os.Add ("installed", stats.installed);
        GS::Array<GS::ObjectState> slots;
        for (uint32_t i = 0; i < uint32_t (av::dxgi::ContextSlot::Count); ++i) {
            const av::dxgi::ContextSlot slot = av::dxgi::ContextSlot (i);
            GS::ObjectState row;
            row.Add ("name", GS::UniString (av::dxgi::ContextSlotName (slot), CC_UTF8));
            row.Add ("enabled", av::dxgi::ContextSlotEnabled (slot));
            row.Add ("calls", (GS::Int32) stats.perSlot[i]);
            slots.Push (row);
        }
        os.Add ("slots", slots);
        return os;
    }
};

// ---- the scored constant-buffer candidates (PLAT-RE155, stage 3) ------------
//
// ⚠️ THE COUNTERS ALONE CANNOT ANSWER STAGE 3. `CameraSyncModeState` reports the
// best candidate's pixel error, which says whether SOMETHING matched; it cannot
// say whether the thing that matched behaves like a camera. That takes the row:
// which buffer, at what offset, under which storage convention, and -- the
// discriminator the handoff actually asks for -- how often those 64 bytes
// changed while the view was moving against while it was still. A region with a
// good error and a high `changesWhileStill` is not a view matrix; it is a buffer
// that happens to hold sixteen agreeable floats this frame.
class ViewerGpuStateCandidatesCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "ViewerGpuStateCandidates"; }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Int32 limit = 8;
        params.Get ("limit", limit);
        if (limit < 1)
            limit = 1;
        if (limit > 16)
            limit = 16;

        av::dxgi::viewmatrix::Candidate found[16];
        const size_t count = av::dxgi::viewmatrix::Classify (found, size_t (limit));

        GS::ObjectState os;
        // ⚠️ `scored` FALSE IS THE COMMON CASE AND IS NOT A FAULT. Classification
        // only runs on a SETTLED view -- the reference camera is stale by
        // construction while the view moves, and scoring against it mid-drag
        // would penalise a candidate for being fresher than the reference, which
        // is the entire property being looked for. A caller that asks during a
        // drag gets nothing and should hold still and ask again.
        os.Add ("scored", count > 0);
        os.Add ("referenceValid", av::dxgi::viewmatrix::HasReference ());

        // Spelled out rather than reported as the raw enum: "1" means nothing to
        // somebody reading a log a month later, and the transpose being the
        // EXPECTED hit is the most useful thing a reader can be told here.
        static const char* const kVariants[] = {"as-stored", "transposed",
                                                "inverse", "inverse-transposed"};
        GS::Array<GS::ObjectState> rows;
        for (size_t i = 0; i < count; ++i) {
            const av::dxgi::viewmatrix::Candidate& candidate = found[i];
            GS::ObjectState row;
            // As a STRING: a buffer pointer does not fit an Int32, and the value
            // is only ever compared for equality across rows and runs.
            row.Add ("buffer",
                     GS::UniString (std::to_string (candidate.buffer).c_str (), CC_UTF8));
            row.Add ("byteOffset", (GS::Int32) candidate.byteOffset);
            row.Add ("byteWidth", (GS::Int32) candidate.byteWidth);
            row.Add ("variant", GS::UniString (kVariants[candidate.variant % 4], CC_UTF8));
            const char* stage = (candidate.shaderStage < uint32_t (av::dxgi::ContextSlot::Count))
                ? av::dxgi::ContextSlotName (av::dxgi::ContextSlot (candidate.shaderStage))
                : "(never seen bound)";
            row.Add ("boundBy", GS::UniString (stage, CC_UTF8));
            row.Add ("bindSlot", (GS::Int32) candidate.bindSlot);
            row.Add ("maxPixelError", candidate.maxPixelError);
            row.Add ("meanPixelError", candidate.meanPixelError);
            row.Add ("changesWhileMoving", (GS::Int32) candidate.changesWhileMoving);
            row.Add ("changesWhileStill", (GS::Int32) candidate.changesWhileStill);
            rows.Push (row);
        }
        os.Add ("candidates", rows);
        return os;
    }
};

// ---- which GPU API is Archicad's 3D window actually drawn with? -------------
//
// ⚠️ THIS COMMAND EXISTS BECAUSE THE HOOK WORKED AND SAW NOTHING. On 2026-09-13
// the context hook installed on Archicad's own immediate context, correctly and
// exclusively, and then recorded 53 viewport sets and 46 target binds across
// 2722 Archicad frames -- roughly two a second while the user orbited at 100 fps.
// A 3D scene pass does not look like that. Something else is drawing the model.
//
// Before reverse-engineering anything, two cheap in-process facts settle where
// to look, and this reports both:
//
//   * `is11On12` -- a D3D11On12 device means the real renderer is D3D12 and the
//     immediate context is a presentation shim. No amount of
//     `ID3D11DeviceContext` hooking will ever see a view matrix, because the
//     transform is going out through `ID3D12GraphicsCommandList` instead. That
//     moves the whole rung, and it is the single most valuable bit here.
//   * the swap-chain inventory -- the rival hypothesis is that we nominated the
//     wrong chain. `busiestSwapChain` reports a winner and hides the field;
//     this prints the field, with the window each one presents into.
//
// The draw counters in `CameraSyncModeState` are the third leg: if Archicad drew
// on this context we would see thousands per second, not tens.
class ViewerGpuDeviceInfoCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "ViewerGpuDeviceInfo"; }
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        const auto device = av::dxgi::deviceidentity::Describe (
            (ID3D11Device*) av::dxgi::DiscoveredArchicadDevice ());
        os.Add ("deviceFound", device.found);
        os.Add ("is11On12", device.is11On12);
        os.Add ("creationFlags", (GS::Int32) device.creationFlags);
        os.Add ("featureLevel", (GS::Int32) device.featureLevel);
        os.Add ("debugLayer", device.debugLayer);
        os.Add ("singleThreaded", device.singleThreaded);
        os.Add ("bgraSupport", device.bgraSupport);
        os.Add ("highestDeviceInterface", (GS::Int32) device.highestDeviceInterface);

        // Which graphics runtimes are live, and whether the canvas the overlay
        // covers is an OpenGL window. See DeviceIdentity.hpp: after five runs the
        // D3D11 path is exonerated and the question is which stack draws the
        // model at all.
        const auto stack = av::dxgi::deviceidentity::DescribeRenderStack (
            uint64_t (uintptr_t (av::viewportoverlay::Stats ().target)));
        os.Add ("openglLoaded", stack.openglLoaded);
        os.Add ("openglIcdLoaded", stack.openglIcdLoaded);
        os.Add ("d3d12Loaded", stack.d3d12Loaded);
        os.Add ("vulkanLoaded", stack.vulkanLoaded);
        os.Add ("d2dLoaded", stack.d2dLoaded);
        os.Add ("dcompLoaded", stack.dcompLoaded);
        os.Add ("targetWindow",
                GS::UniString (std::to_string (stack.targetWindow).c_str (), CC_UTF8));
        os.Add ("targetHasPixelFormat", stack.targetHasPixelFormat);
        os.Add ("targetPixelFormat", (GS::Int32) stack.targetPixelFormat);
        os.Add ("targetSupportsOpenGL", stack.targetSupportsOpenGL);
        os.Add ("targetSupportsGdi", stack.targetSupportsGdi);
        os.Add ("targetDoubleBuffered", stack.targetDoubleBuffered);

        av::dxgi::ChainInfo chains[8];
        const size_t count = av::dxgi::GetChainInventory (chains, 8);
        GS::Array<GS::ObjectState> rows;
        for (size_t i = 0; i < count; ++i) {
            GS::ObjectState row;
            // As strings: a swap chain or HWND does not fit an Int32, and these
            // are only ever compared for equality and read by eye.
            row.Add ("swapChain",
                     GS::UniString (std::to_string (chains[i].swapChain).c_str (), CC_UTF8));
            row.Add ("window",
                     GS::UniString (std::to_string (chains[i].window).c_str (), CC_UTF8));
            row.Add ("presents", (GS::Int32) chains[i].presents);
            row.Add ("width", (GS::Int32) chains[i].width);
            row.Add ("height", (GS::Int32) chains[i].height);
            row.Add ("ours", chains[i].ours);
            row.Add ("nominated", chains[i].nominated);
            rows.Push (row);
        }
        os.Add ("chains", rows);
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
      R"json({"type":"object","properties":{"mode":{"type":"string","enum":["off","legacy","hideonnav","wake","predict","wakepredict","hookdiag","hookdraw"]},"intervalMs":{"type":"integer","minimum":10,"maximum":1000},"predictionScale":{"type":"number","minimum":0,"maximum":4},"hideOnNav":{"type":"boolean"},"gpuState":{"type":"boolean"}},"additionalProperties":false,"required":["mode"]})json",
      R"json({"type":"object","properties":{"mode":{"type":"string"},"intervalMs":{"type":"integer","minimum":10,"maximum":1000},"predictionScale":{"type":"number","minimum":0,"maximum":4},"hideOnNav":{"type":"boolean"},"gpuState":{"type":"boolean"}},"additionalProperties":false,"required":["mode","intervalMs","hideOnNav"]})json" },
    { "ViewerGpuDeviceInfo", &MakeRegisteredNativeCommand<ViewerGpuDeviceInfoCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"deviceFound":{"type":"boolean"},"is11On12":{"type":"boolean"},"creationFlags":{"type":"integer"},"featureLevel":{"type":"integer"},"debugLayer":{"type":"boolean"},"singleThreaded":{"type":"boolean"},"bgraSupport":{"type":"boolean"},"highestDeviceInterface":{"type":"integer","minimum":0,"maximum":5},"openglLoaded":{"type":"boolean"},"openglIcdLoaded":{"type":"boolean"},"d3d12Loaded":{"type":"boolean"},"vulkanLoaded":{"type":"boolean"},"d2dLoaded":{"type":"boolean"},"dcompLoaded":{"type":"boolean"},"targetWindow":{"type":"string"},"targetHasPixelFormat":{"type":"boolean"},"targetPixelFormat":{"type":"integer"},"targetSupportsOpenGL":{"type":"boolean"},"targetSupportsGdi":{"type":"boolean"},"targetDoubleBuffered":{"type":"boolean"},"chains":{"type":"array","items":{"type":"object","properties":{"swapChain":{"type":"string"},"window":{"type":"string"},"presents":{"type":"integer"},"width":{"type":"integer"},"height":{"type":"integer"},"ours":{"type":"boolean"},"nominated":{"type":"boolean"}},"additionalProperties":false,"required":["swapChain","window","presents","ours","nominated"]}}},"additionalProperties":false,"required":["deviceFound","is11On12","chains"]})json" },
    { "ViewerGpuStateSlots", &MakeRegisteredNativeCommand<ViewerGpuStateSlotsCommand>, false,
      R"json({"type":"object","properties":{"constantBuffers":{"type":"boolean"},"slot":{"type":"string"},"enabled":{"type":"boolean"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"installed":{"type":"boolean"},"slots":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"enabled":{"type":"boolean"},"calls":{"type":"integer"}},"additionalProperties":false,"required":["name","enabled","calls"]}}},"additionalProperties":false,"required":["installed","slots"]})json" },
    { "ViewerGpuStateCandidates", &MakeRegisteredNativeCommand<ViewerGpuStateCandidatesCommand>, false,
      R"json({"type":"object","properties":{"limit":{"type":"integer","minimum":1,"maximum":16}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"scored":{"type":"boolean"},"referenceValid":{"type":"boolean"},"candidates":{"type":"array","items":{"type":"object","properties":{"buffer":{"type":"string"},"byteOffset":{"type":"integer"},"byteWidth":{"type":"integer"},"variant":{"type":"string"},"boundBy":{"type":"string"},"bindSlot":{"type":"integer"},"maxPixelError":{"type":"number"},"meanPixelError":{"type":"number"},"changesWhileMoving":{"type":"integer"},"changesWhileStill":{"type":"integer"}},"additionalProperties":false,"required":["buffer","byteOffset","variant","maxPixelError"]}}},"additionalProperties":false,"required":["scored","referenceValid","candidates"]})json" },
    { "ViewerPatchProfile", &MakeRegisteredNativeCommand<ViewerPatchProfileCommand>, false,
      R"json({"type":"object","properties":{"pin":{"type":"boolean"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"path":{"type":"string"},"pinned":{"type":"boolean"},"pinnedSummary":{"type":"string"},"currentSummary":{"type":"string"},"hostPath":{"type":"string"},"hostVersion":{"type":"string"},"hostSha256":{"type":"string"},"targets":{"type":"integer","minimum":0},"verifies":{"type":"boolean"},"verifyError":{"type":"string"}},"additionalProperties":false,"required":["path","pinned","verifies"]})json" },
    { "ViewerNavLog", &MakeRegisteredNativeCommand<ViewerNavLogCommand>, false,
      R"json({"type":"object","properties":{"enable":{"type":"boolean"},"intervalMs":{"type":"integer","minimum":0,"maximum":5000},"sampler":{"type":"boolean"}},"additionalProperties":false,"required":["enable"]})json",
      R"json({"type":"object","properties":{"running":{"type":"boolean"},"intervalMs":{"type":"integer","minimum":0},"viewerRows":{"type":"integer","minimum":0},"archicadRows":{"type":"integer","minimum":0},"archicadFails":{"type":"integer","minimum":0},"maxArchicadGapMs":{"type":"integer","minimum":0},"writeFailures":{"type":"integer","minimum":0},"droppedRows":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["running","intervalMs","viewerRows","archicadRows","archicadFails","maxArchicadGapMs"]})json" },
    { "NavLogMark", &MakeRegisteredNativeCommand<NavLogMarkCommand>, false,
      R"json({"type":"object","properties":{"label":{"type":"string"}},"additionalProperties":false,"required":["label"]})json",
      R"json({"type":"object","properties":{"marked":{"type":"boolean"}},"additionalProperties":false,"required":["marked"]})json" },
    { "CameraSyncModeState", &MakeRegisteredNativeCommand<CameraSyncModeStateCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"mode":{"type":"string"},"intervalMs":{"type":"integer","minimum":10,"maximum":1000},"experimentsBlocked":{"type":"boolean"},"experimentsBlockedWhy":{"type":"string"},"predictionScale":{"type":"number"},"hideOnNav":{"type":"boolean"},"breadcrumbPath":{"type":"string"},"safeModePath":{"type":"string"},"wakeInstalled":{"type":"boolean"},"wakeWheelEvents":{"type":"integer"},"wakeDragEvents":{"type":"integer"},"wakeKeyEvents":{"type":"integer"},"pollsPosted":{"type":"integer"},"pollsCoalesced":{"type":"integer"},"presentHookInstalled":{"type":"boolean"},"presentCalls":{"type":"integer"},"present1Calls":{"type":"integer"},"presentResizeCalls":{"type":"integer"},"busiestFrameCount":{"type":"integer"},"medianFrameUs":{"type":"integer"},"p95FrameUs":{"type":"integer"},"markerEnabled":{"type":"boolean"},"markerTargetChosen":{"type":"boolean"},"markerDraws":{"type":"integer"},"markerFailures":{"type":"integer"},"markerLastError":{"type":"string"},"compositeEnabled":{"type":"boolean"},"compositeReady":{"type":"boolean"},"compositeBlits":{"type":"integer"},"compositeFramesConsumed":{"type":"integer"},"compositeReprojections":{"type":"integer"},"compositeFailures":{"type":"integer"},"compositeBackBufferFormat":{"type":"integer"},"compositeWidth":{"type":"integer"},"compositeHeight":{"type":"integer"},"compositeLastError":{"type":"string"},"gpuState":{"type":"boolean"},"gpuStateInstalled":{"type":"boolean"},"gpuStateWanted":{"type":"boolean"},"gpuStatePinned":{"type":"boolean"},"gpuStateContextFound":{"type":"boolean"},"gpuStateContextChosen":{"type":"boolean"},"gpuStateProof":{"type":"string"},"gpuStateCallSpanMs":{"type":"integer"},"gpuStateSlotsStillPatched":{"type":"integer"},"gpuStateRepairs":{"type":"integer"},"gpuStateCalls":{"type":"integer"},"gpuStateOtherContextCalls":{"type":"integer"},"gpuStateEventsDropped":{"type":"integer"},"gpuStateLastError":{"type":"string"},"gpuFrames":{"type":"integer"},"gpuFramesDropped":{"type":"integer"},"gpuViewportWidth":{"type":"number"},"gpuViewportHeight":{"type":"number"},"gpuViewportX":{"type":"number"},"gpuViewportY":{"type":"number"},"gpuLargestWidth":{"type":"number"},"gpuLargestHeight":{"type":"number"},"gpuDistinctViewports":{"type":"integer"},"gpuReferenceValid":{"type":"boolean"},"gpuBuffersTracked":{"type":"integer"},"gpuBuffersDropped":{"type":"integer"},"gpuWritesCaptured":{"type":"integer"},"gpuMapsSeen":{"type":"integer"},"gpuMapsNotConstantBuffer":{"type":"integer"},"gpuMapsTooLarge":{"type":"integer"},"gpuMapsNoSlot":{"type":"integer"},"gpuLargestConstantBytes":{"type":"integer"},"gpuBufferCacheFull":{"type":"boolean"},"gpuRegionsScored":{"type":"integer"},"gpuBestMaxPixelError":{"type":"number"},"gpuBestByteOffset":{"type":"integer"},"gpuBestVariant":{"type":"integer"},"patchProfilePath":{"type":"string"},"patchProfilePinned":{"type":"boolean"}},"additionalProperties":false,"required":["mode","intervalMs","experimentsBlocked","experimentsBlockedWhy"]})json" },
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
