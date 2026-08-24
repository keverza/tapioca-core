#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/ArchVizCommands.hpp"
#include "NativeCommands/ArchVizCaptureParams.hpp"   // ReadCaptureCamera, ReadCaptureOverlays
#include "NativeCommands/CommandRegistration.hpp"

#include "ArchViz/ArchVizPanel.hpp"
#include "ArchViz/D3D12FeasibilityProbe.hpp"
#include "ArchViz/DiligentFxLink.hpp"
#include "ArchViz/DiligentProbe.hpp"
#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/SelectionBridge.hpp"
#include "ArchViz/ViewportOverlayWindow.hpp"
#include "Python/MainThreadGate.hpp"

namespace geomsrv {

namespace {

namespace av = archviz;

class StartDiligentCaptureCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "StartDiligentCapture";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Int32 width = 0, height = 0;
        GS::UniString quality;
        params.Get ("width", width);
        params.Get ("height", height);
        params.Get ("renderQuality", quality);
        uint64_t id = 0;
        std::string error;
        if (!av::DiligentViewport::Get ().StartCapture (uint32_t (width), uint32_t (height), ReadCaptureCamera (params),
                                                        quality == "realistic" ? 1 : 0,
                                                        ReadCaptureOverlays (params), id, error))
            return NativeCommandResult::Failure (GS::UniString (error.c_str (), CC_UTF8));
        GS::ObjectState os;
        os.Add ("id", static_cast<GS::Int64> (id));
        os.Add ("status", "running");
        return os;
    }
};

class DiligentCaptureStateCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "DiligentCaptureState";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Int64 id = 0;
        params.Get ("id", id);
        const av::DiligentCaptureStats stats = av::DiligentViewport::Get ().CaptureStats ();
        if (stats.id != static_cast<uint64_t> (id))
            return NativeCommandResult::Failure ("the Diligent capture id is unknown or has expired");
        GS::ObjectState os;
        os.Add ("id", static_cast<GS::Int64> (stats.id));
        os.Add ("status", GS::UniString (stats.status.c_str (), CC_UTF8));
        os.Add ("stage", GS::UniString (stats.stage.c_str (), CC_UTF8));
        os.Add ("width", static_cast<GS::Int32> (stats.width));
        os.Add ("height", static_cast<GS::Int32> (stats.height));
        os.Add ("bytes", static_cast<GS::Int64> (stats.bytes));
        os.Add ("url", GS::UniString (stats.url.c_str (), CC_UTF8));
        os.Add ("failureMessage", GS::UniString (stats.failureMessage.c_str (), CC_UTF8));
        return os;
    }
};

class CancelDiligentCaptureCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "CancelDiligentCapture";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Int64 id = 0;
        params.Get ("id", id);
        GS::ObjectState os;
        os.Add ("cancelled", av::DiligentViewport::Get ().CancelCapture (static_cast<uint64_t> (id)));
        return os;
    }
};

class ProbeDiligentDeviceCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "ProbeDiligentDevice";
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        GS::UniString error;
        const bool posted = evp::MainThreadGate::Get ().Post ([] () { ArchVizPanel::OpenDiligentProbe (); }, error);
        if (!posted)
            return NativeCommandResult::Failure (EVP_FAIL (error, "posting Diligent Probe 1c to the palette"));

        GS::ObjectState os;
        os.Add ("posted", true);
        return os;
    }
};

class DiligentProbeStateCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "DiligentProbeState";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        const auto stats = av::DiligentProbe::Get ().Stats ();
        GS::ObjectState os;
        os.Add ("attempted", stats.attempted);
        os.Add ("running", stats.running);
        os.Add ("succeeded", stats.succeeded);
        os.Add ("failureMessage", GS::UniString (stats.error.c_str (), CC_UTF8));
        return os;
    }
};

class StartD3D12FeasibilityProbeCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "StartD3D12FeasibilityProbe";
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        bool confirm = false;
        params.Get ("confirm", confirm);
        std::string error;
        bool started = false;
        if (!confirm) {
            error = "confirm=true is required for the in-process D3D12 feasibility probe";
        }
        else {
            started = ArchVizPanel::OpenD3D12FeasibilityProbe (error);
        }

        GS::ObjectState os;
        os.Add ("started", started);
        os.Add ("error", GS::UniString (error.c_str (), CC_UTF8));
        return os;
    }
};

class D3D12FeasibilityProbeStateCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "D3D12FeasibilityProbeState";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        const auto stats = av::D3D12FeasibilityProbe::Get ().Stats ();
        GS::ObjectState os;
        os.Add ("attempted", stats.attempted);
        os.Add ("running", stats.running);
        os.Add ("completed", stats.completed);
        os.Add ("cancelled", stats.cancelled);
        os.Add ("cleanTeardown", stats.cleanTeardown);
        os.Add ("stage", GS::UniString (stats.stage.c_str (), CC_UTF8));
        os.Add ("failureMessage", GS::UniString (stats.error.c_str (), CC_UTF8));
        os.Add ("deviceAttempted", stats.deviceAttempted);
        os.Add ("deviceSucceeded", stats.deviceSucceeded);
        os.Add ("deviceFailure", GS::UniString (stats.deviceError.c_str (), CC_UTF8));
        os.Add ("adapter", GS::UniString (stats.adapter.c_str (), CC_UTF8));
        os.Add ("hardwarePreflightSucceeded", stats.hardwarePreflightSucceeded);
        os.Add ("hardwareCreateResult", (GS::Int32) stats.hardwareCreateResult);
        os.Add ("hardwareFeatureLevel", (GS::Int32) stats.hardwareFeatureLevel);
        os.Add ("d3d12Runtime", GS::UniString (stats.d3d12Runtime.c_str (), CC_UTF8));
        os.Add ("childAttempted", stats.childAttempted);
        os.Add ("childSucceeded", stats.childSucceeded);
        os.Add ("childPresents", (GS::Int32) stats.childPresents);
        os.Add ("childLastPresentResult", (GS::Int32) stats.childLastPresentResult);
        os.Add ("childFailure", GS::UniString (stats.childError.c_str (), CC_UTF8));
        os.Add ("overlayAttempted", stats.overlayAttempted);
        os.Add ("overlaySucceeded", stats.overlaySucceeded);
        os.Add ("overlayPresents", (GS::Int32) stats.overlayPresents);
        os.Add ("overlayLastPresentResult", (GS::Int32) stats.overlayLastPresentResult);
        os.Add ("overlayFailure", GS::UniString (stats.overlayError.c_str (), CC_UTF8));
        os.Add ("rayTracingFeature", (GS::Int32) stats.rayTracingFeature);
        os.Add ("rayTracingCaps", (GS::Int32) stats.rayTracingCaps);
        os.Add ("rayTracingStandalone", stats.rayTracingStandalone);
        os.Add ("rayTracingInline", stats.rayTracingInline);
        os.Add ("rayTracingIndirect", stats.rayTracingIndirect);
        os.Add ("maxRecursionDepth", (GS::Int32) stats.maxRecursionDepth);
        os.Add ("maxRayGenThreads", (GS::Int32) stats.maxRayGenThreads);
        return os;
    }
};

class StopD3D12FeasibilityProbeCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "StopD3D12FeasibilityProbe";
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        ArchVizPanel::CloseD3D12FeasibilityProbe ();
        GS::ObjectState os;
        os.Add ("stopped", true);
        return os;
    }
};

class OpenDiligentViewportCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "OpenDiligentViewport";
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        GS::UniString error;
        const bool posted = evp::MainThreadGate::Get ().Post ([] () { ArchVizPanel::OpenDiligentViewport (); }, error);
        if (!posted)
            return NativeCommandResult::Failure (
                EVP_FAIL (error, "posting the Diligent viewport smoke test to the palette"));

        GS::ObjectState os;
        os.Add ("posted", true);
        return os;
    }
};

// Shut the viewport down from a script, without hunting for the palette's close
// box.
//
// ⚠️ IT IS `Post`, SO IT RETURNS BEFORE THE RENDER THREAD HAS JOINED. CloseViewer
// joins that thread, which takes up to a frame, and Invoke would hold the gate for
// it. Poll `DiligentViewportState.running` to know when the release has finished;
// reopening before it does is what re-creates the E_ACCESSDENIED the teardown
// exists to avoid.
class CloseDiligentViewportCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "CloseDiligentViewport";
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        GS::UniString error;
        const bool posted = evp::MainThreadGate::Get ().Post ([] () { ArchVizPanel::CloseViewer (); }, error);
        if (!posted)
            return NativeCommandResult::Failure (
                EVP_FAIL (error, "posting the Diligent viewport close to the palette"));

        GS::ObjectState os;
        os.Add ("posted", true);
        return os;
    }
};

class DiligentViewportStateCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "DiligentViewportState";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        const auto stats = av::DiligentViewport::Get ().Stats ();
        GS::ObjectState os;
        os.Add ("running", stats.running);
        os.Add ("initialized", stats.initialized);
        // ⚠️ ADDING A FIELD TO THE SCHEMA WITHOUT ADDING IT HERE FAILS *EVERY*
        // CALL, AND THE MESSAGE NAMES THE FIELD RATHER THAN THE COMMAND. That
        // cost the whole first overlay run (2026-08-11): `overlay` was in
        // `required` and not in the response, so every DiligentViewportState
        // returned SchemaValidationFailed, the probe read `{}` for the running
        // viewport and reported "THE RENDERER NEVER STARTED" while it was
        // rendering 11,118 frames. The schema and this block are ONE EDIT.
        os.Add ("overlay", stats.overlay);
        os.Add ("failed", stats.failed);
        os.Add ("failureMessage", GS::UniString (stats.error.c_str (), CC_UTF8));
        os.Add ("frames", (GS::Int64) stats.frames);
        os.Add ("fps", stats.fps);
        os.Add ("width", (GS::Int32) stats.width);
        os.Add ("height", (GS::Int32) stats.height);
        os.Add ("resizes", (GS::Int32) stats.resizes);
        // PLAT-RE22: the clear A/B and the device identity. These travel with
        // the state rather than living only in archviz.log because the smoke
        // command has to decide PASS/FAIL without the user reading a log.
        os.Add ("clearChecked", stats.clearChecked);
        os.Add ("diligentClearMatched", stats.diligentClearMatched);
        os.Add ("nativeClearMatched", stats.nativeClearMatched);
        os.Add ("diligentClearReport", GS::UniString (stats.diligentClearReport.c_str (), CC_UTF8));
        os.Add ("nativeClearReport", GS::UniString (stats.nativeClearReport.c_str (), CC_UTF8));
        os.Add ("adapter", GS::UniString (stats.adapter.c_str (), CC_UTF8));
        os.Add ("featureLevel", (GS::Int32) stats.featureLevel);
        os.Add ("presentCount", (GS::Int64) stats.presentCount);
        // PLAT-RE99: the afterimage evidence the camera-desync number cannot carry.
        os.Add ("stalePresents", (GS::Int64) stats.stalePresents);
        os.Add ("presentFailures", (GS::Int64) stats.presentFailures);
        os.Add ("lastPresentResult", (GS::Int64) stats.lastPresentResult);
        os.Add ("frameLatency", (GS::Int32) stats.frameLatency);
        os.Add ("deviceRemovedReason", (GS::Int64) stats.deviceRemovedReason);
        os.Add ("sceneReady", stats.sceneReady);
        os.Add ("sceneElements", (GS::Int64) stats.sceneElements);
        os.Add ("sceneTriangles", (GS::Int64) stats.sceneTriangles);
        os.Add ("sceneVertices", (GS::Int64) stats.sceneVertices);
        os.Add ("sceneGpuBytes", (GS::Int64) stats.sceneGpuBytes);
        os.Add ("scenePending", (GS::Int64) stats.scenePending);
        os.Add ("sceneMaterials", (GS::Int64) stats.sceneMaterials);
        os.Add ("materialMisses", (GS::Int64) stats.materialMisses);
        os.Add ("transparentRanges", (GS::Int64) stats.transparentRanges);
        os.Add ("sunApplied", stats.sunApplied);
        os.Add ("sunBelowHorizon", stats.sunBelowHorizon);
        os.Add ("sunX", stats.sun[0]);
        os.Add ("sunY", stats.sun[1]);
        os.Add ("sunZ", stats.sun[2]);
        os.Add ("ambient", stats.ambient);
        os.Add ("sunOverridden", stats.sunOverridden);
        os.Add ("sunAzimuthDegrees", stats.sunAzimuthDegrees);
        os.Add ("sunBearingDegrees", stats.sunBearingDegrees);
        os.Add ("northDegrees", stats.northDegrees);
        os.Add ("sunAltitudeDegrees", stats.sunAltitudeDegrees);
        // ⚠️ THE SUN'S PROVENANCE. `sunAzimuth/AltitudeDegrees` above are
        // Archicad's STORED angles -- what its own 3D window shades with, which
        // is the whole requirement. `computedAzimuth/AltitudeDegrees` is what
        // this place and date would imply instead; the two differ whenever the
        // sun was typed into the Sun dialog rather than computed from a date,
        // and preferring the computed pair is the bug this reports around.
        os.Add ("latitudeDegrees", stats.latitudeDegrees);
        os.Add ("longitudeDegrees", stats.longitudeDegrees);
        os.Add ("siteAltitudeMetres", stats.siteAltitudeMetres);
        os.Add ("sunYear", (GS::Int32) stats.year);
        os.Add ("sunMonth", (GS::Int32) stats.month);
        os.Add ("sunDay", (GS::Int32) stats.day);
        os.Add ("sunHour", (GS::Int32) stats.hour);
        os.Add ("sunMinute", (GS::Int32) stats.minute);
        os.Add ("summerTime", stats.summerTime);
        os.Add ("haveComputedSun", stats.haveComputedSun);
        os.Add ("computedAzimuthDegrees", stats.computedAzimuthDegrees);
        os.Add ("computedAltitudeDegrees", stats.computedAltitudeDegrees);
        os.Add ("shadowReady", stats.shadowReady);
        os.Add ("shadowFitted", stats.shadowFitted);
        os.Add ("shadowResolution", (GS::Int64) stats.shadowResolution);
        // ⚠️ `environmentAverage` IS THE ONE FIELD THAT SEPARATES A BLACK SKY
        // FROM AN UNBOUND ONE -- both render identically. `environmentError` is
        // the only surviving evidence of a deferred load's failure, since
        // SetDiligentEnvironmentMap returns before the load is attempted.
        os.Add ("environmentLoaded", stats.environmentLoaded);
        os.Add ("environmentActive", stats.environmentActive);
        os.Add ("environmentMipLevels", (GS::Int64) stats.environmentMipLevels);
        os.Add ("environmentAverageR", (double) stats.environmentAverage[0]);
        os.Add ("environmentAverageG", (double) stats.environmentAverage[1]);
        os.Add ("environmentAverageB", (double) stats.environmentAverage[2]);
        os.Add ("environmentPath", GS::UniString (stats.environmentPath.c_str ()));
        os.Add ("environmentError", GS::UniString (stats.environmentError.c_str ()));
        // ---- RE51.B6 -------------------------------------------------------
        // ⚠️ A PREFILTERED MIP CHAIN AND A BOX-FILTERED ONE LOOK THE SAME until
        // you judge a mirror, and "does this reflection look right" is a
        // judgement rather than an observation. These four say which one the
        // renderer actually has, and name the reason when it is the fallback.
        os.Add ("environmentPrefiltered", stats.environmentPrefiltered);
        os.Add ("environmentPrefilteredMips", (GS::Int64) stats.environmentPrefilteredMips);
        os.Add ("environmentPrefilterMs", stats.environmentPrefilterMs);
        os.Add ("environmentPrefilterError", GS::UniString (stats.environmentPrefilterError.c_str (), CC_UTF8));
        // ---- RE51.B9 -------------------------------------------------------
        // ⚠️ `autoExposure` IS WHAT THE ESTIMATE WOULD CHOOSE, `appliedExposure`
        // IS WHAT RENDERED. They differ whenever the auto path is switched off,
        // which is its shipped state -- and reporting both is exactly how one
        // live run settles whether middle grey is the right target here.
        os.Add ("autoExposureEnabled", stats.autoExposureEnabled);
        os.Add ("autoExposure", (double) stats.autoExposure);
        os.Add ("appliedExposure", (double) stats.appliedExposure);
        // ⚠️ THE MANUAL VALUE TOO, so the two-defaults drift check has
        // somewhere to look while the auto path is driving.
        os.Add ("fixedExposure", (double) stats.fixedExposure);
        os.Add ("sceneLuminance", (double) stats.sceneLuminance);
        os.Add ("meanAlbedo", (double) stats.meanAlbedo);
        // RE51.C3. ⚠️ THE RADIUS IS DERIVED unless the HUD overrides it, so it
        // is invisible from outside -- and it is the leading suspect for the
        // live report "AO darkens whole scene, but soft contact shadow is not
        // visible".
        os.Add ("aoRadiusMetres", (double) stats.aoRadiusMetres);
        os.Add ("whiteBalanceR", (double) stats.whiteBalanceGains[0]);
        os.Add ("whiteBalanceG", (double) stats.whiteBalanceGains[1]);
        os.Add ("whiteBalanceB", (double) stats.whiteBalanceGains[2]);
        // ---- RE51.B2 -------------------------------------------------------
        // ⚠️ ZERO NAMED WITH A NON-EMPTY POOL IS THE ONE ALARMING READING, and
        // it is what a wrong element-index join looks like: every body's
        // building material read as some other element's, so no surface reaches
        // the dominance bar. A LOW count is ordinary -- both classifiers refuse
        // by design. The breakdown is what separates the two.
        os.Add ("substanceNamed", (GS::Int64) stats.substanceNamed);
        {
            // ⚠️ THE ORDER IS Substance's OWN ENUM ORDER (BuildingMaterialSignal.hpp)
            // and index 0 is the REFUSALS. A reader that renames or reorders these
            // without moving the enum silently mislabels every count.
            static const char* kSubstanceNames[7] = { "unknown", "earth", "concrete", "metal",
                                                      "plastic", "glass", "wood" };
            GS::ObjectState counts;
            for (int i = 0; i < 7; ++i)
                counts.Add (kSubstanceNames[i], (GS::Int64) stats.substanceCounts[i]);
            os.Add ("substanceCounts", counts);
        }
        os.Add ("shadowTexelMetres", stats.shadowTexelMetres);
        os.Add ("cameraEyeX", stats.cameraEye[0]);
        os.Add ("cameraEyeY", stats.cameraEye[1]);
        os.Add ("cameraEyeZ", stats.cameraEye[2]);
        os.Add ("cameraTargetX", stats.cameraTarget[0]);
        os.Add ("cameraTargetY", stats.cameraTarget[1]);
        os.Add ("cameraTargetZ", stats.cameraTarget[2]);
        os.Add ("cameraFovDegreesVertical", stats.cameraFovDegreesVertical);
        os.Add ("cameraSyncs", (GS::Int64) stats.cameraSyncs);
        os.Add ("cameraSource", GS::UniString (stats.cameraSource.c_str (), CC_UTF8));
        // PLAT-RE34. ⚠️ `pickSeq` IS THE SIGNAL a probe watches, not
        // `pickedGuid`: clicking the same element twice leaves the guid
        // unchanged. `pickedGuid` is the guid THE MODEL gave us, which for a
        // stair, railing, curtain wall or column is a SUB-PART -- the bridge
        // resolves its owner before selecting, and reporting the raw one here is
        // what separates "the pick missed" from "the owner walk failed".
        os.Add ("pickAvailable", stats.pickAvailable);
        os.Add ("pickSeq", (GS::Int64) stats.pickSeq);
        os.Add ("pickedGuid", GS::UniString (stats.pickedGuid.c_str (), CC_UTF8));
        os.Add ("selectedCount", (GS::Int64) stats.selectedCount);
        // PLAT-RE65. ⚠️ THREE FIELDS BECAUSE "I SEE NO ANCHORS" HAS THREE
        // CAUSES and they are one symptom on screen: the layer never started
        // (planAnchorLayerReady false, reason in archviz.log), it started and
        // was handed nothing (planAnchorVertices 0), or it is simply switched
        // off (planAnchors false).
        os.Add ("planAnchors", stats.planAnchors);
        os.Add ("planAnchorLayerReady", stats.planAnchorLayerReady);
        os.Add ("planAnchorVertices", (GS::Int64) stats.planAnchorVertices);
        os.Add ("planAnchorWidthPixels", (double) stats.planAnchorWidthPixels);
        os.Add ("selectionBridgeMode", (GS::Int32) av::selectionbridge::Mode ());
        os.Add ("debugView", (GS::Int32) av::DiligentViewport::Get ().DebugView ());
        os.Add ("renderMode", (GS::Int32) av::DiligentViewport::Get ().RenderMode ());
        os.Add ("callout", av::DiligentViewport::Get ().Callout ());
        return os;
    }
};

// The shader's debug view, switchable while the viewport runs.
//
// ⚠️ IT EXISTS BECAUSE "FLAT, EVENLY LIT" HAS FOUR DIFFERENT CAUSES that look
// identical on a shaded cube: the sun never reaching the pixel shader, the
// normals arriving wrong, the constant buffer being read at the wrong offset,
// or the lighting being correct and merely hard to judge. Rendering the
// shader's own inputs as colour separates all four in one look.
class SetDiligentDebugViewCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "SetDiligentDebugView";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Int32 view = 0;
        params.Get ("view", view);
        av::DiligentViewport::Get ().SetDebugView ((int) view);
        GS::ObjectState os;
        os.Add ("view", view);
        return os;
    }
};

// The HDR sky (PLAT-RE51).
//
// ⚠️ THE PATH COMES FROM PYTHON, NOT FROM A FILE DIALOG. An ImGui file button
// would open a Win32 modal on the RENDER thread inside Archicad's process, which
// blocks the frame loop for human time -- HANDOFF-RenderingPanels caveat #4.
// Python picks the file; this only carries the string.
//
// ⚠️ IT REPORTS `requested`, NOT `loaded`. The load happens on the render thread
// two hops from here, so this command CANNOT know whether the file was readable.
// Read `DiligentViewportState` afterwards for the answer -- reporting success
// here would be a lie that is convenient for exactly one caller.
class SetDiligentEnvironmentMapCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "SetDiligentEnvironmentMap";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString path;
        params.Get ("path", path);

        bool enabled = true;
        if (!params.Get ("enabled", enabled))
            enabled = true;
        double intensity = 1.0;
        if (!params.Get ("intensity", intensity))
            intensity = 1.0;
        double rotationDegrees = 0.0;
        if (!params.Get ("rotationDegrees", rotationDegrees))
            rotationDegrees = 0.0;

        av::DiligentViewport& viewport = av::DiligentViewport::Get ();
        viewport.SetEnvironmentSettings (enabled, (float) intensity, (float) rotationDegrees);
        // ⚠️ THE SETTINGS GO FIRST. The load is what makes the sky visible, and
        // pushing it before the intensity it should be shown at would render one
        // frame at the previous strength -- which on an A/B is the frame the user
        // is looking at.
        viewport.SetEnvironmentMap (std::string (path.ToCStr ().Get ()));

        GS::ObjectState os;
        os.Add ("requested", path);
        os.Add ("enabled", enabled);
        os.Add ("intensity", intensity);
        os.Add ("rotationDegrees", rotationDegrees);
        return os;
    }
};

// ---- the overlay path: Archicad's camera, read and pushed -------------------
//
// ⚠️ TWO COMMANDS RATHER THAN ONE "SyncDiligentCamera". A single command would
// read Archicad and push the result in one main-thread hop, which is cheaper --
// and it would make the two halves untestable separately. The whole question the
// overlay sync test exists to answer is WHERE a mismatch comes from: Archicad
// reporting something unexpected, or the viewport failing to adopt it. Fusing
// them puts that behind one boolean.

// ---------------------------------------------------------------------------
// Tapioca.DiligentFxState {} -> { linked, report }
//
// Is DiligentFX actually reachable from the add-on? See ArchViz/DiligentFxLink
// for why this is a question worth a verb: the library has been in the link line
// and building for some time with no TU referencing it, which proves nothing.
// A response at all means the headers resolved and the symbols linked, because
// the command cannot exist in a binary where they did not.
// ---------------------------------------------------------------------------
class DiligentFxStateCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "DiligentFxState";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        std::string report;
        const bool linked = av::DiligentFxLinked (report);
        GS::ObjectState os;
        os.Add ("linked", linked);
        os.Add ("report", GS::UniString (report.c_str (), CC_UTF8));
        return os;
    }
};

// What is armed right now, and whether experiments are available at all.
//
// ⚠️ `experimentsBlocked` IS THE ANSWER TO "WHY DID MY MODE REFUSE TO ARM". The
// experiment guard latches for a whole session after a crash (or when the user
// has dropped a SAFE_MODE file), and without this field the refusal reads like a
// bug in the mode being requested. The matrix probe also reads `mode` here so it
// can restore what it found in its `finally` block.
// ⚠️ THE ONE LEVER PREDICTION CANNOT SUBSTITUTE FOR. Everything the camera work
// has done moves WHERE the overlay is drawn; none of it affects WHEN a finished
// frame is composited. DXGI queues up to three by default, and a queued frame is
// a lingering afterimage that the desync measurement cannot see -- it timestamps
// submission, not display.
//
// ⚠️ THE BASELINE ARM IS 3, NOT 0. There is no call that un-sets a frame
// latency, so "0 = restore the default" was a fiction: it left the previously
// applied value alone, and since the viewport defaults to 1 BOTH arms of the A/B
// ran at 1. Asking for 3 reproduces DXGI's default explicitly, which is a real
// baseline instead of an imagined one.
class SetOverlayFrameLatencyCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "SetOverlayFrameLatency";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Int32 frames = 1;
        params.Get ("frames", frames);
        // The render thread applies it; this only publishes the request, so it
        // is safe from any thread and takes effect on the next frame.
        av::DiligentViewport::Get ().SetFrameLatency ((uint32_t) frames);

        GS::ObjectState os;
        os.Add ("frames", frames);
        return os;
    }
};

// ⚠️ THE ONLY WAY TO TELL THE USER ANYTHING WHILE THEY NAVIGATE (PLAT-RE111).
// Archicad's DG palette does not repaint during a navigation drag, so the
// palette status line `evp.ui.progress` writes to is frozen for the whole
// gesture -- which is exactly the interval a measurement run needs to talk
// during, and the log is read long afterwards. The overlay renders every frame
// regardless, so its HUD is the one surface that can carry a live instruction.
class SetOverlayInstructionCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "SetOverlayInstruction";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString text;
        params.Get ("text", text);
        // Negative means "no countdown", which is different from zero: zero
        // would clear the banner on the next frame.
        double seconds = -1.0;
        params.Get ("seconds", seconds);
        av::DiligentViewport::Get ().SetInstruction (std::string (text.ToCStr (0, MaxUSize, CC_UTF8).Get ()), seconds);

        GS::ObjectState os;
        os.Add ("shown", !text.IsEmpty ());
        return os;
    }
};

// ---- the overlay (PLAT-RE37) ------------------------------------------------
//
// ⚠️ OPEN IT WITH ARCHICAD'S 3D WINDOW IN FRONT. The overlay covers the
// FRONTMOST document canvas: there is no DevKit call that hands back the 3D
// view's HWND, so an overlay opened over the floor plan lands on the floor plan.
// `DiligentViewportState.overlay` says whether it came up as an overlay at all,
// and the palette's status line names the window class it landed on.
class OpenDiligentOverlayCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "OpenDiligentOverlay";
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        // How the overlay window is attached. ⚠️ A RUNTIME CHOICE ON PURPOSE --
        // see ViewportOverlayWindow.hpp's OverlayAttach. 0 = popup (visible but
        // covers Archicad's callouts), 1 = child of the view window with
        // WS_EX_LAYERED, 2 = child without it. The two child modes differ by one
        // style bit so a single run can bisect which one renders AND stays
        // click-through.
        //
        // ⚠️ 0 (POPUP), AND THE CHILD MODES DO NOT COMPOSITE -- MEASURED, NOT
        // ASSUMED. This defaulted to 2 for one build, to stop the overlay
        // covering Archicad's palettes and callouts, and NOTHING APPEARED on
        // either path. The renderer was fine: 10,080 frames, composition swap
        // chain created, DirectComposition target and visual built, Commit
        // succeeded, no error logged anywhere. The child window is created at
        // 0,0 in the canvas's client area, which is correct. It simply is not
        // composited.
        //
        // That is the experiment the two child modes existed to run, and the
        // answer is no. A composition swap chain reaches the screen through a
        // DirectComposition target bound to the window, and that path works for
        // the top-level popup only.
        //
        // ⚠️ SO "THE OVERLAY COVERS THE PALETTES" IS NOT FIXABLE BY A WINDOW
        // STYLE. An owned popup is always above its owner. The real answers are
        // clipping the popup's region to exclude the windows above it, or
        // PLAT-RE79, which removes the overlay window altogether. Do not set 2
        // as a default again without a probe that proves a frame is visible.
        GS::Int32 attach = 0;
        params.Get ("attach", attach);

        GS::UniString error;
        // ⚠️ BY VALUE. `Post` can run the job after this frame is gone
        // (CLAUDE.md), so the lambda must not reference `attach`.
        const bool posted =
            evp::MainThreadGate::Get ().Post ([attach] () { ArchVizPanel::OpenDiligentOverlay ((int) attach); }, error);
        if (!posted)
            return NativeCommandResult::Failure (EVP_FAIL (error, "posting the Diligent overlay open to the palette"));

        GS::ObjectState os;
        os.Add ("posted", true);
        return os;
    }
};

class CloseDiligentOverlayCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "CloseDiligentOverlay";
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        GS::UniString error;
        const bool posted = evp::MainThreadGate::Get ().Post ([] () { ArchVizPanel::CloseDiligentOverlay (); }, error);
        if (!posted)
            return NativeCommandResult::Failure (EVP_FAIL (error, "posting the Diligent overlay close to the palette"));

        GS::ObjectState os;
        os.Add ("posted", true);
        return os;
    }
};

// Where the overlay actually landed, and whether it is still following.
//
// ⚠️ IT IS A SEPARATE COMMAND FROM DiligentViewportState BECAUSE IT ANSWERS A
// DIFFERENT QUESTION. The viewport state says whether frames are being drawn;
// this says whether they are being drawn OVER THE RIGHT WINDOW. "The overlay is
// running and I cannot see it" has both answers in it, and they point in
// opposite directions.
class DiligentOverlayStateCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "DiligentOverlayState";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        const auto stats = av::viewportoverlay::Stats ();
        GS::ObjectState os;
        os.Add ("active", stats.active);
        os.Add ("width", (GS::Int32) stats.width);
        os.Add ("height", (GS::Int32) stats.height);
        os.Add ("left", (GS::Int32) stats.screenRect.left);
        os.Add ("top", (GS::Int32) stats.screenRect.top);
        os.Add ("targetClass", GS::UniString (stats.targetClass.c_str (), CC_UTF8));
        os.Add ("attach", (GS::Int32) stats.attach);
        os.Add ("how", GS::UniString (stats.how.c_str (), CC_UTF8));
        os.Add ("trackPolls", (GS::Int32) stats.trackPolls);
        os.Add ("trackMoves", (GS::Int32) stats.trackMoves);
        return os;
    }
};

// Shaded, wireframe, or both. ⚠️ WIREFRAME IS WHAT MAKES THE OVERLAY READABLE:
// a shaded viewer over Archicad's own 3D window simply hides it, so "do the two
// agree" -- the only question an overlay exists to answer -- stops being
// answerable at exactly the moment it is asked.
class SetDiligentRenderModeCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "SetDiligentRenderMode";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Int32 mode = 0;
        params.Get ("mode", mode);
        av::DiligentViewport::Get ().SetRenderMode ((int) mode);
        GS::ObjectState os;
        os.Add ("mode", mode);
        return os;
    }
};

// The mouse-following element callout.
class SetDiligentCalloutCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "SetDiligentCallout";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        bool enabled = false;
        params.Get ("enabled", enabled);
        av::DiligentViewport::Get ().SetCallout (enabled);
        GS::ObjectState os;
        os.Add ("enabled", enabled);
        return os;
    }
};

// The sun override, from a script. ⚠️ IT IS A MEASURING INSTRUMENT AND THE HUD'S
// COPY OF IT IS UNREACHABLE ON THE OVERLAY -- that surface is WS_EX_TRANSPARENT,
// so its sliders draw and cannot be touched (PLAT-RE55). The live question this
// exists for is PLAT-RE58: the viewer's shadows read as "the opposite time of
// day" from Archicad's, which is what a MIRRORED azimuth looks like, and one run
// that puts the stored angle and its mirror on screen in turn settles it -- where
// changing a sign in the source and rebuilding would only produce another guess.
//
// `azimuthDegrees` is the MODEL angle, CCW from +X, the same convention
// ExtractionEnvironment reads out of `sunAngXY`. It is NOT the compass bearing
// Archicad's Sun dialog shows; the two differ by project north and confusing
// them is the entire history of this corner of the code.
class SetDiligentSunCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "SetDiligentSun";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        bool enabled = false;
        double azimuth = 0.0;
        double altitude = 45.0;
        params.Get ("enabled", enabled);
        params.Get ("azimuthDegrees", azimuth);
        params.Get ("altitudeDegrees", altitude);
        av::DiligentViewport::Get ().SetSunOverride (enabled, (float) azimuth, (float) altitude);
        GS::ObjectState os;
        os.Add ("enabled", enabled);
        os.Add ("azimuthDegrees", azimuth);
        os.Add ("altitudeDegrees", altitude);
        return os;
    }
};

const NativeCommandRegistration kArchVizCommandRegistrations[] = {
    { "SetOverlayFrameLatency", &MakeRegisteredNativeCommand<SetOverlayFrameLatencyCommand>, false,
      R"json({"type":"object","properties":{"frames":{"type":"integer","minimum":1,"maximum":3}},"additionalProperties":false,"required":["frames"]})json",
      R"json({"type":"object","properties":{"frames":{"type":"integer","minimum":1,"maximum":3}},"additionalProperties":false,"required":["frames"]})json" },
    { "SetOverlayInstruction", &MakeRegisteredNativeCommand<SetOverlayInstructionCommand>, false,
      R"json({"type":"object","properties":{"text":{"type":"string"},"seconds":{"type":"number","minimum":-1,"maximum":600}},"additionalProperties":false,"required":["text"]})json",
      R"json({"type":"object","properties":{"shown":{"type":"boolean"}},"additionalProperties":false,"required":["shown"]})json" },
    { "DiligentFxState", &MakeRegisteredNativeCommand<DiligentFxStateCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"linked":{"type":"boolean"},"report":{"type":"string"}},"additionalProperties":false,"required":["linked","report"]})json" },
    { "ProbeDiligentDevice", &MakeRegisteredNativeCommand<ProbeDiligentDeviceCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"posted":{"type":"boolean"}},"additionalProperties":false,"required":["posted"]})json" },
    { "DiligentProbeState", &MakeRegisteredNativeCommand<DiligentProbeStateCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"attempted":{"type":"boolean"},"running":{"type":"boolean"},"succeeded":{"type":"boolean"},"failureMessage":{"type":"string"}},"additionalProperties":false,"required":["attempted","running","succeeded","failureMessage"]})json" },
    { "StartD3D12FeasibilityProbe", &MakeRegisteredNativeCommand<StartD3D12FeasibilityProbeCommand>, false,
      R"json({"type":"object","properties":{"confirm":{"type":"boolean"}},"additionalProperties":false,"required":["confirm"]})json",
      R"json({"type":"object","properties":{"started":{"type":"boolean"},"error":{"type":"string"}},"additionalProperties":false,"required":["started","error"]})json" },
    { "D3D12FeasibilityProbeState", &MakeRegisteredNativeCommand<D3D12FeasibilityProbeStateCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"attempted":{"type":"boolean"},"running":{"type":"boolean"},"completed":{"type":"boolean"},"cancelled":{"type":"boolean"},"cleanTeardown":{"type":"boolean"},"stage":{"type":"string"},"failureMessage":{"type":"string"},"deviceAttempted":{"type":"boolean"},"deviceSucceeded":{"type":"boolean"},"deviceFailure":{"type":"string"},"adapter":{"type":"string"},"hardwarePreflightSucceeded":{"type":"boolean"},"hardwareCreateResult":{"type":"integer"},"hardwareFeatureLevel":{"type":"integer","minimum":0},"d3d12Runtime":{"type":"string"},"childAttempted":{"type":"boolean"},"childSucceeded":{"type":"boolean"},"childPresents":{"type":"integer","minimum":0},"childLastPresentResult":{"type":"integer"},"childFailure":{"type":"string"},"overlayAttempted":{"type":"boolean"},"overlaySucceeded":{"type":"boolean"},"overlayPresents":{"type":"integer","minimum":0},"overlayLastPresentResult":{"type":"integer"},"overlayFailure":{"type":"string"},"rayTracingFeature":{"type":"integer","minimum":0},"rayTracingCaps":{"type":"integer","minimum":0},"rayTracingStandalone":{"type":"boolean"},"rayTracingInline":{"type":"boolean"},"rayTracingIndirect":{"type":"boolean"},"maxRecursionDepth":{"type":"integer","minimum":0},"maxRayGenThreads":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["attempted","running","completed","cancelled","cleanTeardown","stage","failureMessage","deviceAttempted","deviceSucceeded","deviceFailure","adapter","hardwarePreflightSucceeded","hardwareCreateResult","hardwareFeatureLevel","d3d12Runtime","childAttempted","childSucceeded","childPresents","childLastPresentResult","childFailure","overlayAttempted","overlaySucceeded","overlayPresents","overlayLastPresentResult","overlayFailure","rayTracingFeature","rayTracingCaps","rayTracingStandalone","rayTracingInline","rayTracingIndirect","maxRecursionDepth","maxRayGenThreads"]})json" },
    { "StopD3D12FeasibilityProbe", &MakeRegisteredNativeCommand<StopD3D12FeasibilityProbeCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"stopped":{"type":"boolean"}},"additionalProperties":false,"required":["stopped"]})json" },
    { "OpenDiligentViewport", &MakeRegisteredNativeCommand<OpenDiligentViewportCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"posted":{"type":"boolean"}},"additionalProperties":false,"required":["posted"]})json" },
    { "CloseDiligentViewport", &MakeRegisteredNativeCommand<CloseDiligentViewportCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"posted":{"type":"boolean"}},"additionalProperties":false,"required":["posted"]})json" },
    { "DiligentViewportState", &MakeRegisteredNativeCommand<DiligentViewportStateCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"running":{"type":"boolean"},"initialized":{"type":"boolean"},"failed":{"type":"boolean"},"failureMessage":{"type":"string"},"frames":{"type":"integer","minimum":0},"fps":{"type":"number","minimum":0},"width":{"type":"integer","minimum":0},"height":{"type":"integer","minimum":0},"resizes":{"type":"integer","minimum":0},"clearChecked":{"type":"boolean"},"diligentClearMatched":{"type":"boolean"},"nativeClearMatched":{"type":"boolean"},"diligentClearReport":{"type":"string"},"nativeClearReport":{"type":"string"},"adapter":{"type":"string"},"featureLevel":{"type":"integer","minimum":0},"presentCount":{"type":"integer","minimum":0},"stalePresents":{"type":"integer","minimum":0},"presentFailures":{"type":"integer","minimum":0},"lastPresentResult":{"type":"integer"},"frameLatency":{"type":"integer","minimum":0},"deviceRemovedReason":{"type":"integer","minimum":0},"sceneReady":{"type":"boolean"},"sceneElements":{"type":"integer","minimum":0},"sceneTriangles":{"type":"integer","minimum":0},"sceneVertices":{"type":"integer","minimum":0},"sceneGpuBytes":{"type":"integer","minimum":0},"scenePending":{"type":"integer","minimum":0},"sceneMaterials":{"type":"integer","minimum":0},"materialMisses":{"type":"integer","minimum":0},"transparentRanges":{"type":"integer","minimum":0},"sunApplied":{"type":"boolean"},"sunBelowHorizon":{"type":"boolean"},"sunX":{"type":"number"},"sunY":{"type":"number"},"sunZ":{"type":"number"},"ambient":{"type":"number","minimum":0,"maximum":1},"sunOverridden":{"type":"boolean"},"sunAzimuthDegrees":{"type":"number"},"sunBearingDegrees":{"type":"number"},"northDegrees":{"type":"number"},"sunAltitudeDegrees":{"type":"number"},"shadowReady":{"type":"boolean"},"shadowFitted":{"type":"boolean"},"shadowResolution":{"type":"integer","minimum":0},"shadowTexelMetres":{"type":"number","minimum":0},"cameraEyeX":{"type":"number"},"cameraEyeY":{"type":"number"},"cameraEyeZ":{"type":"number"},"cameraTargetX":{"type":"number"},"cameraTargetY":{"type":"number"},"cameraTargetZ":{"type":"number"},"cameraFovDegreesVertical":{"type":"number","minimum":0},"cameraSyncs":{"type":"integer","minimum":0},"cameraSource":{"type":"string"},"pickAvailable":{"type":"boolean"},"pickSeq":{"type":"integer","minimum":0},"pickedGuid":{"type":"string"},"selectedCount":{"type":"integer","minimum":0},"planAnchors":{"type":"boolean"},"planAnchorLayerReady":{"type":"boolean"},"planAnchorVertices":{"type":"integer","minimum":0},"planAnchorWidthPixels":{"type":"number","minimum":0},"selectionBridgeMode":{"type":"integer","minimum":0,"maximum":3},"overlay":{"type":"boolean"},"latitudeDegrees":{"type":"number"},"longitudeDegrees":{"type":"number"},"siteAltitudeMetres":{"type":"number"},"sunYear":{"type":"integer","minimum":0},"sunMonth":{"type":"integer","minimum":0},"sunDay":{"type":"integer","minimum":0},"sunHour":{"type":"integer","minimum":0},"sunMinute":{"type":"integer","minimum":0},"summerTime":{"type":"boolean"},"haveComputedSun":{"type":"boolean"},"computedAzimuthDegrees":{"type":"number"},"computedAltitudeDegrees":{"type":"number"},"renderMode":{"type":"integer","minimum":0,"maximum":2},"callout":{"type":"boolean"},"debugView":{"type":"integer","minimum":0,"maximum":13},"environmentLoaded":{"type":"boolean"},"environmentActive":{"type":"boolean"},"environmentMipLevels":{"type":"integer","minimum":0},"environmentAverageR":{"type":"number"},"environmentAverageG":{"type":"number"},"environmentAverageB":{"type":"number"},"environmentPath":{"type":"string"},"environmentError":{"type":"string"},"environmentPrefiltered":{"type":"boolean"},"environmentPrefilteredMips":{"type":"integer","minimum":0},"environmentPrefilterMs":{"type":"number","minimum":0},"environmentPrefilterError":{"type":"string"},"autoExposureEnabled":{"type":"boolean"},"autoExposure":{"type":"number","minimum":0},"appliedExposure":{"type":"number","minimum":0},"fixedExposure":{"type":"number","minimum":0},"sceneLuminance":{"type":"number","minimum":0},"meanAlbedo":{"type":"number","minimum":0},"aoRadiusMetres":{"type":"number","minimum":0},"whiteBalanceR":{"type":"number","minimum":0},"whiteBalanceG":{"type":"number","minimum":0},"whiteBalanceB":{"type":"number","minimum":0},"substanceNamed":{"type":"integer","minimum":0},"substanceCounts":{"type":"object","properties":{"unknown":{"type":"integer","minimum":0},"earth":{"type":"integer","minimum":0},"concrete":{"type":"integer","minimum":0},"metal":{"type":"integer","minimum":0},"plastic":{"type":"integer","minimum":0},"glass":{"type":"integer","minimum":0},"wood":{"type":"integer","minimum":0}},"additionalProperties":false}},"additionalProperties":false,"required":["overlay","latitudeDegrees","longitudeDegrees","siteAltitudeMetres","sunYear","sunMonth","sunDay","sunHour","sunMinute","summerTime","haveComputedSun","computedAzimuthDegrees","computedAltitudeDegrees","renderMode","callout","running","initialized","failed","failureMessage","frames","fps","width","height","resizes","clearChecked","diligentClearMatched","nativeClearMatched","diligentClearReport","nativeClearReport","adapter","featureLevel","presentCount","deviceRemovedReason","sceneReady","sceneElements","sceneTriangles","sceneVertices","sceneGpuBytes","scenePending","sceneMaterials","materialMisses","transparentRanges","sunApplied","sunBelowHorizon","sunX","sunY","sunZ","ambient","sunOverridden","sunAzimuthDegrees","sunBearingDegrees","northDegrees","sunAltitudeDegrees","shadowReady","shadowFitted","shadowResolution","shadowTexelMetres","cameraEyeX","cameraEyeY","cameraEyeZ","cameraTargetX","cameraTargetY","cameraTargetZ","cameraFovDegreesVertical","cameraSyncs","cameraSource","pickAvailable","pickSeq","pickedGuid","selectedCount","planAnchors","planAnchorLayerReady","planAnchorVertices","planAnchorWidthPixels","selectionBridgeMode","debugView"]})json" },
    { "StartDiligentCapture", &MakeRegisteredNativeCommand<StartDiligentCaptureCommand>, false,
      R"json({"type":"object","properties":{"width":{"type":"integer","minimum":16,"maximum":8192},"height":{"type":"integer","minimum":16,"maximum":8192},"renderQuality":{"type":"string","enum":["fast","realistic"]},"storySlices":{"type":"boolean"},"storySliceFill":{"type":"boolean"},"storySliceOccluded":{"type":"string","enum":["hidden","dashed","solid"]},"storySliceWidthPixels":{"type":"number","exclusiveMinimum":0,"maximum":32},"storySliceRgba":{"type":"integer"},"storySliceFillRgba":{"type":"integer"},"camera":{"type":"object","properties":{"valid":{"type":"boolean"},"source":{"type":"string"},"orthographic":{"type":"boolean"},"viewMoving":{"type":"boolean"},"eyeX":{"type":"number"},"eyeY":{"type":"number"},"eyeZ":{"type":"number"},"targetX":{"type":"number"},"targetY":{"type":"number"},"targetZ":{"type":"number"},"viewConeDegreesHorizontal":{"type":"number","exclusiveMinimum":1,"exclusiveMaximum":179}},"additionalProperties":false,"required":["valid","source","orthographic","viewMoving","eyeX","eyeY","eyeZ","targetX","targetY","targetZ","viewConeDegreesHorizontal"]}},"additionalProperties":false,"required":["width","height","renderQuality","camera"]})json", R"json({"type":"object","properties":{"id":{"type":"integer","minimum":1},"status":{"type":"string","const":"running"}},"additionalProperties":false,"required":["id","status"]})json" },
    { "DiligentCaptureState", &MakeRegisteredNativeCommand<DiligentCaptureStateCommand>, false,
      R"json({"type":"object","properties":{"id":{"type":"integer","minimum":1}},"additionalProperties":false,"required":["id"]})json",
      R"json({"type":"object","properties":{"id":{"type":"integer","minimum":1},"status":{"type":"string","enum":["running","completed","failed","cancelled"]},"stage":{"type":"string"},"width":{"type":"integer","minimum":16},"height":{"type":"integer","minimum":16},"bytes":{"type":"integer","minimum":0},"url":{"type":"string","minLength":1},"failureMessage":{"type":"string"}},"additionalProperties":false,"required":["id","status","stage","width","height","bytes","url","failureMessage"]})json" },
    { "CancelDiligentCapture", &MakeRegisteredNativeCommand<CancelDiligentCaptureCommand>, false,
      R"json({"type":"object","properties":{"id":{"type":"integer","minimum":1}},"additionalProperties":false,"required":["id"]})json",
      R"json({"type":"object","properties":{"cancelled":{"type":"boolean"}},"additionalProperties":false,"required":["cancelled"]})json" },
    { "OpenDiligentOverlay", &MakeRegisteredNativeCommand<OpenDiligentOverlayCommand>, false,
      R"json({"type":"object","properties":{"attach":{"type":"integer","minimum":0,"maximum":2}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"posted":{"type":"boolean"}},"additionalProperties":false,"required":["posted"]})json" },
    { "CloseDiligentOverlay", &MakeRegisteredNativeCommand<CloseDiligentOverlayCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"posted":{"type":"boolean"}},"additionalProperties":false,"required":["posted"]})json" },
    { "DiligentOverlayState",
      &MakeRegisteredNativeCommand<DiligentOverlayStateCommand>, false, R"json({"type":"object","properties":{},"additionalProperties":false})json", R"json({"type":"object","properties":{"active":{"type":"boolean"},"width":{"type":"integer","minimum":0},"height":{"type":"integer","minimum":0},"left":{"type":"integer"},"top":{"type":"integer"},"targetClass":{"type":"string"},"attach":{"type":"integer"},"how":{"type":"string"},"trackPolls":{"type":"integer","minimum":0},"trackMoves":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["active","width","height","left","top","targetClass","how","trackPolls","trackMoves"]})json" },
    { "SetDiligentRenderMode", &MakeRegisteredNativeCommand<SetDiligentRenderModeCommand>, false,
      R"json({"type":"object","properties":{"mode":{"type":"integer","minimum":0,"maximum":2}},"additionalProperties":false,"required":["mode"]})json",
      R"json({"type":"object","properties":{"mode":{"type":"integer","minimum":0,"maximum":2}},"additionalProperties":false,"required":["mode"]})json" },
    { "SetDiligentCallout", &MakeRegisteredNativeCommand<SetDiligentCalloutCommand>, false,
      R"json({"type":"object","properties":{"enabled":{"type":"boolean"}},"additionalProperties":false,"required":["enabled"]})json",
      R"json({"type":"object","properties":{"enabled":{"type":"boolean"}},"additionalProperties":false,"required":["enabled"]})json" },
    { "SetDiligentSun", &MakeRegisteredNativeCommand<SetDiligentSunCommand>, false,
      R"json({"type":"object","properties":{"enabled":{"type":"boolean"},"azimuthDegrees":{"type":"number","minimum":-360,"maximum":360},"altitudeDegrees":{"type":"number","minimum":-90,"maximum":90}},"additionalProperties":false,"required":["enabled"]})json",
      R"json({"type":"object","properties":{"enabled":{"type":"boolean"},"azimuthDegrees":{"type":"number"},"altitudeDegrees":{"type":"number"}},"additionalProperties":false,"required":["enabled","azimuthDegrees","altitudeDegrees"]})json" },
    { "SetDiligentDebugView", &MakeRegisteredNativeCommand<SetDiligentDebugViewCommand>, false,
      R"json({"type":"object","properties":{"view":{"type":"integer","minimum":0,"maximum":12}},"additionalProperties":false,"required":["view"]})json",
      R"json({"type":"object","properties":{"view":{"type":"integer","minimum":0,"maximum":12}},"additionalProperties":false,"required":["view"]})json" },
    { "SetDiligentEnvironmentMap", &MakeRegisteredNativeCommand<SetDiligentEnvironmentMapCommand>, false,
      R"json({"type":"object","properties":{"path":{"type":"string"},"enabled":{"type":"boolean"},"intensity":{"type":"number","minimum":0,"maximum":20},"rotationDegrees":{"type":"number","minimum":-360,"maximum":360}},"additionalProperties":false,"required":["path"]})json",
      R"json({"type":"object","properties":{"requested":{"type":"string"},"enabled":{"type":"boolean"},"intensity":{"type":"number"},"rotationDegrees":{"type":"number"}},"additionalProperties":false,"required":["requested","enabled","intensity","rotationDegrees"]})json" },
};

} // namespace

NativeCommandRegistrations GetArchVizCommandRegistrations ()
{
    return MakeRegistrationView (kArchVizCommandRegistrations);
}

} // namespace geomsrv
