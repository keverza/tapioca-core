// NativeCommands/ViewerGpuStateCommands -- see the header for the boundary this
// file exists to keep. Extracted OUT of ViewerSyncCommands.cpp (2026-09-13) when
// the auto-orbit verb pushed that file past the size cap; the seam was already
// there, because nothing in here is part of how the viewer follows Archicad.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/ViewerGpuStateCommands.hpp"
#include "NativeCommands/CommandRegistration.hpp"

#include "ArchViz/ArchVizPanel.hpp"
#include "ArchViz/AutoOrbit.hpp"
#include "ArchViz/Dxgi/ContextHook.hpp"
#include "ArchViz/Dxgi/DeviceIdentity.hpp"
#include "ArchViz/Dxgi/ContextStateTracker.hpp"
#include "ArchViz/Dxgi/CameraCensus.hpp"
#include "ArchViz/Dxgi/InjectionOracle.hpp"
#include "ArchViz/Dxgi/InjectionRenderer.hpp"
#include "ArchViz/Dxgi/PresentHook.hpp"
#include "ArchViz/Dxgi/RenderStateCapture.hpp"
#include "ArchViz/Dxgi/ViewMatrixCandidates.hpp"
#include "ArchViz/PatchProfile.hpp"
#include "ArchViz/ViewportOverlayWindow.hpp"

#include <string>

namespace geomsrv {

namespace av = geomsrv::archviz;

namespace {

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
        // 0..3 are one captured block; 4..7 are the PRODUCT of an affine block
        // and a projective one, which is the only form a renderer that uploads
        // view and projection separately can be found in.
        static const char* const kVariants[] = {"as-stored", "transposed",
                                                "inverse", "inverse-transposed",
                                                "view x proj", "viewT x proj",
                                                "view x projT", "viewT x projT",
                                                "captured-view x OURproj",
                                                "captured-viewT x OURproj",
                                                "OURview x captured-proj",
                                                "OURview x captured-projT"};
        GS::Array<GS::ObjectState> rows;
        for (size_t i = 0; i < count; ++i) {
            const av::dxgi::viewmatrix::Candidate& candidate = found[i];
            GS::ObjectState row;
            // As a STRING: a buffer pointer does not fit an Int32, and the value
            // is only ever compared for equality across rows and runs.
            row.Add ("buffer",
                     GS::UniString (std::to_string (candidate.buffer).c_str (), CC_UTF8));
            row.Add ("byteOffset", (GS::Int32) candidate.byteOffset);
            // Which window of a ring it came from; 0 for a buffer that is its
            // own window. `byteOffset` already includes it.
            row.Add ("windowOffset", (GS::Int32) candidate.windowOffset);
            // Only meaningful for a product variant: where the projective half
            // came from. `byteOffset` is the affine half.
            row.Add ("pairedOffset", (GS::Int32) candidate.pairedOffset);
            row.Add ("byteWidth", (GS::Int32) candidate.byteWidth);
            row.Add ("variant", GS::UniString (kVariants[candidate.variant % 12], CC_UTF8));
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
// ---------------------------------------------------------------------------
// Tapioca.ViewerAutoOrbit { enabled, degreesPerStep? } -> { running, steps }
//
// Turn Archicad's own 3D camera at a fixed rate so a measurement phase does the
// same amount of work every time. See ArchViz/AutoOrbit.hpp for why this is a
// step on the camera-sync tick rather than a loop in the diagnostic, and for the
// promise that the projection is saved and restored inside the add-on.
//
// ⚠️ IT MOVES THE USER'S OWN 3D VIEW. Anything that turns it on owes a `Stop`,
// and `Stop` is also what `CameraSyncReset` calls: a cancelled run must not be
// able to leave the view rotated, and after a Stop the bus refuses the calls a
// diagnostic's own `finally` would make.
// ---------------------------------------------------------------------------
class ViewerAutoOrbitCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "ViewerAutoOrbit"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        namespace orbit = geomsrv::archviz::autoorbit;

        bool enabled = false;
        params.Get ("enabled", enabled);

        if (!enabled) {
            orbit::Stop ();
        } else {
            double degreesPerStep = 0.4;
            params.Get ("degreesPerStep", degreesPerStep);
            GS::UniString error;
            if (!orbit::Start (degreesPerStep, error))
                return NativeCommandResult::Failure (error);
        }

        GS::ObjectState os;
        os.Add ("running", orbit::IsRunning ());
        os.Add ("steps", (GS::Int32) orbit::StepsTaken ());
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.ViewerInjectTriangle { enabled } -> { enabled, injected, ... }
//
// Stage 5 Proof A. Arms the native injected triangle, and reports its counters.
//
// ⚠️ AN EXPLICIT SWITCH, NOT A SIDE EFFECT OF ARMING THE DIAGNOSTIC HOOKS. This
// is the only thing in the project that draws into Archicad's own scene target,
// and it must be impossible to turn on by accident -- arming `hookdiag` for any
// other reason must not start injecting geometry into the user's 3D window.
// `CameraSyncReset` turns it off along with everything else.
// ---------------------------------------------------------------------------
class ViewerInjectTriangleCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "ViewerInjectTriangle"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        namespace inj = av::dxgi::injection;

        // ⚠️ THE ANCHOR IS SET BEFORE THE ARM, so a run that names a point gets a
        // vertex there on its first injection rather than on its second.
        if (params.Contains ("x") || params.Contains ("y") || params.Contains ("z") ||
            params.Contains ("sizeMetres")) {
            double x = 0.0, y = 0.0, z = 0.0, size = 1.0;
            params.Get ("x", x);
            params.Get ("y", y);
            params.Get ("z", z);
            params.Get ("sizeMetres", size);
            inj::SetAnchor (float (x), float (y), float (z), float (size));
        }

        // ⚠️ WHERE TO DRAW. "present" is Proof A -- on top of everything, which
        // settles the transform question alone. "scenepass" is Proof B's point,
        // inside the pass where Archicad's depth buffer still exists.
        if (params.Contains ("point")) {
            GS::UniString point;
            params.Get ("point", point);
            // ⚠️ "both" DRAWS AT BOTH POINTS IN ONE RUN. Seeing the scene-pass
            // triangle and not the Present one -- or the reverse -- is itself the
            // answer, and it costs one run instead of three.
            inj::SetPoint (point == "scenepass" ? inj::Point::ScenePass
                           : point == "both"    ? inj::Point::Both
                                                : inj::Point::Present);
        }

        // ⚠️ LEARNING AND DRAWING ARE SEPARATE PHASES. The occurrence table is
        // gathered while nothing is injected -- an occurrence chosen from
        // statistics gathered while drawing with an unchosen occurrence would be
        // a circular argument -- and only then is one locked.
        if (params.Contains ("occurrenceReset")) {
            bool wanted = false;
            params.Get ("occurrenceReset", wanted);
            if (wanted) {
                inj::ResetOccurrences ();
                inj::ClearOccurrenceLock ();
            }
        }
        bool occurrenceSelected = false;
        if (params.Contains ("occurrenceSelect")) {
            bool wanted = false;
            params.Get ("occurrenceSelect", wanted);
            if (wanted)
                occurrenceSelected = inj::SelectOccurrence ();
        }

        bool enabled = false;
        if (params.Contains ("enabled")) {
            params.Get ("enabled", enabled);
            inj::SetEnabled (enabled);
        }

        const inj::InjectionStats stats = inj::GetInjectionStats ();
        GS::ObjectState os;
        os.Add ("enabled", inj::Enabled ());
        os.Add ("point", GS::UniString (inj::GetPoint () == inj::Point::Present ? "present"
                                        : inj::GetPoint () == inj::Point::Both ? "both"
                                        : "scenepass", CC_UTF8));

        // ⚠️ WHERE THE CAMERA COMES FROM, STATED OUTRIGHT. An accidental fallback
        // to the learner is the one failure that would look like success.
        const inj::CameraSource cameraSource = inj::GetCameraSource ();
        os.Add ("cameraSource",
                GS::UniString (cameraSource == inj::CameraSource::CensusSelectedGroup
                                       ? "CensusSelectedGroup"
                               : cameraSource == inj::CameraSource::Learner ? "Learner"
                                                                            : "None",
                               CC_UTF8));
        const inj::SelectedCameraState selected = inj::GetSelectedCamera ();
        os.Add ("selectedCameraValid", selected.valid);
        os.Add ("selectedGroupId", (GS::Int32) stats.selectedGroupId);
        os.Add ("selectedGroupDraws", (GS::Int32) stats.selectedGroupDraws);
        os.Add ("selectedGroupSnapshots", (GS::Int32) stats.selectedGroupSnapshots);
        os.Add ("selectedSnapshotGeneration", (GS::Int32) stats.selectedSnapshotGeneration);
        os.Add ("occurrenceSelected", occurrenceSelected);
        os.Add ("occurrenceLocked", stats.occurrenceLocked);
        os.Add ("lockedOccurrence", (GS::Int32) stats.lockedOccurrence);
        os.Add ("occurrenceDraws", (GS::Int32) stats.occurrenceDraws);
        os.Add ("authoritativeSnapshots", (GS::Int32) stats.authoritativeSnapshots);
        os.Add ("occurrenceModelFrames", (GS::Int32) stats.occurrenceModelFrames);
        {
            inj::OccurrenceStats occurrences[inj::kOccurrenceCapacity];
            const size_t count = inj::CopyOccurrences (occurrences,
                    inj::kOccurrenceCapacity);
            GS::Array<GS::ObjectState> rows;
            for (size_t i = 0; i < count; ++i) {
                GS::ObjectState row;
                row.Add ("index", (GS::Int32) occurrences[i].index);
                row.Add ("draws", (GS::Int32) occurrences[i].draws);
                row.Add ("modelFrames", (GS::Int32) occurrences[i].modelFrames);
                row.Add ("samples", (GS::Int32) occurrences[i].samples);
                row.Add ("insideClip", (GS::Int32) occurrences[i].insideClip);
                row.Add ("medianCentreError", (double) occurrences[i].medianCentreError);
                row.Add ("meanCentreError", (double) occurrences[i].meanCentreError);
                row.Add ("worstCentreError", (double) occurrences[i].worstCentreError);
                row.Add ("viewportWidth", (double) occurrences[i].viewportWidth);
                row.Add ("viewportHeight", (double) occurrences[i].viewportHeight);
                row.Add ("lastDrawSequence", (GS::Int32) occurrences[i].lastDrawSequence);
                rows.Push (row);
            }
            os.Add ("occurrences", rows);
        }
        os.Add ("injectedPresent", (GS::Int32) stats.injectedPresent);
        os.Add ("injectedScenePass", (GS::Int32) stats.injectedScenePass);
        os.Add ("shaderInterpretation", (GS::Int32) stats.shaderInterpretation);
        os.Add ("expectedInterpretation", (GS::Int32) stats.expectedInterpretation);
        os.Add ("interpretationAgrees", stats.interpretationAgrees);
        os.Add ("skippedInterpretation", (GS::Int32) stats.skippedInterpretation);
        const av::dxgi::contextstate::DrawCameraCounts cameraCounts =
                av::dxgi::contextstate::GetDrawCameraCounts ();
        os.Add ("drawsTotal", (GS::Int32) cameraCounts.total);
        os.Add ("drawsWithView", (GS::Int32) cameraCounts.withView);
        os.Add ("drawsWithProjection", (GS::Int32) cameraCounts.withProjection);
        os.Add ("drawsWithBothCamera", (GS::Int32) cameraCounts.withBoth);
        os.Add ("initialised", stats.initialised);
        os.Add ("injected", (GS::Int32) stats.injected);
        os.Add ("skippedNoSceneDraw", (GS::Int32) stats.skippedNoSceneDraw);
        os.Add ("skippedNoCamera", (GS::Int32) stats.skippedNoCamera);
        os.Add ("skippedNotReady", (GS::Int32) stats.skippedNotReady);
        os.Add ("skippedPassMismatch", (GS::Int32) stats.skippedPassMismatch);
        os.Add ("skippedWindowSize", (GS::Int32) stats.skippedWindowSize);
        os.Add ("skippedReentrant", (GS::Int32) stats.skippedReentrant);
        os.Add ("skippedStaleCamera", (GS::Int32) stats.skippedStaleCamera);
        os.Add ("backBufferFailures", (GS::Int32) stats.backBufferFailures);
        os.Add ("newScene", (GS::Int32) stats.newScene);
        os.Add ("repeatScene", (GS::Int32) stats.repeatScene);
        os.Add ("invalidScene", (GS::Int32) stats.invalidScene);
        os.Add ("snapshotsTaken", (GS::Int32) stats.snapshotsTaken);
        os.Add ("qualifyingCameraDraws", (GS::Int32) stats.qualifyingCameraDraws);
        os.Add ("viewCopies", (GS::Int32) stats.viewCopies);
        os.Add ("projectionCopies", (GS::Int32) stats.projectionCopies);
        os.Add ("snapshotValid", stats.snapshotValid);
        os.Add ("drawsWithBothInModelPass", (GS::Int32) cameraCounts.withBothInModelPass);

        // Why a target departure was or was not taken as scene completion.
        const av::dxgi::renderstate::DepartureStats departures =
                av::dxgi::renderstate::GetDepartureStats ();
        os.Add ("departuresSeen", (GS::Int32) departures.departuresSeen);
        os.Add ("acceptedAsScene", (GS::Int32) departures.acceptedAsScene);
        os.Add ("rejectedTooFewDraws", (GS::Int32) departures.rejectedTooFewDraws);
        os.Add ("rejectedAlreadyDone", (GS::Int32) departures.rejectedAlreadyDone);
        os.Add ("drawThreshold", (GS::Int32) departures.drawThreshold);
        os.Add ("busiestPassDraws", (GS::Int32) departures.busiestPassDraws);

        // ⚠️ WHAT PHASE 1 LEARNED. Nothing is injected until `learned` is true,
        // and it only becomes true when consecutive frames agree on the same
        // colour resource, depth view and viewport for a camera-bearing pass.
        const av::dxgi::renderstate::SceneSignature signature =
                av::dxgi::renderstate::GetSceneSignature ();
        os.Add ("signatureLearned", signature.learned);
        os.Add ("signatureStableFrames", (GS::Int32) signature.stableFrames);
        os.Add ("signatureFramesWatched", (GS::Int32) signature.framesWatched);
        os.Add ("signatureCandidates", (GS::Int32) signature.candidatesThisFrame);
        os.Add ("signatureDraws", (GS::Int32) signature.draws);
        os.Add ("signatureViewportWidth", (double) signature.viewportWidth);
        os.Add ("signatureViewportHeight", (double) signature.viewportHeight);
        os.Add ("sceneConsumers",
                (GS::Int32) av::dxgi::renderstate::SceneConsumedCount ());
        os.Add ("lastError", GS::UniString (stats.lastError, CC_UTF8));
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.ViewerInjectionOracle { limit } -> { rows, counters }
//
// ⚠️ THE NUMERIC WITNESS FOR STAGE 5, AND IT EXISTS BECAUSE THE EYE RAN OUT OF
// ANSWERS. Six runs ended with "visible at rest, gone in motion", and every one
// of them was compatible with a wrong camera, a correct camera that rasterised
// nothing, or a correct rasterisation that something painted over. One row per
// tested Present makes each of those a different set of numbers. See
// ArchViz/Dxgi/InjectionOracle.hpp.
// ---------------------------------------------------------------------------
class ViewerInjectionOracleCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "ViewerInjectionOracle"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        namespace orc = av::dxgi::injection::oracle;

        // ⚠️ THE RESET IS WHAT BOUNDS THE MEASUREMENT TO THE GESTURE. Called
        // immediately before the navigation interval, it makes the variant table
        // describe that interval and nothing before it -- which is the whole
        // correction run twenty-eight demanded, because its verdict was decided
        // by the frames after the user let go of the mouse.
        bool reset = false;
        if (params.Contains ("reset"))
            params.Get ("reset", reset);
        if (reset)
            orc::ResetStatistics ();

        GS::Int32 limit = (GS::Int32) orc::kRowCapacity;
        if (params.Contains ("limit"))
            params.Get ("limit", limit);
        if (limit < 1)
            limit = 1;
        if (limit > (GS::Int32) orc::kRowCapacity)
            limit = (GS::Int32) orc::kRowCapacity;

        orc::Row rows[orc::kRowCapacity];
        const size_t count = orc::CopyRows (rows, size_t (limit));

        GS::Array<GS::ObjectState> out;
        for (size_t i = 0; i < count; ++i) {
            const orc::Row& row = rows[i];
            GS::ObjectState entry;
            entry.Add ("present", (GS::Int32) row.present);
            entry.Add ("modelSceneGeneration", (GS::Int32) row.modelSceneGeneration);
            entry.Add ("snapshotSequence", (GS::Int32) row.snapshotSequence);
            entry.Add ("snapshotDrawSequence", (GS::Int32) row.snapshotDrawSequence);
            entry.Add ("state", (GS::Int32) row.state);
            entry.Add ("cameraSource", (GS::Int32) row.cameraSource);
            entry.Add ("selectedGroupId", (GS::Int32) row.selectedGroupId);
            entry.Add ("selectedGroupSnapshotGeneration",
                       (GS::Int32) row.selectedGroupSnapshotGeneration);
            entry.Add ("sourceDrawSequence", (GS::Int32) row.sourceDrawSequence);
            entry.Add ("sourceModelGeneration", (GS::Int32) row.sourceModelGeneration);
            // As strings: a buffer pointer does not fit an Int32, and these are
            // only ever compared for equality and read by eye.
            entry.Add ("viewBuffer",
                       GS::UniString (std::to_string (row.viewBuffer).c_str (), CC_UTF8));
            entry.Add ("viewFirstConstant", (GS::Int32) row.viewFirstConstant);
            entry.Add ("viewNumConstants", (GS::Int32) row.viewNumConstants);
            entry.Add ("projectionBuffer",
                       GS::UniString (std::to_string (row.projectionBuffer).c_str (), CC_UTF8));
            entry.Add ("projectionFirstConstant", (GS::Int32) row.projectionFirstConstant);
            entry.Add ("projectionNumConstants", (GS::Int32) row.projectionNumConstants);
            entry.Add ("matricesRead", row.matricesRead);
            entry.Add ("clipW", (double) row.clipW);
            entry.Add ("ndcX", (double) row.ndcX);
            entry.Add ("ndcY", (double) row.ndcY);
            entry.Add ("ndcZ", (double) row.ndcZ);
            entry.Add ("pixelX", (double) row.pixelX);
            entry.Add ("pixelY", (double) row.pixelY);
            entry.Add ("insideClipVolume", row.insideClipVolume);
            entry.Add ("viewportX", (double) row.viewportX);
            entry.Add ("viewportY", (double) row.viewportY);
            entry.Add ("viewportWidth", (double) row.viewportWidth);
            entry.Add ("viewportHeight", (double) row.viewportHeight);
            entry.Add ("bestVariant", (GS::Int32) row.bestVariant);
            entry.Add ("bestVariantPixelX", (double) row.bestVariantPixelX);
            entry.Add ("bestVariantPixelY", (double) row.bestVariantPixelY);
            entry.Add ("bestVariantCentreError", (double) row.bestVariantCentreError);
            entry.Add ("drawIssued", row.drawIssued);
            entry.Add ("samplesKnown", row.samplesKnown);
            entry.Add ("samplesPassed", (GS::Int32) row.samplesPassed);
            out.Push (entry);
        }

        // ⚠️ THE VARIANT TABLE IS THE RUN'S ANSWER AND THE ROWS ARE ONLY SAMPLES.
        // It is accumulated over every qualifying row of the interval, not over
        // the 64 that happen to still be in the ring.
        orc::VariantStats variants[orc::kVariantCount];
        const size_t variantCount = orc::CopyVariants (variants, orc::kVariantCount);
        GS::Array<GS::ObjectState> variantRows;
        for (size_t i = 0; i < variantCount; ++i) {
            GS::ObjectState entry;
            entry.Add ("variant", (GS::Int32) i);
            entry.Add ("rowsTested", (GS::Int32) variants[i].rowsTested);
            entry.Add ("rowsInsideClip", (GS::Int32) variants[i].rowsInsideClip);
            entry.Add ("rowsWon", (GS::Int32) variants[i].rowsWon);
            entry.Add ("medianCentreError", (double) variants[i].medianCentreError);
            entry.Add ("meanCentreError", (double) variants[i].meanCentreError);
            entry.Add ("worstCentreError", (double) variants[i].worstCentreError);
            variantRows.Push (entry);
        }

        const orc::Counters counters = orc::GetCounters ();
        GS::ObjectState os;
        os.Add ("variants", variantRows);
        os.Add ("rowsQualified", (GS::Int32) counters.rowsQualified);
        os.Add ("rowsRejectedNotNew", (GS::Int32) counters.rowsRejectedNotNew);
        os.Add ("rowsRejectedStill", (GS::Int32) counters.rowsRejectedStill);
        os.Add ("ready", counters.ready);
        os.Add ("rows", out);
        os.Add ("rowsCompleted", (GS::Int32) counters.rowsCompleted);
        os.Add ("rowsDroppedUnresolved", (GS::Int32) counters.rowsDroppedUnresolved);
        os.Add ("readbacksAttempted", (GS::Int32) counters.readbacksAttempted);
        os.Add ("readbacksServed", (GS::Int32) counters.readbacksServed);
        os.Add ("readbacksStillDrawing", (GS::Int32) counters.readbacksStillDrawing);
        os.Add ("readbacksStale", (GS::Int32) counters.readbacksStale);
        os.Add ("queriesIssued", (GS::Int32) counters.queriesIssued);
        os.Add ("queriesResolved", (GS::Int32) counters.queriesResolved);
        os.Add ("queriesUnavailable", (GS::Int32) counters.queriesUnavailable);
        os.Add ("snapshotsStaged", (GS::Int32) counters.snapshotsStaged);
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.ViewerCameraCensus { enabled, reset, limit } -> { groups, ... }
//
// ⚠️ THE LEARNER IS AN OBSERVER HERE AND THE CENSUS IS THE SOURCE OF TRUTH. Run
// thirty showed the learned model pass contains at least two different camera-
// bearing draw groups -- one projecting the orbit target near the centre with
// 2180 samples, one putting it off-screen with a clip z of fifty-two -- so
// asking which PASS is the model pass is asking the wrong question. This verb
// groups every draw that binds both camera windows by what the draw IS, scores
// each group's own bytes, and ranks them. It draws nothing. See
// ArchViz/Dxgi/CameraCensus.hpp.
// ---------------------------------------------------------------------------
class ViewerCameraCensusCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "ViewerCameraCensus"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        namespace cen = av::dxgi::census;

        if (params.Contains ("x") || params.Contains ("y") || params.Contains ("z")) {
            double x = 0.0, y = 0.0, z = 0.0;
            params.Get ("x", x);
            params.Get ("y", y);
            params.Get ("z", z);
            cen::SetAnchor (float (x), float (y), float (z));
        }
        // ⚠️ `reset` KEEPS THE SELECTION AND `clearSelection` DROPS IT. Phase B
        // resets the counts so the report can say whether the chosen group stayed
        // correct WHILE the triangle was drawn with it -- a different claim from
        // "it was correct when we chose it".
        bool reset = false;
        if (params.Contains ("reset"))
            params.Get ("reset", reset);
        if (reset)
            cen::ResetCounts ();
        bool clearSelection = false;
        if (params.Contains ("clearSelection"))
            params.Get ("clearSelection", clearSelection);
        if (clearSelection) {
            cen::ClearSelection ();
            av::dxgi::injection::SetCameraSource (
                    av::dxgi::injection::CameraSource::Learner);
        }

        // ⚠️ PHASE A'S DECISION, AND IT FAILS CLOSED. If no group clears the
        // eligibility gate the selection stays empty and the camera source stays
        // on the learner, so a run that could not identify the camera injects
        // nothing rather than injecting with whatever drew last.
        bool selected = false;
        if (params.Contains ("select")) {
            bool wanted = false;
            params.Get ("select", wanted);
            if (wanted) {
                selected = cen::SelectCandidate ();
                av::dxgi::injection::SetCameraSource (
                        selected ? av::dxgi::injection::CameraSource::CensusSelectedGroup
                                 : av::dxgi::injection::CameraSource::Learner);
            }
        }

        if (params.Contains ("enabled")) {
            bool enabled = false;
            params.Get ("enabled", enabled);
            cen::SetEnabled (enabled);
        }

        GS::Int32 limit = (GS::Int32) cen::kGroupCapacity;
        if (params.Contains ("limit"))
            params.Get ("limit", limit);
        if (limit < 1)
            limit = 1;
        if (limit > (GS::Int32) cen::kGroupCapacity)
            limit = (GS::Int32) cen::kGroupCapacity;

        cen::Group groups[cen::kGroupCapacity];
        const size_t count = cen::CopyGroups (groups, size_t (limit));

        GS::Array<GS::ObjectState> rows;
        for (size_t i = 0; i < count; ++i) {
            const cen::Group& group = groups[i];
            GS::ObjectState row;
            // As strings: a shader or view pointer does not fit an Int32, and
            // these are only ever compared for equality and read by eye.
            row.Add ("vertexShader",
                     GS::UniString (std::to_string (group.vertexShader).c_str (), CC_UTF8));
            row.Add ("renderTarget",
                     GS::UniString (std::to_string (group.renderTarget).c_str (), CC_UTF8));
            row.Add ("depthStencil",
                     GS::UniString (std::to_string (group.depthStencil).c_str (), CC_UTF8));
            row.Add ("viewportX", (double) group.viewportX);
            row.Add ("viewportY", (double) group.viewportY);
            row.Add ("viewportWidth", (double) group.viewportWidth);
            row.Add ("viewportHeight", (double) group.viewportHeight);
            row.Add ("viewBuffer",
                     GS::UniString (std::to_string (group.viewBuffer).c_str (), CC_UTF8));
            row.Add ("viewFirstConstant", (GS::Int32) group.viewFirstConstant);
            row.Add ("viewNumConstants", (GS::Int32) group.viewNumConstants);
            row.Add ("projectionBuffer",
                     GS::UniString (std::to_string (group.projectionBuffer).c_str (), CC_UTF8));
            row.Add ("projectionFirstConstant", (GS::Int32) group.projectionFirstConstant);
            row.Add ("projectionNumConstants", (GS::Int32) group.projectionNumConstants);
            row.Add ("b0Bound", group.b0Bound);
            row.Add ("b0Buffer",
                     GS::UniString (std::to_string (group.b0Buffer).c_str (), CC_UTF8));
            row.Add ("b0FirstConstant", (GS::Int32) group.b0FirstConstant);
            row.Add ("b0NumConstants", (GS::Int32) group.b0NumConstants);
            row.Add ("passGeneration", (GS::Int32) group.passGeneration);
            row.Add ("targetEpoch", (GS::Int32) group.targetEpoch);
            row.Add ("drawSequenceFirst", (GS::Int32) group.drawSequenceFirst);
            row.Add ("drawSequenceLast", (GS::Int32) group.drawSequenceLast);
            row.Add ("drawsObserved", (GS::Int32) group.drawsObserved);
            row.Add ("framesObserved", (GS::Int32) group.framesObserved);
            row.Add ("modelFramesObserved", (GS::Int32) group.modelFramesObserved);
            row.Add ("groupId", (GS::Int32) group.groupId);
            row.Add ("firstPresent", (GS::Int32) group.firstPresent);
            row.Add ("lastPresent", (GS::Int32) group.lastPresent);
            row.Add ("drawKindMask", (GS::Int32) group.drawKindMask);
            row.Add ("lastIndexCount", (GS::Int32) group.lastIndexCount);
            row.Add ("samplesScored", (GS::Int32) group.samplesScored);
            row.Add ("winningVariant", (GS::Int32) group.winningVariant);
            row.Add ("winningVariantValid", (GS::Int32) group.winningVariantValid);
            row.Add ("medianCentreError", (double) group.medianCentreError);
            row.Add ("meanCentreError", (double) group.meanCentreError);
            row.Add ("worstCentreError", (double) group.worstCentreError);
            rows.Push (row);
        }

        const cen::Selection selection = cen::GetSelection ();
        GS::ObjectState chosen;
        chosen.Add ("valid", selection.valid);
        chosen.Add ("groupId", (GS::Int32) selection.groupId);
        chosen.Add ("vertexShader",
                    GS::UniString (std::to_string (selection.vertexShader).c_str (), CC_UTF8));
        chosen.Add ("renderTarget",
                    GS::UniString (std::to_string (selection.renderTarget).c_str (), CC_UTF8));
        chosen.Add ("depthStencil",
                    GS::UniString (std::to_string (selection.depthStencil).c_str (), CC_UTF8));
        chosen.Add ("viewportWidth", (double) selection.viewportWidth);
        chosen.Add ("viewportHeight", (double) selection.viewportHeight);
        chosen.Add ("variant", (GS::Int32) selection.variant);
        chosen.Add ("samples", (GS::Int32) selection.samples);
        chosen.Add ("modelCoverage", (double) selection.modelCoverage);
        chosen.Add ("insideClip", (double) selection.insideClip);
        chosen.Add ("medianCentreError", (double) selection.medianCentreError);
        chosen.Add ("snapshotsTaken", (GS::Int32) selection.snapshotsTaken);

        const cen::Eligibility gate = cen::GetEligibility ();
        GS::ObjectState gateState;
        gateState.Add ("minSamples", (GS::Int32) gate.minSamples);
        gateState.Add ("minModelCoverage", (double) gate.minModelCoverage);
        gateState.Add ("minInsideClip", (double) gate.minInsideClip);
        gateState.Add ("maxMedianCentreError", (double) gate.maxMedianCentreError);

        const cen::Stats stats = cen::GetStats ();
        GS::ObjectState os;
        os.Add ("selection", chosen);
        os.Add ("selected", selected);
        os.Add ("eligibility", gateState);
        os.Add ("modelFramesSeen", (GS::Int32) stats.modelFramesSeen);
        os.Add ("cameraSource",
                GS::UniString (av::dxgi::injection::GetCameraSource () ==
                                       av::dxgi::injection::CameraSource::CensusSelectedGroup
                               ? "CensusSelectedGroup" : "Learner", CC_UTF8));
        os.Add ("enabled", cen::Enabled ());
        os.Add ("ready", stats.ready);
        os.Add ("groups", rows);
        os.Add ("drawsSeen", (GS::Int32) stats.drawsSeen);
        os.Add ("drawsQualified", (GS::Int32) stats.drawsQualified);
        os.Add ("framesSeen", (GS::Int32) stats.framesSeen);
        os.Add ("groupsUsed", (GS::Int32) stats.groupsUsed);
        os.Add ("groupsOverflowed", (GS::Int32) stats.groupsOverflowed);
        os.Add ("copiesIssued", (GS::Int32) stats.copiesIssued);
        os.Add ("readbacksServed", (GS::Int32) stats.readbacksServed);
        os.Add ("readbacksBusy", (GS::Int32) stats.readbacksBusy);
        return os;
    }
};

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

const NativeCommandRegistration kViewerGpuStateCommandRegistrations[] = {
    { "ViewerPatchProfile", &MakeRegisteredNativeCommand<ViewerPatchProfileCommand>, false,
      R"json({"type":"object","properties":{"pin":{"type":"boolean"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"path":{"type":"string"},"pinned":{"type":"boolean"},"pinnedSummary":{"type":"string"},"currentSummary":{"type":"string"},"hostPath":{"type":"string"},"hostVersion":{"type":"string"},"hostSha256":{"type":"string"},"targets":{"type":"integer","minimum":0},"verifies":{"type":"boolean"},"verifyError":{"type":"string"}},"additionalProperties":false,"required":["path","pinned","verifies"]})json" },
    { "ViewerGpuStateSlots", &MakeRegisteredNativeCommand<ViewerGpuStateSlotsCommand>, false,
      R"json({"type":"object","properties":{"constantBuffers":{"type":"boolean"},"slot":{"type":"string"},"enabled":{"type":"boolean"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"installed":{"type":"boolean"},"slots":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"enabled":{"type":"boolean"},"calls":{"type":"integer"}},"additionalProperties":false,"required":["name","enabled","calls"]}}},"additionalProperties":false,"required":["installed","slots"]})json" },
    { "ViewerGpuStateCandidates", &MakeRegisteredNativeCommand<ViewerGpuStateCandidatesCommand>, false,
      R"json({"type":"object","properties":{"limit":{"type":"integer","minimum":1,"maximum":16}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"scored":{"type":"boolean"},"referenceValid":{"type":"boolean"},"candidates":{"type":"array","items":{"type":"object","properties":{"buffer":{"type":"string"},"byteOffset":{"type":"integer"},"windowOffset":{"type":"integer"},"pairedOffset":{"type":"integer"},"byteWidth":{"type":"integer"},"variant":{"type":"string"},"boundBy":{"type":"string"},"bindSlot":{"type":"integer"},"maxPixelError":{"type":"number"},"meanPixelError":{"type":"number"},"changesWhileMoving":{"type":"integer"},"changesWhileStill":{"type":"integer"}},"additionalProperties":false,"required":["buffer","byteOffset","variant","maxPixelError"]}}},"additionalProperties":false,"required":["scored","referenceValid","candidates"]})json" },
    { "ViewerInjectTriangle", &MakeRegisteredNativeCommand<ViewerInjectTriangleCommand>, false,
      R"json({"type":"object","properties":{"enabled":{"type":"boolean"},"point":{"type":"string","enum":["present","scenepass","both"]},"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"sizeMetres":{"type":"number","exclusiveMinimum":0,"maximum":1000},"occurrenceReset":{"type":"boolean"},"occurrenceSelect":{"type":"boolean"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"enabled":{"type":"boolean"},"point":{"type":"string"},"drawsTotal":{"type":"integer"},"drawsWithView":{"type":"integer"},"drawsWithProjection":{"type":"integer"},"drawsWithBothCamera":{"type":"integer"},"initialised":{"type":"boolean"},"injected":{"type":"integer"},"skippedNoSceneDraw":{"type":"integer"},"skippedNoCamera":{"type":"integer"},"skippedNotReady":{"type":"integer"},"skippedPassMismatch":{"type":"integer"},"skippedWindowSize":{"type":"integer"},"skippedReentrant":{"type":"integer"},"skippedStaleCamera":{"type":"integer"},"backBufferFailures":{"type":"integer"},"newScene":{"type":"integer"},"repeatScene":{"type":"integer"},"invalidScene":{"type":"integer"},"snapshotsTaken":{"type":"integer"},"qualifyingCameraDraws":{"type":"integer"},"cameraSource":{"type":"string"},"selectedCameraValid":{"type":"boolean"},"selectedGroupId":{"type":"integer"},"selectedGroupDraws":{"type":"integer"},"selectedGroupSnapshots":{"type":"integer"},"selectedSnapshotGeneration":{"type":"integer"},"occurrenceSelected":{"type":"boolean"},"occurrenceLocked":{"type":"boolean"},"lockedOccurrence":{"type":"integer"},"occurrenceDraws":{"type":"integer"},"authoritativeSnapshots":{"type":"integer"},"occurrenceModelFrames":{"type":"integer"},"occurrences":{"type":"array","items":{"type":"object","properties":{"index":{"type":"integer"},"draws":{"type":"integer"},"modelFrames":{"type":"integer"},"samples":{"type":"integer"},"insideClip":{"type":"integer"},"medianCentreError":{"type":"number"},"meanCentreError":{"type":"number"},"worstCentreError":{"type":"number"},"viewportWidth":{"type":"number"},"viewportHeight":{"type":"number"},"lastDrawSequence":{"type":"integer"}},"additionalProperties":false,"required":["index","draws","samples"]}},"injectedPresent":{"type":"integer"},"injectedScenePass":{"type":"integer"},"shaderInterpretation":{"type":"integer"},"expectedInterpretation":{"type":"integer"},"interpretationAgrees":{"type":"boolean"},"skippedInterpretation":{"type":"integer"},"viewCopies":{"type":"integer"},"projectionCopies":{"type":"integer"},"snapshotValid":{"type":"boolean"},"drawsWithBothInModelPass":{"type":"integer"},"departuresSeen":{"type":"integer"},"acceptedAsScene":{"type":"integer"},"rejectedTooFewDraws":{"type":"integer"},"rejectedAlreadyDone":{"type":"integer"},"drawThreshold":{"type":"integer"},"busiestPassDraws":{"type":"integer"},"signatureLearned":{"type":"boolean"},"signatureStableFrames":{"type":"integer"},"signatureFramesWatched":{"type":"integer"},"signatureCandidates":{"type":"integer"},"signatureDraws":{"type":"integer"},"signatureViewportWidth":{"type":"number"},"signatureViewportHeight":{"type":"number"},"sceneConsumers":{"type":"integer"},"lastError":{"type":"string"}},"additionalProperties":false,"required":["enabled","injected"]})json" },
    { "ViewerInjectionOracle", &MakeRegisteredNativeCommand<ViewerInjectionOracleCommand>, false,
      R"json({"type":"object","properties":{"limit":{"type":"integer","minimum":1,"maximum":64},"reset":{"type":"boolean"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"ready":{"type":"boolean"},"rows":{"type":"array","items":{"type":"object","properties":{"present":{"type":"integer"},"modelSceneGeneration":{"type":"integer"},"snapshotSequence":{"type":"integer"},"snapshotDrawSequence":{"type":"integer"},"state":{"type":"integer"},"cameraSource":{"type":"integer"},"selectedGroupId":{"type":"integer"},"selectedGroupSnapshotGeneration":{"type":"integer"},"sourceDrawSequence":{"type":"integer"},"sourceModelGeneration":{"type":"integer"},"viewBuffer":{"type":"string"},"viewFirstConstant":{"type":"integer"},"viewNumConstants":{"type":"integer"},"projectionBuffer":{"type":"string"},"projectionFirstConstant":{"type":"integer"},"projectionNumConstants":{"type":"integer"},"matricesRead":{"type":"boolean"},"clipW":{"type":"number"},"ndcX":{"type":"number"},"ndcY":{"type":"number"},"ndcZ":{"type":"number"},"pixelX":{"type":"number"},"pixelY":{"type":"number"},"insideClipVolume":{"type":"boolean"},"viewportX":{"type":"number"},"viewportY":{"type":"number"},"viewportWidth":{"type":"number"},"viewportHeight":{"type":"number"},"bestVariant":{"type":"integer"},"bestVariantPixelX":{"type":"number"},"bestVariantPixelY":{"type":"number"},"bestVariantCentreError":{"type":"number"},"drawIssued":{"type":"boolean"},"samplesKnown":{"type":"boolean"},"samplesPassed":{"type":"integer"}},"additionalProperties":false,"required":["present","state","matricesRead"]}},"rowsCompleted":{"type":"integer"},"rowsDroppedUnresolved":{"type":"integer"},"readbacksAttempted":{"type":"integer"},"readbacksServed":{"type":"integer"},"readbacksStillDrawing":{"type":"integer"},"readbacksStale":{"type":"integer"},"queriesIssued":{"type":"integer"},"queriesResolved":{"type":"integer"},"queriesUnavailable":{"type":"integer"},"snapshotsStaged":{"type":"integer"},"rowsQualified":{"type":"integer"},"rowsRejectedNotNew":{"type":"integer"},"rowsRejectedStill":{"type":"integer"},"variants":{"type":"array","items":{"type":"object","properties":{"variant":{"type":"integer"},"rowsTested":{"type":"integer"},"rowsInsideClip":{"type":"integer"},"rowsWon":{"type":"integer"},"medianCentreError":{"type":"number"},"meanCentreError":{"type":"number"},"worstCentreError":{"type":"number"}},"additionalProperties":false,"required":["variant","rowsTested"]}}},"additionalProperties":false,"required":["ready","rows"]})json" },
    { "ViewerCameraCensus", &MakeRegisteredNativeCommand<ViewerCameraCensusCommand>, false,
      R"json({"type":"object","properties":{"enabled":{"type":"boolean"},"reset":{"type":"boolean"},"limit":{"type":"integer","minimum":1,"maximum":32},"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"select":{"type":"boolean"},"clearSelection":{"type":"boolean"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"enabled":{"type":"boolean"},"ready":{"type":"boolean"},"groups":{"type":"array","items":{"type":"object","properties":{"vertexShader":{"type":"string"},"renderTarget":{"type":"string"},"depthStencil":{"type":"string"},"viewportX":{"type":"number"},"viewportY":{"type":"number"},"viewportWidth":{"type":"number"},"viewportHeight":{"type":"number"},"viewBuffer":{"type":"string"},"viewFirstConstant":{"type":"integer"},"viewNumConstants":{"type":"integer"},"projectionBuffer":{"type":"string"},"projectionFirstConstant":{"type":"integer"},"projectionNumConstants":{"type":"integer"},"b0Bound":{"type":"boolean"},"b0Buffer":{"type":"string"},"b0FirstConstant":{"type":"integer"},"b0NumConstants":{"type":"integer"},"passGeneration":{"type":"integer"},"targetEpoch":{"type":"integer"},"drawSequenceFirst":{"type":"integer"},"drawSequenceLast":{"type":"integer"},"drawsObserved":{"type":"integer"},"framesObserved":{"type":"integer"},"modelFramesObserved":{"type":"integer"},"groupId":{"type":"integer"},"firstPresent":{"type":"integer"},"lastPresent":{"type":"integer"},"drawKindMask":{"type":"integer"},"lastIndexCount":{"type":"integer"},"samplesScored":{"type":"integer"},"winningVariant":{"type":"integer"},"winningVariantValid":{"type":"integer"},"medianCentreError":{"type":"number"},"meanCentreError":{"type":"number"},"worstCentreError":{"type":"number"}},"additionalProperties":false,"required":["vertexShader","drawsObserved","samplesScored"]}},"drawsSeen":{"type":"integer"},"drawsQualified":{"type":"integer"},"framesSeen":{"type":"integer"},"groupsUsed":{"type":"integer"},"groupsOverflowed":{"type":"integer"},"copiesIssued":{"type":"integer"},"readbacksServed":{"type":"integer"},"readbacksBusy":{"type":"integer"},"modelFramesSeen":{"type":"integer"},"selected":{"type":"boolean"},"cameraSource":{"type":"string"},"eligibility":{"type":"object","properties":{"minSamples":{"type":"integer"},"minModelCoverage":{"type":"number"},"minInsideClip":{"type":"number"},"maxMedianCentreError":{"type":"number"}},"additionalProperties":false},"selection":{"type":"object","properties":{"valid":{"type":"boolean"},"groupId":{"type":"integer"},"vertexShader":{"type":"string"},"renderTarget":{"type":"string"},"depthStencil":{"type":"string"},"viewportWidth":{"type":"number"},"viewportHeight":{"type":"number"},"variant":{"type":"integer"},"samples":{"type":"integer"},"modelCoverage":{"type":"number"},"insideClip":{"type":"number"},"medianCentreError":{"type":"number"},"snapshotsTaken":{"type":"integer"}},"additionalProperties":false,"required":["valid"]}},"additionalProperties":false,"required":["enabled","groups"]})json" },
    { "ViewerGpuDeviceInfo", &MakeRegisteredNativeCommand<ViewerGpuDeviceInfoCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"deviceFound":{"type":"boolean"},"is11On12":{"type":"boolean"},"creationFlags":{"type":"integer"},"featureLevel":{"type":"integer"},"debugLayer":{"type":"boolean"},"singleThreaded":{"type":"boolean"},"bgraSupport":{"type":"boolean"},"highestDeviceInterface":{"type":"integer","minimum":0,"maximum":5},"openglLoaded":{"type":"boolean"},"openglIcdLoaded":{"type":"boolean"},"d3d12Loaded":{"type":"boolean"},"vulkanLoaded":{"type":"boolean"},"d2dLoaded":{"type":"boolean"},"dcompLoaded":{"type":"boolean"},"targetWindow":{"type":"string"},"targetHasPixelFormat":{"type":"boolean"},"targetPixelFormat":{"type":"integer"},"targetSupportsOpenGL":{"type":"boolean"},"targetSupportsGdi":{"type":"boolean"},"targetDoubleBuffered":{"type":"boolean"},"chains":{"type":"array","items":{"type":"object","properties":{"swapChain":{"type":"string"},"window":{"type":"string"},"presents":{"type":"integer"},"width":{"type":"integer"},"height":{"type":"integer"},"ours":{"type":"boolean"},"nominated":{"type":"boolean"}},"additionalProperties":false,"required":["swapChain","window","presents","ours","nominated"]}}},"additionalProperties":false,"required":["deviceFound","is11On12","chains"]})json" },
    { "ViewerAutoOrbit", &MakeRegisteredNativeCommand<ViewerAutoOrbitCommand>, false,
      R"json({"type":"object","properties":{"enabled":{"type":"boolean"},"degreesPerStep":{"type":"number","minimum":0.01,"maximum":15}},"additionalProperties":false,"required":["enabled"]})json",
      R"json({"type":"object","properties":{"running":{"type":"boolean"},"steps":{"type":"integer"}},"additionalProperties":false,"required":["running","steps"]})json" },
};

}   // namespace

NativeCommandRegistrations GetViewerGpuStateCommandRegistrations ()
{
    return MakeRegistrationView (kViewerGpuStateCommandRegistrations);
}

}   // namespace geomsrv
