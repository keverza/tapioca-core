// NativeCommands/ViewerInjectionCommands -- see the header for the boundary this
// file exists to keep.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/ViewerInjectionCommands.hpp"
#include "NativeCommands/CommandRegistration.hpp"

#include "ArchViz/Dxgi/CameraCensus.hpp"
#include "ArchViz/Dxgi/CameraRecognizer.hpp"
#include "ArchViz/Dxgi/DepthCheckpoints.hpp"
#include "ArchViz/Dxgi/ContextStateTracker.hpp"
#include "ArchViz/Dxgi/InjectionCamera.hpp"
#include "ArchViz/Dxgi/GhostMesh.hpp"
#include "ArchViz/Dxgi/HostOccluders.hpp"
#include "ArchViz/Dxgi/HostOverlay.hpp"
#include "ArchViz/Dxgi/InjectionDepth.hpp"
#include "ArchViz/Dxgi/InjectionOracle.hpp"
#include "ArchViz/Dxgi/InjectionProbes.hpp"
#include "ArchViz/Dxgi/InjectionRenderer.hpp"
#include "ArchViz/Dxgi/RenderStateCapture.hpp"

#include <string>

namespace geomsrv {

namespace av = geomsrv::archviz;

namespace {

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
    GS::String GetName () const override
    {
        return "ViewerInjectTriangle";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        namespace inj = av::dxgi::injection;

        // ⚠️ THE ANCHOR IS SET BEFORE THE ARM, so a run that names a point gets a
        // vertex there on its first injection rather than on its second.
        if (params.Contains ("x") || params.Contains ("y") || params.Contains ("z") || params.Contains ("sizeMetres")) {
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

        // ⚠️ WHAT THE OVERLAY TESTS AGAINST, AND IT IS NOW A PRODUCTION SETTING
        // RATHER THAN AN EXPERIMENT. "off" is the Proof A control; "scene" is
        // Proof B2's finding, Archicad's own depth read and never written;
        // "private" copies that depth into a texture we own and writes into
        // ours, so our own geometry occludes itself as well.
        if (params.Contains ("depth")) {
            GS::UniString depthMode;
            params.Get ("depth", depthMode);
            namespace dep = av::dxgi::injection::depth;
            dep::SetMode (depthMode == "private" ? dep::Mode::PrivateCopy
                          : depthMode == "scene" ? dep::Mode::SceneReadOnly
                                                 : dep::Mode::Off);
        }
        {
            // ⚠️ WHICH INSTANT OF ARCHICAD'S DEPTH, WHICH RUN
            // FORTY-EIGHT SHOWED MATTERS MORE THAN WHICH BUFFER. See
            // InjectionDepth.hpp: at Present the buffer already holds a
            // transparent plane's near write, and geometry in FRONT of the model
            // was being rejected 26% of the time.
            namespace dep = av::dxgi::injection::depth;
            if (params.Contains ("depthSource")) {
                GS::UniString source;
                params.Get ("depthSource", source);
                dep::SetSource (source == "opaque"        ? dep::Source::AfterOpaque
                                : source == "transparent" ? dep::Source::AfterTransparent
                                                          : dep::Source::AtPresent);
            }
            // ⚠️ THE CHECKPOINTS ARE ARMED SEPARATELY AND ARE
            // EXPENSIVE. Eight full-resolution depth textures and eight copies
            // on every instrumented frame is diagnostic machinery; nothing
            // production ever turns this on.
            {
                namespace chk = av::dxgi::injection::checkpoints;
                if (params.Contains ("checkpoints")) {
                    bool wanted = false;
                    params.Get ("checkpoints", wanted);
                    chk::SetEnabled (wanted);
                }
                if (params.Contains ("checkpointInterval")) {
                    GS::Int32 interval = 12;
                    params.Get ("checkpointInterval", interval);
                    chk::SetInterval (uint32_t (interval < 1 ? 1 : interval));
                }
                if (params.Contains ("checkpointReset")) {
                    bool wanted = false;
                    params.Get ("checkpointReset", wanted);
                    if (wanted)
                        chk::Reset ();
                }
            }
            if (params.Contains ("provenanceReset")) {
                bool wanted = false;
                params.Get ("provenanceReset", wanted);
                if (wanted)
                    dep::ResetProvenance ();
            }
        }

        // ⚠️ THE GHOST MESH IS ARMED SEPARATELY FROM THE INJECTION, and
        // that is deliberate. Arming the injection draws the frozen proof
        // primitives -- probes A, B and C and the production triangle -- which
        // are the regression test for the camera and depth work. A mesh bug must
        // not be able to take the instrument down with it.
        {
            namespace gh = av::dxgi::ghost;
            if (params.Contains ("mesh")) {
                bool wanted = false;
                params.Get ("mesh", wanted);
                gh::SetEnabled (wanted);
            }
            if (params.Contains ("meshAnimate")) {
                bool wanted = true;
                params.Get ("meshAnimate", wanted);
                gh::SetAnimated (wanted);
            }
            if (params.Contains ("meshStandIns")) {
                bool wanted = false;
                params.Get ("meshStandIns", wanted);
                gh::SetStandIns (wanted);
            }
        }

        // ⚠️ THE TWO HOST OVERLAYS, SWITCHED SEPARATELY. They draw
        // Archicad's OWN extracted building -- edges and scalar-coloured
        // surfaces -- from the same snapshot the occluder renders depth from,
        // so all three overlay kinds compose against one depth buffer instead of
        // each guessing.
        {
            namespace ho = av::dxgi::hostoverlay;
            if (params.Contains ("hostWireframe")) {
                bool wanted = true;
                params.Get ("hostWireframe", wanted);
                ho::SetEnabled (ho::Kind::Wireframe, wanted);
            }
            if (params.Contains ("hostHeatmap")) {
                bool wanted = true;
                params.Get ("hostHeatmap", wanted);
                ho::SetEnabled (ho::Kind::Heatmap, wanted);
            }
        }

        // ⚠️ THE DIAGNOSTIC ASKS FOR ITS OWN INSTRUMENTS, and that
        // is the whole point of the switch. The anchor triangle and the probe
        // wedges are how this command answers questions a picture cannot; they
        // are also exactly what a user must never see, so they are off unless
        // asked for here. Defaults to ON for this command so every existing
        // proof run behaves as it did.
        {
            bool proofs = true;
            if (params.Contains ("proofPrimitives"))
                params.Get ("proofPrimitives", proofs);
            inj::SetProofPrimitives (proofs);
        }

        bool enabled = false;
        if (params.Contains ("enabled")) {
            params.Get ("enabled", enabled);
            inj::SetEnabled (enabled);
        }

        const inj::InjectionStats stats = inj::GetInjectionStats ();
        GS::ObjectState os;
        {
            namespace gh = av::dxgi::ghost;
            const gh::Stats mesh = gh::GetStats ();
            os.Add ("mesh", gh::Enabled ());
            os.Add ("meshAnimate", gh::Animated ());
            os.Add ("meshCreated", mesh.created);
            os.Add ("meshBuilds", (GS::Int32) mesh.builds);
            os.Add ("meshUploads", (GS::Int32) mesh.uploads);
            os.Add ("meshUploadFailures", (GS::Int32) mesh.uploadFailures);
            os.Add ("meshDraws", (GS::Int32) mesh.draws);
            os.Add ("meshVertices", (GS::Int32) mesh.vertices);
            os.Add ("meshIndices", (GS::Int32) mesh.indices);
            os.Add ("meshTriangles", (GS::Int32) mesh.triangles);
            os.Add ("meshPhase", (GS::Int32) mesh.phase);
            os.Add ("meshError", GS::UniString (mesh.lastError, CC_UTF8));
            os.Add ("meshStandIns", gh::StandIns ());
            os.Add ("proofPrimitives", inj::ProofPrimitives ());

            namespace ho = av::dxgi::hostoverlay;
            const ho::Stats hostOverlay = ho::GetStats ();
            os.Add ("hostWireframe", ho::Enabled (ho::Kind::Wireframe));
            os.Add ("hostHeatmap", ho::Enabled (ho::Kind::Heatmap));
            os.Add ("hostOverlayReady", hostOverlay.ready);
            os.Add ("hostWireframeDraws", (GS::Int32) hostOverlay.wireframeDraws);
            os.Add ("hostHeatmapDraws", (GS::Int32) hostOverlay.heatmapDraws);
            os.Add ("hostHiddenPassDraws", (GS::Int32) hostOverlay.hiddenPassDraws);
            os.Add ("hostCulledPasses", (GS::Int32) hostOverlay.culledPasses);
            os.Add ("hostOverlayLines", (GS::Int32) hostOverlay.linesDrawn);
            os.Add ("hostOverlaySkippedNoEdges", (GS::Int32) hostOverlay.skippedNoEdges);
            os.Add ("hostOverlayTriangles", (GS::Int32) hostOverlay.trianglesDrawn);
            os.Add ("hostOverlaySkippedNoGeometry", (GS::Int32) hostOverlay.skippedNoGeometry);
            os.Add ("hostOverlaySkippedNoCamera", (GS::Int32) hostOverlay.skippedNoCamera);
            os.Add ("hostOverlayError", GS::UniString (hostOverlay.lastError, CC_UTF8));

            // ⚠️ WHETHER THE BUILDING EVER ARRIVED, WHICH IS THE ONE
            // THING THAT DECIDES WHETHER HOST OCCLUSION WAS TESTED AT ALL. A run
            // with no snapshot proves nothing about opaque-versus-transparent,
            // however good the picture looks.
            namespace host = av::dxgi::hostocclusion;
            const host::Stats hostStats = host::GetStats ();
            os.Add ("hostReady", hostStats.ready);
            os.Add ("hostHaveSnapshot", hostStats.haveSnapshot);
            os.Add ("hostPublishedTriangles", (GS::Int32) hostStats.publishedTriangles);
            os.Add ("hostPublishSucceeded", (GS::Int32) hostStats.publishSucceeded);
            os.Add ("hostPublishFailure", GS::UniString (hostStats.publishFailureReason, CC_UTF8));
            os.Add ("hostWindingKnown", hostStats.windingKnown);
            os.Add ("hostFrontCounterClockwise", hostStats.frontCounterClockwise);
            os.Add ("hostSignedVolume", double (hostStats.signedVolume));
            os.Add ("hostPublishedLines", (GS::Int32) hostStats.publishedLines);
            os.Add ("hostEdgesConsidered", (GS::Int32) hostStats.edgesConsidered);
            os.Add ("hostUploads", (GS::Int32) hostStats.uploads);
            os.Add ("hostRenders", (GS::Int32) hostStats.renders);
            os.Add ("hostVertices", (GS::Int32) hostStats.vertices);
            os.Add ("hostTriangles", (GS::Int32) hostStats.triangles);
            os.Add ("hostPendingVertices", (GS::Int32) hostStats.pendingVertices);
            os.Add ("hostElementsReceived", (GS::Int32) hostStats.elementsReceived);
            os.Add ("hostOpaqueTriangles", (GS::Int32) hostStats.opaqueTriangles);
            os.Add ("hostTransparentTriangles", (GS::Int32) hostStats.transparentTrianglesSkipped);
            os.Add ("hostBatchBegins", (GS::Int32) hostStats.batchBegins);
            os.Add ("hostBatchEnds", (GS::Int32) hostStats.batchEnds);
            os.Add ("hostSkippedNoCamera", (GS::Int32) hostStats.skippedNoCamera);
            os.Add ("hostSkippedNoGeometry", (GS::Int32) hostStats.skippedNoGeometry);
            os.Add ("hostSkippedNoDepthTarget", (GS::Int32) hostStats.skippedNoDepthTarget);
            os.Add ("hostDroppedOverCapacity", (GS::Int32) hostStats.droppedOverCapacity);
            os.Add ("hostError", GS::UniString (hostStats.lastError, CC_UTF8));
        }
        os.Add ("enabled", inj::Enabled ());
        os.Add ("point", GS::UniString (inj::GetPoint () == inj::Point::Present ? "present"
                                        : inj::GetPoint () == inj::Point::Both  ? "both"
                                                                                : "scenepass",
                                        CC_UTF8));

        // ⚠️ WHERE THE CAMERA COMES FROM, STATED OUTRIGHT. An accidental fallback
        // to the learner is the one failure that would look like success.
        const inj::CameraSource cameraSource = inj::GetCameraSource ();
        os.Add ("cameraSource",
                GS::UniString (cameraSource == inj::CameraSource::CensusSelectedGroup ? "CensusSelectedGroup"
                               : cameraSource == inj::CameraSource::Learner           ? "Learner"
                                                                                      : "None",
                               CC_UTF8));
        // ⚠️ THE ARM STATE, WHICH IS THE ANSWER TO "DID ENABLING
        // WORK". `enabled` alone cannot distinguish "armed and hunting" from
        // "armed and drawing", and run forty-five spent a whole run unable to
        // tell those apart from the outside.
        const inj::ArmState armState = inj::GetArmState ();
        os.Add ("armState", GS::UniString (armState == inj::ArmState::Active               ? "Active"
                                           : armState == inj::ArmState::ArmedPendingCamera ? "ArmedPendingCamera"
                                                                                           : "Disabled",
                                           CC_UTF8));
        const inj::SelectedCameraState selected = inj::GetSelectedCamera ();
        os.Add ("selectedCameraValid", selected.valid);

        // ⚠️ WHETHER THE LOGICAL IDENTITY SURVIVED, AND WHETHER IT
        // FOUND ANYTHING. These four come from the census and are repeated here
        // because this is the verb the caller arms with: a refusal has to be
        // able to name its cause without a second call.
        {
            namespace cen = av::dxgi::census;
            const cen::Stats censusStats = cen::GetStats ();
            os.Add ("fingerprintValid", censusStats.fingerprintValid);
            os.Add ("cameraLifecycle", GS::UniString (censusStats.lifecycle, CC_UTF8));
            os.Add ("censusSelectionMatches", (GS::Int32) censusStats.selectionMatches);
            os.Add ("censusLogicalMatches", (GS::Int32) censusStats.logicalMatches);
            os.Add ("censusRebinds", (GS::Int32) censusStats.rebinds);
            os.Add ("censusRebindsRefused", (GS::Int32) censusStats.rebindsRefused);
        }
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
            namespace dep = av::dxgi::injection::depth;
            const dep::Mode mode = dep::GetMode ();
            const dep::Stats depthStats = dep::GetStats ();
            os.Add ("depthMode", GS::UniString (mode == dep::Mode::PrivateCopy     ? "private"
                                                : mode == dep::Mode::SceneReadOnly ? "scene"
                                                                                   : "off",
                                                CC_UTF8));
            os.Add ("depthPreparations", (GS::Int32) depthStats.preparations);
            os.Add ("depthSourceViewAcquired", (GS::Int32) depthStats.sourceViewAcquired);
            os.Add ("depthSourceResourceAcquired", (GS::Int32) depthStats.sourceResourceAcquired);
            os.Add ("depthSourceDescAccepted", (GS::Int32) depthStats.sourceDescAccepted);
            os.Add ("depthTextureCreated", (GS::Int32) depthStats.textureCreated);
            os.Add ("depthViewCreated", (GS::Int32) depthStats.viewCreated);
            os.Add ("depthCopyIssued", (GS::Int32) depthStats.copyIssued);
            os.Add ("depthViewBound", (GS::Int32) depthStats.viewBound);
            os.Add ("depthCopies", (GS::Int32) depthStats.copies);
            os.Add ("depthNoSceneView", (GS::Int32) depthStats.noSceneView);
            os.Add ("depthCopyRefused", (GS::Int32) depthStats.copyRefused);
            os.Add ("depthRebuilds", (GS::Int32) depthStats.rebuilds);
            os.Add ("depthWidth", (GS::Int32) depthStats.width);
            os.Add ("depthHeight", (GS::Int32) depthStats.height);
            os.Add ("depthFormat", (GS::Int32) depthStats.format);
            os.Add ("depthSamples", (GS::Int32) depthStats.sampleCount);
            os.Add ("depthPrivateReady", depthStats.privateReady);
            os.Add ("depthError", GS::UniString (depthStats.lastError, CC_UTF8));

            const dep::Source source = dep::GetSource ();
            os.Add ("depthSource", GS::UniString (source == dep::Source::AfterOpaque        ? "opaque"
                                                  : source == dep::Source::AfterTransparent ? "transparent"
                                                                                            : "present",
                                                  CC_UTF8));
            const dep::Provenance prov = dep::GetProvenance ();
            os.Add ("provSceneDraws", (GS::Int32) prov.sceneDraws);
            os.Add ("provOpaqueDraws", (GS::Int32) prov.opaqueDraws);
            os.Add ("provBlendedDraws", (GS::Int32) prov.blendedDraws);
            os.Add ("provNoDepthWriteDraws", (GS::Int32) prov.noDepthWriteDraws);
            os.Add ("provDepthTestOffDraws", (GS::Int32) prov.depthTestOffDraws);
            os.Add ("provOpaqueAfterTransition", (GS::Int32) prov.opaqueAfterTransition);
            os.Add ("provTransitions", (GS::Int32) prov.transitions);
            os.Add ("provFramesWithoutOpaque", (GS::Int32) prov.framesWithoutOpaque);
            os.Add ("provFramesWithoutTransition", (GS::Int32) prov.framesWithoutTransition);
            os.Add ("provCapturedAfterOpaque", (GS::Int32) prov.capturedAfterOpaque);
            os.Add ("provCapturedAfterTransparent", (GS::Int32) prov.capturedAfterTransparent);
            os.Add ("provCapturedAtPresent", (GS::Int32) prov.capturedAtPresent);
            os.Add ("provServedStale", (GS::Int32) prov.servedStale);
            os.Add ("provLastOpaqueDrawIndex", (GS::Int32) prov.lastOpaqueDrawIndex);
            os.Add ("provLastSceneDrawIndex", (GS::Int32) prov.lastSceneDrawIndex);

            {
                namespace chk = av::dxgi::injection::checkpoints;
                const chk::Stats chkStats = chk::GetStats ();
                os.Add ("checkpoints", chk::Enabled ());
                os.Add ("checkpointReady", chkStats.ready);
                os.Add ("checkpointInterval", (GS::Int32) chkStats.interval);
                os.Add ("checkpointFrames", (GS::Int32) chkStats.framesInstrumented);
                os.Add ("checkpointRounds", (GS::Int32) chkStats.roundsIssued);
                os.Add ("checkpointDrawsLastFrame", (GS::Int32) chkStats.drawsLastFrame);
                os.Add ("checkpointSkippedNoCamera", (GS::Int32) chkStats.skippedNoCamera);
                os.Add ("checkpointSkippedNoPipeline", (GS::Int32) chkStats.skippedNoPipeline);
                os.Add ("checkpointOverflowed", (GS::Int32) chkStats.overflowed);
                os.Add ("checkpointQueriesIssued", (GS::Int32) chkStats.queriesIssued);
                os.Add ("checkpointQueriesResolved", (GS::Int32) chkStats.queriesResolved);
                os.Add ("checkpointUsed", (GS::Int32) chkStats.used);
                os.Add ("checkpointError", GS::UniString (chkStats.lastError, CC_UTF8));

                chk::Result results[chk::kCapacity];
                const uint32_t count = chk::CopyResults (results, chk::kCapacity);
                GS::Array<GS::ObjectState> rows;
                for (uint32_t i = 0; i < count; ++i) {
                    const chk::Result& row = results[i];
                    if (!row.attributes.used)
                        continue;
                    GS::ObjectState entry;
                    entry.Add ("checkpoint", (GS::Int32) i);
                    entry.Add ("drawOrdinal", (GS::Int32) row.attributes.drawOrdinal);
                    entry.Add ("drawKind", (GS::Int32) row.attributes.drawKind);
                    entry.Add ("indexCount", (GS::Int32) row.attributes.indexCount);
                    entry.Add ("topology", (GS::Int32) row.attributes.topology);
                    entry.Add ("depthWrite", row.attributes.depthWrite);
                    entry.Add ("depthTest", row.attributes.depthTest);
                    entry.Add ("depthFunc", (GS::Int32) row.attributes.depthFunc);
                    entry.Add ("blendEnable", row.attributes.blendEnable);
                    entry.Add ("srcBlend", (GS::Int32) row.attributes.srcBlend);
                    entry.Add ("destBlend", (GS::Int32) row.attributes.destBlend);
                    entry.Add ("vertexShader",
                               GS::UniString (std::to_string (row.attributes.vertexShader).c_str (), CC_UTF8));
                    entry.Add ("pixelShader",
                               GS::UniString (std::to_string (row.attributes.pixelShader).c_str (), CC_UTF8));
                    entry.Add ("blendState",
                               GS::UniString (std::to_string (row.attributes.blendState).c_str (), CC_UTF8));
                    entry.Add ("depthStencilState",
                               GS::UniString (std::to_string (row.attributes.depthStencilState).c_str (), CC_UTF8));
                    entry.Add ("rounds", (GS::Int32) row.rounds);
                    entry.Add ("frontSurvived", (GS::Int32) row.frontSurvived);
                    entry.Add ("behindSurvived", (GS::Int32) row.behindSurvived);
                    entry.Add ("ghostSurvived", (GS::Int32) row.ghostSurvived);
                    entry.Add ("frontSamples", (GS::Int32) row.frontSamples);
                    entry.Add ("behindSamples", (GS::Int32) row.behindSamples);
                    entry.Add ("ghostSamples", (GS::Int32) row.ghostSamples);
                    rows.Push (entry);
                }
                os.Add ("checkpointRows", rows);
            }
        }

        // ⚠️ THE THREE SAMPLE COUNTS THE RUN EXISTS TO OBTAIN. A, B and C differ
        // by exactly one thing each, so which of them produced pixels names the
        // remaining fault without a further run.
        {
            namespace prb = av::dxgi::injection::probes;
            if (params.Contains ("probeReset")) {
                bool wanted = false;
                params.Get ("probeReset", wanted);
                if (wanted)
                    prb::Reset ();
            }
            if (params.Contains ("frontOffsetZ") || params.Contains ("behindOffsetZ")) {
                double front = 2.0, behind = -6.0;
                params.Get ("frontOffsetZ", front);
                params.Get ("behindOffsetZ", behind);
                prb::SetDepthOffsets (float (front), float (behind));
            }
            const prb::Stats probes = prb::GetStats ();
            GS::Array<GS::ObjectState> rows;
            const char* const names[prb::kProbeCount] = {
                "A raster/output", "B camera shader", "C production IA",     "FRONT depth off",     "BEHIND depth off",
                "FRONT depth ON",  "BEHIND depth ON", "PRESENT front depth", "PRESENT behind depth"
            };
            for (size_t i = 0; i < prb::kProbeCount; ++i) {
                GS::ObjectState row;
                row.Add ("name", GS::UniString (names[i], CC_UTF8));
                row.Add ("draws", (GS::Int32) probes.probe[i].draws);
                row.Add ("queriesIssued", (GS::Int32) probes.probe[i].queriesIssued);
                row.Add ("queriesResolved", (GS::Int32) probes.probe[i].queriesResolved);
                row.Add ("drawsWithSamples", (GS::Int32) probes.probe[i].drawsWithSamples);
                row.Add ("totalSamples", (GS::Int32) probes.probe[i].totalSamples);
                rows.Push (row);
            }
            os.Add ("probes", rows);
            os.Add ("probesReady", probes.ready);
            // As strings: a 64-bit hash does not fit an Int32, and these are only
            // ever compared for equality and read by eye.
            os.Add ("cameraVsHash", GS::UniString (std::to_string (probes.cameraVsHash).c_str (), CC_UTF8));
            os.Add ("probeVsHash", GS::UniString (std::to_string (probes.probeVsHash).c_str (), CC_UTF8));
            os.Add ("rasterVsHash", GS::UniString (std::to_string (probes.rasterVsHash).c_str (), CC_UTF8));
            os.Add ("probeError", GS::UniString (probes.lastError, CC_UTF8));
            os.Add ("depthInjections", (GS::Int32) probes.depthInjections);
            os.Add ("depthNoView", (GS::Int32) probes.depthNoView);
            os.Add ("depthViewChanged", (GS::Int32) probes.depthViewChanged);
            os.Add ("presentDepthDraws", (GS::Int32) probes.presentDepthDraws);
            os.Add ("presentDepthNoView", (GS::Int32) probes.presentDepthNoView);
            os.Add ("lastDepthView", GS::UniString (std::to_string (probes.lastDepthView).c_str (), CC_UTF8));
        }
        {
            inj::OccurrenceStats occurrences[inj::kOccurrenceCapacity];
            const size_t count = inj::CopyOccurrences (occurrences, inj::kOccurrenceCapacity);
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
                row.Add ("meanSpreadPixels", (double) occurrences[i].meanSpreadPixels);
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
        const av::dxgi::contextstate::DrawCameraCounts cameraCounts = av::dxgi::contextstate::GetDrawCameraCounts ();
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
        os.Add ("invalidNoSnapshot", (GS::Int32) stats.invalidNoSnapshot);
        os.Add ("invalidNoDrawThisGeneration", (GS::Int32) stats.invalidNoDrawThisGeneration);
        os.Add ("invalidGenerationAdvanced", (GS::Int32) stats.invalidGenerationAdvanced);
        os.Add ("invalidGenerationMismatch", (GS::Int32) stats.invalidGenerationMismatch);
        os.Add ("snapshotsTaken", (GS::Int32) stats.snapshotsTaken);
        os.Add ("qualifyingCameraDraws", (GS::Int32) stats.qualifyingCameraDraws);
        os.Add ("viewCopies", (GS::Int32) stats.viewCopies);
        os.Add ("projectionCopies", (GS::Int32) stats.projectionCopies);
        os.Add ("snapshotValid", stats.snapshotValid);
        os.Add ("drawsWithBothInModelPass", (GS::Int32) cameraCounts.withBothInModelPass);

        // Why a target departure was or was not taken as scene completion.
        const av::dxgi::renderstate::DepartureStats departures = av::dxgi::renderstate::GetDepartureStats ();
        os.Add ("departuresSeen", (GS::Int32) departures.departuresSeen);
        os.Add ("acceptedAsScene", (GS::Int32) departures.acceptedAsScene);
        os.Add ("rejectedTooFewDraws", (GS::Int32) departures.rejectedTooFewDraws);
        os.Add ("rejectedAlreadyDone", (GS::Int32) departures.rejectedAlreadyDone);
        os.Add ("drawThreshold", (GS::Int32) departures.drawThreshold);
        os.Add ("busiestPassDraws", (GS::Int32) departures.busiestPassDraws);

        // ⚠️ WHAT PHASE 1 LEARNED. Nothing is injected until `learned` is true,
        // and it only becomes true when consecutive frames agree on the same
        // colour resource, depth view and viewport for a camera-bearing pass.
        const av::dxgi::renderstate::SceneSignature signature = av::dxgi::renderstate::GetSceneSignature ();
        os.Add ("signatureLearned", signature.learned);
        os.Add ("signatureStableFrames", (GS::Int32) signature.stableFrames);
        os.Add ("signatureFramesWatched", (GS::Int32) signature.framesWatched);
        os.Add ("signatureCandidates", (GS::Int32) signature.candidatesThisFrame);
        os.Add ("signatureDraws", (GS::Int32) signature.draws);
        os.Add ("signatureViewportWidth", (double) signature.viewportWidth);
        os.Add ("signatureViewportHeight", (double) signature.viewportHeight);
        os.Add ("sceneConsumers", (GS::Int32) av::dxgi::renderstate::SceneConsumedCount ());
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
    GS::String GetName () const override
    {
        return "ViewerInjectionOracle";
    }

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
            entry.Add ("selectedGroupSnapshotGeneration", (GS::Int32) row.selectedGroupSnapshotGeneration);
            entry.Add ("sourceDrawSequence", (GS::Int32) row.sourceDrawSequence);
            entry.Add ("sourceModelGeneration", (GS::Int32) row.sourceModelGeneration);
            // As strings: a buffer pointer does not fit an Int32, and these are
            // only ever compared for equality and read by eye.
            entry.Add ("viewBuffer", GS::UniString (std::to_string (row.viewBuffer).c_str (), CC_UTF8));
            entry.Add ("viewFirstConstant", (GS::Int32) row.viewFirstConstant);
            entry.Add ("viewNumConstants", (GS::Int32) row.viewNumConstants);
            entry.Add ("projectionBuffer", GS::UniString (std::to_string (row.projectionBuffer).c_str (), CC_UTF8));
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
    GS::String GetName () const override
    {
        return "ViewerCameraCensus";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        namespace cen = av::dxgi::census;

        if (params.Contains ("x") || params.Contains ("y") || params.Contains ("z")) {
            double x = 0.0, y = 0.0, z = 0.0, size = 1.0;
            params.Get ("x", x);
            params.Get ("y", y);
            params.Get ("z", z);
            params.Get ("sizeMetres", size);
            cen::SetAnchor (float (x), float (y), float (z), float (size));
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
            av::dxgi::injection::SetCameraSource (av::dxgi::injection::CameraSource::Learner);
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
                // ⚠️ THE SOURCE IS NO LONGER SET HERE. `SelectCandidate`
                // owns every consequence of choosing a candidate, including this
                // one; setting it from a caller is what let the automatic path
                // implement half the transaction. One implementation, two callers.
                selected = cen::SelectCandidate ();
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
            row.Add ("vertexShader", GS::UniString (std::to_string (group.vertexShader).c_str (), CC_UTF8));
            row.Add ("renderTarget", GS::UniString (std::to_string (group.renderTarget).c_str (), CC_UTF8));
            row.Add ("depthStencil", GS::UniString (std::to_string (group.depthStencil).c_str (), CC_UTF8));
            row.Add ("viewportX", (double) group.viewportX);
            row.Add ("viewportY", (double) group.viewportY);
            row.Add ("viewportWidth", (double) group.viewportWidth);
            row.Add ("viewportHeight", (double) group.viewportHeight);
            row.Add ("viewBuffer", GS::UniString (std::to_string (group.viewBuffer).c_str (), CC_UTF8));
            row.Add ("viewFirstConstant", (GS::Int32) group.viewFirstConstant);
            row.Add ("viewNumConstants", (GS::Int32) group.viewNumConstants);
            row.Add ("projectionBuffer", GS::UniString (std::to_string (group.projectionBuffer).c_str (), CC_UTF8));
            row.Add ("projectionFirstConstant", (GS::Int32) group.projectionFirstConstant);
            row.Add ("projectionNumConstants", (GS::Int32) group.projectionNumConstants);
            row.Add ("b0Bound", group.b0Bound);
            row.Add ("b0Buffer", GS::UniString (std::to_string (group.b0Buffer).c_str (), CC_UTF8));
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
            row.Add ("occurrenceIndex", (GS::Int32) group.occurrenceIndex);
            row.Add ("anchorInside", (GS::Int32) group.anchorInside);
            row.Add ("trianglesFinite", (GS::Int32) group.trianglesFinite);
            row.Add ("medianAreaPixels", (double) group.medianAreaPixels);
            row.Add ("medianMaxEdgePixels", (double) group.medianMaxEdgePixels);
            row.Add ("firstPresent", (GS::Int32) group.firstPresent);
            row.Add ("lastPresent", (GS::Int32) group.lastPresent);
            row.Add ("drawKindMask", (GS::Int32) group.drawKindMask);
            row.Add ("lastIndexCount", (GS::Int32) group.lastIndexCount);
            row.Add ("samplesScored", (GS::Int32) group.samplesScored);
            row.Add ("winningVariant", (GS::Int32) group.winningVariant);
            row.Add ("winningVariantValid", (GS::Int32) group.winningVariantValid);
            row.Add ("medianCentreError", (double) group.medianCentreError);
            row.Add ("meanCentreError", (double) group.meanCentreError);
            row.Add ("meanSpreadPixels", (double) group.meanSpreadPixels);
            row.Add ("worstCentreError", (double) group.worstCentreError);
            rows.Push (row);
        }

        const cen::Selection selection = cen::GetSelection ();
        GS::ObjectState chosen;
        chosen.Add ("valid", selection.valid);
        chosen.Add ("groupId", (GS::Int32) selection.groupId);
        chosen.Add ("occurrenceIndex", (GS::Int32) selection.occurrenceIndex);
        chosen.Add ("medianAreaPixels", (double) selection.medianAreaPixels);
        chosen.Add ("medianMaxEdgePixels", (double) selection.medianMaxEdgePixels);
        chosen.Add ("vertexShader", GS::UniString (std::to_string (selection.vertexShader).c_str (), CC_UTF8));
        chosen.Add ("renderTarget", GS::UniString (std::to_string (selection.renderTarget).c_str (), CC_UTF8));
        chosen.Add ("depthStencil", GS::UniString (std::to_string (selection.depthStencil).c_str (), CC_UTF8));
        chosen.Add ("viewportWidth", (double) selection.viewportWidth);
        chosen.Add ("viewportHeight", (double) selection.viewportHeight);
        chosen.Add ("variant", (GS::Int32) selection.variant);
        chosen.Add ("samples", (GS::Int32) selection.samples);
        chosen.Add ("modelCoverage", (double) selection.modelCoverage);
        chosen.Add ("insideClip", (double) selection.insideClip);
        chosen.Add ("medianCentreError", (double) selection.medianCentreError);
        chosen.Add ("snapshotsTaken", (GS::Int32) selection.snapshotsTaken);

        // ⚠️ THE IDENTITY, BESIDE THE BINDING. `selection` above is
        // this session's pointers; this is what the camera IS, and it is the part
        // a later phase can find again after Archicad rebuilds its views.
        const cen::Fingerprint print = cen::GetFingerprint ();
        GS::ObjectState fingerprint;
        fingerprint.Add ("valid", print.valid);
        fingerprint.Add ("occurrenceIndex", (GS::Int32) print.occurrenceIndex);
        fingerprint.Add ("viewportX", (double) print.viewportX);
        fingerprint.Add ("viewportY", (double) print.viewportY);
        fingerprint.Add ("viewportWidth", (double) print.viewportWidth);
        fingerprint.Add ("viewportHeight", (double) print.viewportHeight);
        fingerprint.Add ("drawKindMask", (GS::Int32) print.drawKindMask);
        fingerprint.Add ("indexCount", (GS::Int32) print.indexCount);
        fingerprint.Add ("viewNumConstants", (GS::Int32) print.viewNumConstants);
        fingerprint.Add ("projectionNumConstants", (GS::Int32) print.projectionNumConstants);
        fingerprint.Add ("depthPresent", print.depthPresent);
        fingerprint.Add ("renderTargetWidth", (GS::Int32) print.renderTargetWidth);
        fingerprint.Add ("renderTargetHeight", (GS::Int32) print.renderTargetHeight);
        fingerprint.Add ("renderTargetFormat", (GS::Int32) print.renderTargetFormat);
        fingerprint.Add ("renderTargetSamples", (GS::Int32) print.renderTargetSamples);
        fingerprint.Add ("depthWidth", (GS::Int32) print.depthWidth);
        fingerprint.Add ("depthHeight", (GS::Int32) print.depthHeight);
        fingerprint.Add ("depthFormat", (GS::Int32) print.depthFormat);
        fingerprint.Add ("depthSamples", (GS::Int32) print.depthSamples);
        fingerprint.Add ("variant", (GS::Int32) print.variant);
        fingerprint.Add ("drawOrdinalFirst", (GS::Int32) print.drawOrdinalFirst);
        fingerprint.Add ("drawOrdinalLast", (GS::Int32) print.drawOrdinalLast);
        const cen::Eligibility gate = cen::GetEligibility ();
        GS::ObjectState gateState;
        gateState.Add ("minSamples", (GS::Int32) gate.minSamples);
        gateState.Add ("minModelCoverage", (double) gate.minModelCoverage);
        gateState.Add ("minInsideClip", (double) gate.minInsideClip);
        gateState.Add ("maxMedianCentreError", (double) gate.maxMedianCentreError);
        gateState.Add ("minFiniteTriangles", (double) gate.minFiniteTriangles);
        gateState.Add ("minMedianAreaPixels", (double) gate.minMedianAreaPixels);
        gateState.Add ("minMedianMaxEdgePixels", (double) gate.minMedianMaxEdgePixels);

        const cen::Stats stats = cen::GetStats ();
        GS::ObjectState os;
        os.Add ("fingerprint", fingerprint);
        os.Add ("selection", chosen);
        os.Add ("selected", selected);
        os.Add ("eligibility", gateState);
        os.Add ("modelFramesSeen", (GS::Int32) stats.modelFramesSeen);
        os.Add ("cameraSource", GS::UniString (av::dxgi::injection::GetCameraSource () ==
                                                       av::dxgi::injection::CameraSource::CensusSelectedGroup
                                                   ? "CensusSelectedGroup"
                                                   : "Learner",
                                               CC_UTF8));
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
        os.Add ("fingerprintValid", stats.fingerprintValid);
        os.Add ("cameraLifecycle", GS::UniString (stats.lifecycle, CC_UTF8));
        os.Add ("selectionMatches", (GS::Int32) stats.selectionMatches);
        os.Add ("logicalMatches", (GS::Int32) stats.logicalMatches);
        os.Add ("rebinds", (GS::Int32) stats.rebinds);
        os.Add ("rebindsRefused", (GS::Int32) stats.rebindsRefused);

        // ⚠️ WHICH TERM OF THE FINGERPRINT REFUSED. `logicalMatches
        // == 0` is the one failure this whole mechanism exists to prevent and
        // also the least informative thing it could report; `soleMiss` names the
        // single term that disagreed on draws which matched every other one.
        {
            const cen::FingerprintDiagnosis diag = cen::GetFingerprintDiagnosis ();
            const char* const termNames[cen::kFingerprintTermCount] = { "occurrence",     "viewport",
                                                                        "draw kind",      "index count",
                                                                        "camera windows", "depth presence",
                                                                        "colour target",  "depth target" };
            GS::Array<GS::ObjectState> terms;
            for (size_t i = 0; i < cen::kFingerprintTermCount; ++i) {
                GS::ObjectState term;
                term.Add ("name", GS::UniString (termNames[i], CC_UTF8));
                term.Add ("missed", (GS::Int32) diag.missed[i]);
                term.Add ("soleMiss", (GS::Int32) diag.soleMiss[i]);
                term.Add ("sampled", diag.sampled[i]);
                term.Add ("observed0", (GS::Int32) diag.observed[i][0]);
                term.Add ("observed1", (GS::Int32) diag.observed[i][1]);
                term.Add ("observed2", (GS::Int32) diag.observed[i][2]);
                term.Add ("observed3", (GS::Int32) diag.observed[i][3]);
                terms.Push (term);
            }
            os.Add ("fingerprintTerms", terms);
            os.Add ("fingerprintEvaluated", (GS::Int32) diag.evaluated);
        }
        return os;
    }
};

const NativeCommandRegistration kViewerInjectionCommandRegistrations[] = {
    { "ViewerInjectTriangle", &MakeRegisteredNativeCommand<ViewerInjectTriangleCommand>, false,
      R"json({"type":"object","properties":{"enabled":{"type":"boolean"},"point":{"type":"string","enum":["present","scenepass","both"]},"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"sizeMetres":{"type":"number","exclusiveMinimum":0,"maximum":1000},"occurrenceReset":{"type":"boolean"},"occurrenceSelect":{"type":"boolean"},"probeReset":{"type":"boolean"},"depth":{"type":"string","enum":["off","scene","private"]},"mesh":{"type":"boolean"},"meshAnimate":{"type":"boolean"},"meshStandIns":{"type":"boolean"},"proofPrimitives":{"type":"boolean"},"hostWireframe":{"type":"boolean"},"hostHeatmap":{"type":"boolean"},"depthSource":{"type":"string","enum":["opaque","transparent","present"]},"provenanceReset":{"type":"boolean"},"checkpoints":{"type":"boolean"},"checkpointInterval":{"type":"integer","minimum":1,"maximum":240},"checkpointReset":{"type":"boolean"},"frontOffsetZ":{"type":"number"},"behindOffsetZ":{"type":"number"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"enabled":{"type":"boolean"},"point":{"type":"string"},"drawsTotal":{"type":"integer"},"drawsWithView":{"type":"integer"},"drawsWithProjection":{"type":"integer"},"drawsWithBothCamera":{"type":"integer"},"initialised":{"type":"boolean"},"injected":{"type":"integer"},"skippedNoSceneDraw":{"type":"integer"},"skippedNoCamera":{"type":"integer"},"skippedNotReady":{"type":"integer"},"skippedPassMismatch":{"type":"integer"},"skippedWindowSize":{"type":"integer"},"skippedReentrant":{"type":"integer"},"skippedStaleCamera":{"type":"integer"},"backBufferFailures":{"type":"integer"},"newScene":{"type":"integer"},"repeatScene":{"type":"integer"},"invalidScene":{"type":"integer"},"invalidNoSnapshot":{"type":"integer"},"invalidNoDrawThisGeneration":{"type":"integer"},"invalidGenerationAdvanced":{"type":"integer"},"invalidGenerationMismatch":{"type":"integer"},"snapshotsTaken":{"type":"integer"},"qualifyingCameraDraws":{"type":"integer"},"cameraSource":{"type":"string"},"selectedCameraValid":{"type":"boolean"},"armState":{"type":"string"},"fingerprintValid":{"type":"boolean"},"cameraLifecycle":{"type":"string"},"censusSelectionMatches":{"type":"integer"},"censusLogicalMatches":{"type":"integer"},"censusRebinds":{"type":"integer"},"censusRebindsRefused":{"type":"integer"},"selectedGroupId":{"type":"integer"},"selectedGroupDraws":{"type":"integer"},"selectedGroupSnapshots":{"type":"integer"},"selectedSnapshotGeneration":{"type":"integer"},"occurrenceSelected":{"type":"boolean"},"occurrenceLocked":{"type":"boolean"},"lockedOccurrence":{"type":"integer"},"occurrenceDraws":{"type":"integer"},"authoritativeSnapshots":{"type":"integer"},"occurrenceModelFrames":{"type":"integer"},"depthMode":{"type":"string"},"depthPreparations":{"type":"integer"},"depthSourceViewAcquired":{"type":"integer"},"depthSourceResourceAcquired":{"type":"integer"},"depthSourceDescAccepted":{"type":"integer"},"depthTextureCreated":{"type":"integer"},"depthViewCreated":{"type":"integer"},"depthCopyIssued":{"type":"integer"},"depthViewBound":{"type":"integer"},"depthCopies":{"type":"integer"},"depthNoSceneView":{"type":"integer"},"depthCopyRefused":{"type":"integer"},"depthRebuilds":{"type":"integer"},"depthWidth":{"type":"integer"},"depthHeight":{"type":"integer"},"depthFormat":{"type":"integer"},"depthSamples":{"type":"integer"},"depthPrivateReady":{"type":"boolean"},"depthError":{"type":"string"},"mesh":{"type":"boolean"},"meshAnimate":{"type":"boolean"},"meshCreated":{"type":"boolean"},"meshBuilds":{"type":"integer"},"meshUploads":{"type":"integer"},"meshUploadFailures":{"type":"integer"},"meshDraws":{"type":"integer"},"meshVertices":{"type":"integer"},"meshIndices":{"type":"integer"},"meshTriangles":{"type":"integer"},"meshPhase":{"type":"integer"},"meshError":{"type":"string"},"meshStandIns":{"type":"boolean"},"proofPrimitives":{"type":"boolean"},"hostWireframe":{"type":"boolean"},"hostHeatmap":{"type":"boolean"},"hostOverlayReady":{"type":"boolean"},"hostWireframeDraws":{"type":"integer"},"hostHeatmapDraws":{"type":"integer"},"hostHiddenPassDraws":{"type":"integer"},"hostCulledPasses":{"type":"integer"},"hostOverlayLines":{"type":"integer"},"hostOverlaySkippedNoEdges":{"type":"integer"},"hostSkippedNoDepthTarget":{"type":"integer"},"hostOverlayTriangles":{"type":"integer"},"hostOverlaySkippedNoGeometry":{"type":"integer"},"hostOverlaySkippedNoCamera":{"type":"integer"},"hostOverlayError":{"type":"string"},"hostReady":{"type":"boolean"},"hostHaveSnapshot":{"type":"boolean"},"hostPublishedTriangles":{"type":"integer"},"hostPublishSucceeded":{"type":"integer"},"hostPublishFailure":{"type":"string"},"hostWindingKnown":{"type":"boolean"},"hostFrontCounterClockwise":{"type":"boolean"},"hostSignedVolume":{"type":"number"},"hostPublishedLines":{"type":"integer"},"hostEdgesConsidered":{"type":"integer"},"hostUploads":{"type":"integer"},"hostRenders":{"type":"integer"},"hostVertices":{"type":"integer"},"hostTriangles":{"type":"integer"},"hostPendingVertices":{"type":"integer"},"hostElementsReceived":{"type":"integer"},"hostOpaqueTriangles":{"type":"integer"},"hostTransparentTriangles":{"type":"integer"},"hostBatchBegins":{"type":"integer"},"hostBatchEnds":{"type":"integer"},"hostSkippedNoCamera":{"type":"integer"},"hostSkippedNoGeometry":{"type":"integer"},"hostDroppedOverCapacity":{"type":"integer"},"hostError":{"type":"string"},"depthSource":{"type":"string"},"provSceneDraws":{"type":"integer"},"provOpaqueDraws":{"type":"integer"},"provBlendedDraws":{"type":"integer"},"provNoDepthWriteDraws":{"type":"integer"},"provDepthTestOffDraws":{"type":"integer"},"provOpaqueAfterTransition":{"type":"integer"},"provTransitions":{"type":"integer"},"provFramesWithoutOpaque":{"type":"integer"},"provFramesWithoutTransition":{"type":"integer"},"provCapturedAfterOpaque":{"type":"integer"},"provCapturedAfterTransparent":{"type":"integer"},"provCapturedAtPresent":{"type":"integer"},"provServedStale":{"type":"integer"},"provLastOpaqueDrawIndex":{"type":"integer"},"provLastSceneDrawIndex":{"type":"integer"},"checkpoints":{"type":"boolean"},"checkpointReady":{"type":"boolean"},"checkpointInterval":{"type":"integer"},"checkpointFrames":{"type":"integer"},"checkpointRounds":{"type":"integer"},"checkpointDrawsLastFrame":{"type":"integer"},"checkpointSkippedNoCamera":{"type":"integer"},"checkpointSkippedNoPipeline":{"type":"integer"},"checkpointOverflowed":{"type":"integer"},"checkpointQueriesIssued":{"type":"integer"},"checkpointQueriesResolved":{"type":"integer"},"checkpointUsed":{"type":"integer"},"checkpointError":{"type":"string"},"checkpointRows":{"type":"array","items":{"type":"object","properties":{"checkpoint":{"type":"integer"},"drawOrdinal":{"type":"integer"},"drawKind":{"type":"integer"},"indexCount":{"type":"integer"},"topology":{"type":"integer"},"depthWrite":{"type":"boolean"},"depthTest":{"type":"boolean"},"depthFunc":{"type":"integer"},"blendEnable":{"type":"boolean"},"srcBlend":{"type":"integer"},"destBlend":{"type":"integer"},"vertexShader":{"type":"string"},"pixelShader":{"type":"string"},"blendState":{"type":"string"},"depthStencilState":{"type":"string"},"rounds":{"type":"integer"},"frontSurvived":{"type":"integer"},"behindSurvived":{"type":"integer"},"ghostSurvived":{"type":"integer"},"frontSamples":{"type":"integer"},"behindSamples":{"type":"integer"},"ghostSamples":{"type":"integer"}},"additionalProperties":false,"required":["checkpoint"]}},"probesReady":{"type":"boolean"},"cameraVsHash":{"type":"string"},"probeVsHash":{"type":"string"},"rasterVsHash":{"type":"string"},"probeError":{"type":"string"},"depthInjections":{"type":"integer"},"depthNoView":{"type":"integer"},"depthViewChanged":{"type":"integer"},"presentDepthDraws":{"type":"integer"},"presentDepthNoView":{"type":"integer"},"lastDepthView":{"type":"string"},"probes":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"draws":{"type":"integer"},"queriesIssued":{"type":"integer"},"queriesResolved":{"type":"integer"},"drawsWithSamples":{"type":"integer"},"totalSamples":{"type":"integer"}},"additionalProperties":false,"required":["name","draws"]}},"occurrences":{"type":"array","items":{"type":"object","properties":{"index":{"type":"integer"},"draws":{"type":"integer"},"modelFrames":{"type":"integer"},"samples":{"type":"integer"},"insideClip":{"type":"integer"},"medianCentreError":{"type":"number"},"meanCentreError":{"type":"number"},"worstCentreError":{"type":"number"},"meanSpreadPixels":{"type":"number"},"viewportWidth":{"type":"number"},"viewportHeight":{"type":"number"},"lastDrawSequence":{"type":"integer"}},"additionalProperties":false,"required":["index","draws","samples"]}},"injectedPresent":{"type":"integer"},"injectedScenePass":{"type":"integer"},"shaderInterpretation":{"type":"integer"},"expectedInterpretation":{"type":"integer"},"interpretationAgrees":{"type":"boolean"},"skippedInterpretation":{"type":"integer"},"viewCopies":{"type":"integer"},"projectionCopies":{"type":"integer"},"snapshotValid":{"type":"boolean"},"drawsWithBothInModelPass":{"type":"integer"},"departuresSeen":{"type":"integer"},"acceptedAsScene":{"type":"integer"},"rejectedTooFewDraws":{"type":"integer"},"rejectedAlreadyDone":{"type":"integer"},"drawThreshold":{"type":"integer"},"busiestPassDraws":{"type":"integer"},"signatureLearned":{"type":"boolean"},"signatureStableFrames":{"type":"integer"},"signatureFramesWatched":{"type":"integer"},"signatureCandidates":{"type":"integer"},"signatureDraws":{"type":"integer"},"signatureViewportWidth":{"type":"number"},"signatureViewportHeight":{"type":"number"},"sceneConsumers":{"type":"integer"},"lastError":{"type":"string"}},"additionalProperties":false,"required":["enabled","injected"]})json" },
    { "ViewerInjectionOracle",
      &MakeRegisteredNativeCommand<ViewerInjectionOracleCommand>, false, R"json({"type":"object","properties":{"limit":{"type":"integer","minimum":1,"maximum":64},"reset":{"type":"boolean"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"ready":{"type":"boolean"},"rows":{"type":"array","items":{"type":"object","properties":{"present":{"type":"integer"},"modelSceneGeneration":{"type":"integer"},"snapshotSequence":{"type":"integer"},"snapshotDrawSequence":{"type":"integer"},"state":{"type":"integer"},"cameraSource":{"type":"integer"},"selectedGroupId":{"type":"integer"},"selectedGroupSnapshotGeneration":{"type":"integer"},"sourceDrawSequence":{"type":"integer"},"sourceModelGeneration":{"type":"integer"},"viewBuffer":{"type":"string"},"viewFirstConstant":{"type":"integer"},"viewNumConstants":{"type":"integer"},"projectionBuffer":{"type":"string"},"projectionFirstConstant":{"type":"integer"},"projectionNumConstants":{"type":"integer"},"matricesRead":{"type":"boolean"},"clipW":{"type":"number"},"ndcX":{"type":"number"},"ndcY":{"type":"number"},"ndcZ":{"type":"number"},"pixelX":{"type":"number"},"pixelY":{"type":"number"},"insideClipVolume":{"type":"boolean"},"viewportX":{"type":"number"},"viewportY":{"type":"number"},"viewportWidth":{"type":"number"},"viewportHeight":{"type":"number"},"bestVariant":{"type":"integer"},"bestVariantPixelX":{"type":"number"},"bestVariantPixelY":{"type":"number"},"bestVariantCentreError":{"type":"number"},"drawIssued":{"type":"boolean"},"samplesKnown":{"type":"boolean"},"samplesPassed":{"type":"integer"}},"additionalProperties":false,"required":["present","state","matricesRead"]}},"rowsCompleted":{"type":"integer"},"rowsDroppedUnresolved":{"type":"integer"},"readbacksAttempted":{"type":"integer"},"readbacksServed":{"type":"integer"},"readbacksStillDrawing":{"type":"integer"},"readbacksStale":{"type":"integer"},"queriesIssued":{"type":"integer"},"queriesResolved":{"type":"integer"},"queriesUnavailable":{"type":"integer"},"snapshotsStaged":{"type":"integer"},"rowsQualified":{"type":"integer"},"rowsRejectedNotNew":{"type":"integer"},"rowsRejectedStill":{"type":"integer"},"variants":{"type":"array","items":{"type":"object","properties":{"variant":{"type":"integer"},"rowsTested":{"type":"integer"},"rowsInsideClip":{"type":"integer"},"rowsWon":{"type":"integer"},"medianCentreError":{"type":"number"},"meanCentreError":{"type":"number"},"worstCentreError":{"type":"number"}},"additionalProperties":false,"required":["variant","rowsTested"]}}},"additionalProperties":false,"required":["ready","rows"]})json" },
    { "ViewerCameraCensus", &MakeRegisteredNativeCommand<ViewerCameraCensusCommand>, false,
      R"json({"type":"object","properties":{"enabled":{"type":"boolean"},"reset":{"type":"boolean"},"limit":{"type":"integer","minimum":1,"maximum":48},"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"sizeMetres":{"type":"number","exclusiveMinimum":0,"maximum":1000},"select":{"type":"boolean"},"clearSelection":{"type":"boolean"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"enabled":{"type":"boolean"},"ready":{"type":"boolean"},"groups":{"type":"array","items":{"type":"object","properties":{"vertexShader":{"type":"string"},"renderTarget":{"type":"string"},"depthStencil":{"type":"string"},"viewportX":{"type":"number"},"viewportY":{"type":"number"},"viewportWidth":{"type":"number"},"viewportHeight":{"type":"number"},"viewBuffer":{"type":"string"},"viewFirstConstant":{"type":"integer"},"viewNumConstants":{"type":"integer"},"projectionBuffer":{"type":"string"},"projectionFirstConstant":{"type":"integer"},"projectionNumConstants":{"type":"integer"},"b0Bound":{"type":"boolean"},"b0Buffer":{"type":"string"},"b0FirstConstant":{"type":"integer"},"b0NumConstants":{"type":"integer"},"passGeneration":{"type":"integer"},"targetEpoch":{"type":"integer"},"drawSequenceFirst":{"type":"integer"},"drawSequenceLast":{"type":"integer"},"drawsObserved":{"type":"integer"},"framesObserved":{"type":"integer"},"modelFramesObserved":{"type":"integer"},"groupId":{"type":"integer"},"occurrenceIndex":{"type":"integer"},"anchorInside":{"type":"integer"},"trianglesFinite":{"type":"integer"},"medianAreaPixels":{"type":"number"},"medianMaxEdgePixels":{"type":"number"},"firstPresent":{"type":"integer"},"lastPresent":{"type":"integer"},"drawKindMask":{"type":"integer"},"lastIndexCount":{"type":"integer"},"samplesScored":{"type":"integer"},"winningVariant":{"type":"integer"},"winningVariantValid":{"type":"integer"},"medianCentreError":{"type":"number"},"meanCentreError":{"type":"number"},"worstCentreError":{"type":"number"},"meanSpreadPixels":{"type":"number"}},"additionalProperties":false,"required":["vertexShader","drawsObserved","samplesScored"]}},"drawsSeen":{"type":"integer"},"drawsQualified":{"type":"integer"},"framesSeen":{"type":"integer"},"groupsUsed":{"type":"integer"},"groupsOverflowed":{"type":"integer"},"copiesIssued":{"type":"integer"},"readbacksServed":{"type":"integer"},"readbacksBusy":{"type":"integer"},"modelFramesSeen":{"type":"integer"},"selected":{"type":"boolean"},"fingerprintValid":{"type":"boolean"},"cameraLifecycle":{"type":"string"},"selectionMatches":{"type":"integer"},"logicalMatches":{"type":"integer"},"rebinds":{"type":"integer"},"rebindsRefused":{"type":"integer"},"fingerprintEvaluated":{"type":"integer"},"fingerprintTerms":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"missed":{"type":"integer"},"soleMiss":{"type":"integer"},"sampled":{"type":"boolean"},"observed0":{"type":"integer"},"observed1":{"type":"integer"},"observed2":{"type":"integer"},"observed3":{"type":"integer"}},"additionalProperties":false,"required":["name"]}},"fingerprint":{"type":"object","properties":{"valid":{"type":"boolean"},"occurrenceIndex":{"type":"integer"},"viewportX":{"type":"number"},"viewportY":{"type":"number"},"viewportWidth":{"type":"number"},"viewportHeight":{"type":"number"},"drawKindMask":{"type":"integer"},"indexCount":{"type":"integer"},"viewNumConstants":{"type":"integer"},"projectionNumConstants":{"type":"integer"},"depthPresent":{"type":"boolean"},"renderTargetWidth":{"type":"integer"},"renderTargetHeight":{"type":"integer"},"renderTargetFormat":{"type":"integer"},"renderTargetSamples":{"type":"integer"},"depthWidth":{"type":"integer"},"depthHeight":{"type":"integer"},"depthFormat":{"type":"integer"},"depthSamples":{"type":"integer"},"variant":{"type":"integer"},"drawOrdinalFirst":{"type":"integer"},"drawOrdinalLast":{"type":"integer"}},"additionalProperties":false,"required":["valid"]},"cameraSource":{"type":"string"},"eligibility":{"type":"object","properties":{"minSamples":{"type":"integer"},"minModelCoverage":{"type":"number"},"minInsideClip":{"type":"number"},"maxMedianCentreError":{"type":"number"},"minFiniteTriangles":{"type":"number"},"minMedianAreaPixels":{"type":"number"},"minMedianMaxEdgePixels":{"type":"number"}},"additionalProperties":false},"selection":{"type":"object","properties":{"valid":{"type":"boolean"},"groupId":{"type":"integer"},"occurrenceIndex":{"type":"integer"},"medianAreaPixels":{"type":"number"},"medianMaxEdgePixels":{"type":"number"},"vertexShader":{"type":"string"},"renderTarget":{"type":"string"},"depthStencil":{"type":"string"},"viewportWidth":{"type":"number"},"viewportHeight":{"type":"number"},"variant":{"type":"integer"},"samples":{"type":"integer"},"modelCoverage":{"type":"number"},"insideClip":{"type":"number"},"medianCentreError":{"type":"number"},"snapshotsTaken":{"type":"integer"}},"additionalProperties":false,"required":["valid"]}},"additionalProperties":false,"required":["enabled","groups"]})json" },
};

} // namespace

NativeCommandRegistrations GetViewerInjectionCommandRegistrations ()
{
    return MakeRegistrationView (kViewerInjectionCommandRegistrations);
}

} // namespace geomsrv
