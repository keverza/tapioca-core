#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/ViewerPassProvenanceCommands.hpp"
#include "NativeCommands/ViewerCameraAgreementOutput.hpp"
#include "NativeCommands/ViewerPresentedContentOutput.hpp"

#include "ArchViz/Dxgi/ImageTransferTrace.hpp"
#include "ArchViz/Dxgi/PassProvenance.hpp"
#include "ArchViz/Dxgi/CameraAgreement.hpp"
#include "ArchViz/Dxgi/PresentProfile.hpp"
#include "ArchViz/Dxgi/PresentedContent.hpp"
#include "ArchViz/Dxgi/SceneCameraPairing.hpp"

#include <string>

namespace geomsrv {

namespace av = geomsrv::archviz;

namespace {

const char* ResourceStateName (av::dxgi::passprovenance::ResourceState state)
{
    namespace provenance = av::dxgi::passprovenance;
    return state == provenance::ResourceState::Known       ? "KNOWN"
           : state == provenance::ResourceState::Ambiguous ? "AMBIGUOUS"
                                                           : "UNKNOWN";
}

const char* DrawKindName (av::dxgi::passprovenance::DrawKind kind)
{
    namespace provenance = av::dxgi::passprovenance;
    switch (kind) {
        case provenance::DrawKind::Indexed:
            return "INDEXED";
        case provenance::DrawKind::Direct:
            return "DIRECT";
        case provenance::DrawKind::IndexedInstanced:
            return "INDEXED_INSTANCED";
        case provenance::DrawKind::Instanced:
            return "INSTANCED";
        case provenance::DrawKind::Auto:
            return "AUTO";
        case provenance::DrawKind::IndexedInstancedIndirect:
            return "INDEXED_INSTANCED_INDIRECT";
        case provenance::DrawKind::InstancedIndirect:
            return "INSTANCED_INDIRECT";
        default:
            return "UNKNOWN";
    }
}

const char* SampledLineageName (av::dxgi::passprovenance::SampledLineage lineage)
{
    namespace provenance = av::dxgi::passprovenance;
    return lineage == provenance::SampledLineage::Known                    ? "KNOWN"
           : lineage == provenance::SampledLineage::Ambiguous              ? "AMBIGUOUS"
           : lineage == provenance::SampledLineage::ConflictingKnownPasses ? "CONFLICTING_KNOWN_PASSES"
                                                                           : "NONE";
}

const char* PairingRelationName (av::dxgi::scenecamerapairing::Relation relation)
{
    namespace pairing = av::dxgi::scenecamerapairing;
    return relation == pairing::Relation::Match       ? "MATCH"
           : relation == pairing::Relation::Mismatch  ? "MISMATCH"
           : relation == pairing::Relation::Ambiguous ? "AMBIGUOUS"
                                                       : "UNKNOWN";
}

const char* BackBufferStateName (av::dxgi::scenecamerapairing::BackBufferState state)
{
    namespace pairing = av::dxgi::scenecamerapairing;
    return state == pairing::BackBufferState::Handoff ? "HANDOFF"
           : state == pairing::BackBufferState::Cleared ? "CLEARED"
           : state == pairing::BackBufferState::Mixed   ? "MIXED"
                                                         : "NONE";
}

const char* ImageTransferRoleName (av::dxgi::imagetransfer::Role role)
{
    namespace imagetransfer = av::dxgi::imagetransfer;
    return role == imagetransfer::Role::Present ? "PRESENT" : "CONTEXT";
}

const char* ImageTransferKindName (av::dxgi::imagetransfer::Kind kind)
{
    namespace imagetransfer = av::dxgi::imagetransfer;
    switch (kind) {
        case imagetransfer::Kind::Root:
            return "ROOT";
        case imagetransfer::Kind::NextRoot:
            return "NEXT_ROOT";
        case imagetransfer::Kind::Draw:
            return "DRAW";
        case imagetransfer::Kind::Copy:
            return "COPY";
        case imagetransfer::Kind::PartialCopy:
            return "PARTIAL_COPY";
        case imagetransfer::Kind::Clear:
            return "CLEAR";
        case imagetransfer::Kind::ResourceWrite:
            return "RESOURCE_WRITE";
        case imagetransfer::Kind::UnmodelledWork:
            return "UNMODELLED_WORK";
        case imagetransfer::Kind::PresentBegin:
            return "PRESENT_BEGIN";
        case imagetransfer::Kind::PresentEnd:
            return "PRESENT_END";
        default:
            return "ROOT";
    }
}

const char* ImageTransferCloseReasonName (av::dxgi::imagetransfer::CloseReason reason)
{
    namespace imagetransfer = av::dxgi::imagetransfer;
    return reason == imagetransfer::CloseReason::Presents ? "PRESENTS"
           : reason == imagetransfer::CloseReason::Full   ? "FULL"
           : reason == imagetransfer::CloseReason::Resize ? "RESIZE"
                                                          : "OPEN";
}

class ViewerPassProvenanceCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "ViewerPassProvenance";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        namespace provenance = av::dxgi::passprovenance;
        namespace pairing = av::dxgi::scenecamerapairing;
        namespace imagetransfer = av::dxgi::imagetransfer;
        namespace presentprofile = av::dxgi::presentprofile;
        namespace cameraagreement = av::dxgi::cameraagreement;
        namespace presentedcontent = av::dxgi::presentedcontent;

        bool reset = false;
        bool enabled = false;
        const bool hasEnabled = params.Contains ("enabled");
        if (params.Contains ("reset"))
            params.Get ("reset", reset);
        if (hasEnabled)
            params.Get ("enabled", enabled);
        // The A/B: force the context hook's after-call repair for this window only
        // (false = the once-per-Present repair alone). Absent, the default (on) holds.
        int repairAfterCalls = -1;
        if (params.Contains ("repairAfterCalls")) {
            bool forced = false;
            params.Get ("repairAfterCalls", forced);
            repairAfterCalls = forced ? 1 : 0;
        }
        const bool restoreEnabled = reset && provenance::Enabled () && !hasEnabled;
        bool drained = true;
        if (reset || (hasEnabled && !enabled)) {
            // Disable order: trace+profile (innermost) -> pairing -> provenance,
            // which is the one that actually drains.
            imagetransfer::SetEnabled (false);
            presentprofile::SetEnabled (false);
            cameraagreement::SetEnabled (false);
            presentedcontent::SetEnabled (false);
            pairing::SetEnabled (false);
            drained = provenance::SetEnabled (false);
        }
        if (reset && drained) {
            provenance::Reset ();
            pairing::Reset ();
            imagetransfer::Reset ();
            presentprofile::Reset ();
            cameraagreement::Reset ();
            presentedcontent::Reset ();
        }
        bool transitionSucceeded = drained;
        if (hasEnabled && enabled && drained) {
            // Enable order: provenance -> pairing -> trace+profile (innermost),
            // each gated on the same success condition as the layer it nests inside.
            transitionSucceeded = provenance::SetEnabled (true);
            pairing::SetEnabled (transitionSucceeded);
            imagetransfer::SetEnabled (transitionSucceeded);
            presentprofile::SetEnabled (transitionSucceeded);
            cameraagreement::SetEnabled (transitionSucceeded);
            presentedcontent::SetEnabled (transitionSucceeded, repairAfterCalls);
        }
        else if (restoreEnabled && drained) {
            transitionSucceeded = provenance::SetEnabled (true);
            pairing::SetEnabled (transitionSucceeded);
            imagetransfer::SetEnabled (transitionSucceeded);
            presentprofile::SetEnabled (transitionSucceeded);
            cameraagreement::SetEnabled (transitionSucceeded);
            presentedcontent::SetEnabled (transitionSucceeded);
        }

        GS::Int32 limit = (GS::Int32) provenance::kRowCapacity;
        if (params.Contains ("limit"))
            params.Get ("limit", limit);
        if (limit < 1)
            limit = 1;
        if (limit > (GS::Int32) provenance::kRowCapacity)
            limit = (GS::Int32) provenance::kRowCapacity;

        provenance::Row rows[provenance::kRowCapacity];
        const size_t count = transitionSucceeded ? provenance::CopyRows (rows, size_t (limit)) : 0;
        GS::Array<GS::ObjectState> out;
        for (size_t i = 0; i < count; ++i) {
            const provenance::Row& row = rows[i];
            GS::ObjectState entry;
            entry.Add ("present", GS::UniString (std::to_string (row.present).c_str (), CC_UTF8));
            entry.Add ("imagePass", GS::UniString (std::to_string (row.imagePass).c_str (), CC_UTF8));
            entry.Add ("cameraPass", GS::UniString (std::to_string (row.cameraPass).c_str (), CC_UTF8));
            entry.Add ("delta", GS::UniString (std::to_string (row.delta).c_str (), CC_UTF8));
            entry.Add ("cameraHash", GS::UniString (std::to_string (row.cameraHash).c_str (), CC_UTF8));
            entry.Add ("backBuffer", GS::UniString (std::to_string (row.backBuffer).c_str (), CC_UTF8));
            entry.Add ("resourceState", GS::UniString (ResourceStateName (row.resourceState), CC_UTF8));
            entry.Add ("resourceAmbiguityMask", GS::Int32 (row.resourceAmbiguityMask));
            entry.Add ("presentContextOverlap", row.presentContextOverlap);
            entry.Add ("relation", GS::UniString (row.relation == provenance::Relation::Match       ? "MATCH"
                                                  : row.relation == provenance::Relation::Mismatch  ? "MISMATCH"
                                                  : row.relation == provenance::Relation::Ambiguous ? "AMBIGUOUS"
                                                                                                    : "UNKNOWN",
                                                  CC_UTF8));
            out.Push (entry);
        }

        pairing::Row pairingRows[pairing::kRowCapacity];
        const size_t pairingLimit = size_t (limit) < pairing::kRowCapacity ? size_t (limit) : pairing::kRowCapacity;
        const size_t pairingCount = transitionSucceeded ? pairing::CopyRows (pairingRows, pairingLimit) : 0;
        GS::Array<GS::ObjectState> pairingOut;
        for (size_t i = 0; i < pairingCount; ++i) {
            const pairing::Row& row = pairingRows[i];
            GS::ObjectState entry;
            entry.Add ("provenanceEpoch", GS::UniString (std::to_string (row.provenanceEpoch).c_str (), CC_UTF8));
            entry.Add ("eventSerial", GS::UniString (std::to_string (row.eventSerial).c_str (), CC_UTF8));
            entry.Add ("presentSerial", GS::UniString (std::to_string (row.presentSerial).c_str (), CC_UTF8));
            entry.Add ("imagePass", GS::UniString (std::to_string (row.imagePass).c_str (), CC_UTF8));
            entry.Add ("imageRootEventSerial",
                       GS::UniString (std::to_string (row.imageRootEventSerial).c_str (), CC_UTF8));
            entry.Add ("imageSourceResource",
                       GS::UniString (std::to_string (row.imageSourceResource).c_str (), CC_UTF8));
            entry.Add ("imageModelGeneration",
                       GS::UniString (std::to_string (row.imageModelGeneration).c_str (), CC_UTF8));
            entry.Add ("overlayCameraSerial",
                       GS::UniString (std::to_string (row.overlayCameraSerial).c_str (), CC_UTF8));
            entry.Add ("cameraSourcePass", GS::UniString (std::to_string (row.cameraSourcePass).c_str (), CC_UTF8));
            entry.Add ("cameraSnapshotEventSerial",
                       GS::UniString (std::to_string (row.cameraSnapshotEventSerial).c_str (), CC_UTF8));
            entry.Add ("cameraAdoptEventSerial",
                       GS::UniString (std::to_string (row.cameraAdoptEventSerial).c_str (), CC_UTF8));
            entry.Add ("ambiguityEventSerial",
                       GS::UniString (std::to_string (row.ambiguityEventSerial).c_str (), CC_UTF8));
            entry.Add ("cameraMetadataPass",
                       GS::UniString (std::to_string (row.cameraMetadataPass).c_str (), CC_UTF8));
            entry.Add ("backBuffer", GS::UniString (std::to_string (row.backBuffer).c_str (), CC_UTF8));
            entry.Add ("delta", GS::UniString (std::to_string (row.delta).c_str (), CC_UTF8));
            entry.Add ("relation", GS::UniString (PairingRelationName (row.relation), CC_UTF8));
            entry.Add ("imageOnBackBuffer", row.imageOnBackBuffer);
            entry.Add ("cameraCoherent", row.cameraCoherent);
            entry.Add ("presentContextOverlap", row.presentContextOverlap);
            entry.Add ("handoffSerial", GS::UniString (std::to_string (row.handoffSerial).c_str (), CC_UTF8));
            entry.Add ("imageGeneration", GS::UniString (std::to_string (row.imageGeneration).c_str (), CC_UTF8));
            entry.Add ("imageRooted", row.imageRooted);
            entry.Add ("cameraImageGeneration",
                       GS::UniString (std::to_string (row.cameraImageGeneration).c_str (), CC_UTF8));
            entry.Add ("metadataStale", row.metadataStale);
            entry.Add ("backBufferState", GS::UniString (BackBufferStateName (row.backBufferState), CC_UTF8));
            pairingOut.Push (entry);
        }

        // IMAGE_TRANSFER_TRACE -- fixed bound (kMaxCaptures x kEventsPerCapture),
        // never governed by `limit`: it is a small, hard-capped diagnostic, not a
        // ring the caller can ask to see more or less of.
        imagetransfer::Event imageTransferEvents[imagetransfer::kMaxCaptures * imagetransfer::kEventsPerCapture];
        const size_t imageTransferEventCount =
            transitionSucceeded ? imagetransfer::CopyEvents (imageTransferEvents, imagetransfer::kMaxCaptures *
                                                                                      imagetransfer::kEventsPerCapture)
                                : 0;
        GS::Array<GS::ObjectState> imageTransferEventsOut;
        for (size_t i = 0; i < imageTransferEventCount; ++i) {
            const imagetransfer::Event& event = imageTransferEvents[i];
            GS::ObjectState entry;
            entry.Add ("epoch", GS::UniString (std::to_string (event.epoch).c_str (), CC_UTF8));
            entry.Add ("capture", GS::Int32 (event.capture));
            entry.Add ("orderSerial", GS::UniString (std::to_string (event.orderSerial).c_str (), CC_UTF8));
            entry.Add ("role", GS::UniString (ImageTransferRoleName (event.role), CC_UTF8));
            entry.Add ("kind", GS::UniString (ImageTransferKindName (event.kind), CC_UTF8));
            entry.Add ("drawKind", GS::UniString (DrawKindName (event.drawKind), CC_UTF8));
            entry.Add ("drawCount", GS::UniString (std::to_string (event.drawCount).c_str (), CC_UTF8));
            entry.Add ("drawsSinceRoot", GS::UniString (std::to_string (event.drawsSinceRoot).c_str (), CC_UTF8));
            entry.Add ("scenePass", GS::UniString (std::to_string (event.scenePass).c_str (), CC_UTF8));
            entry.Add ("rtv0", GS::UniString (std::to_string (event.rtv0).c_str (), CC_UTF8));
            entry.Add ("rtvCount", GS::Int32 (event.rtvCount));
            entry.Add ("srv0", GS::UniString (std::to_string (event.srv0).c_str (), CC_UTF8));
            entry.Add ("srv1", GS::UniString (std::to_string (event.srv1).c_str (), CC_UTF8));
            entry.Add ("srv2", GS::UniString (std::to_string (event.srv2).c_str (), CC_UTF8));
            entry.Add ("srv3", GS::UniString (std::to_string (event.srv3).c_str (), CC_UTF8));
            entry.Add ("trackedSrvMask", GS::UniString (std::to_string (event.trackedSrvMask).c_str (), CC_UTF8));
            entry.Add ("trackedSrvHits", GS::Int32 (event.trackedSrvHits));
            entry.Add ("source", GS::UniString (std::to_string (event.source).c_str (), CC_UTF8));
            entry.Add ("destination", GS::UniString (std::to_string (event.destination).c_str (), CC_UTF8));
            entry.Add ("resource", GS::UniString (std::to_string (event.resource).c_str (), CC_UTF8));
            entry.Add ("backBuffer", GS::UniString (std::to_string (event.backBuffer).c_str (), CC_UTF8));
            entry.Add ("writesBackBuffer", event.writesBackBuffer);
            entry.Add ("readsTracked", event.readsTracked);
            entry.Add ("readsRoot", event.readsRoot);
            entry.Add ("trackedResource", GS::UniString (std::to_string (event.trackedResource).c_str (), CC_UTF8));
            entry.Add ("trackedAdded", GS::Int32 (event.trackedAdded));
            entry.Add ("parentResource", GS::UniString (std::to_string (event.parentResource).c_str (), CC_UTF8));
            entry.Add ("succeeded", event.succeeded);
            imageTransferEventsOut.Push (entry);
        }

        imagetransfer::Capture imageTransferCaptures[imagetransfer::kMaxCaptures];
        const size_t imageTransferCaptureCount =
            transitionSucceeded ? imagetransfer::CopyCaptures (imageTransferCaptures, imagetransfer::kMaxCaptures) : 0;
        GS::Array<GS::ObjectState> imageTransferCapturesOut;
        for (size_t i = 0; i < imageTransferCaptureCount; ++i) {
            const imagetransfer::Capture& capture = imageTransferCaptures[i];
            GS::ObjectState entry;
            entry.Add ("capture", GS::Int32 (capture.capture));
            entry.Add ("rootPass", GS::UniString (std::to_string (capture.rootPass).c_str (), CC_UTF8));
            entry.Add ("rootResource", GS::UniString (std::to_string (capture.rootResource).c_str (), CC_UTF8));
            entry.Add ("rootEventSerial", GS::UniString (std::to_string (capture.rootEventSerial).c_str (), CC_UTF8));
            entry.Add ("openOrderSerial", GS::UniString (std::to_string (capture.openOrderSerial).c_str (), CC_UTF8));
            entry.Add ("closeOrderSerial", GS::UniString (std::to_string (capture.closeOrderSerial).c_str (), CC_UTF8));
            entry.Add ("closeReason", GS::UniString (ImageTransferCloseReasonName (capture.closeReason), CC_UTF8));
            entry.Add ("presentsSeen", GS::Int32 (capture.presentsSeen));
            entry.Add ("drawsSinceRoot", GS::UniString (std::to_string (capture.drawsSinceRoot).c_str (), CC_UTF8));
            entry.Add ("rootWrites", GS::UniString (std::to_string (capture.rootWrites).c_str (), CC_UTF8));
            entry.Add ("unmodelledWork", GS::UniString (std::to_string (capture.unmodelledWork).c_str (), CC_UTF8));
            entry.Add ("trackedCount", GS::Int32 (capture.trackedCount));
            entry.Add ("trackedOverflow", GS::UniString (std::to_string (capture.trackedOverflow).c_str (), CC_UTF8));
            entry.Add ("eventsRecorded", GS::Int32 (capture.eventsRecorded));
            entry.Add ("eventsDropped", GS::UniString (std::to_string (capture.eventsDropped).c_str (), CC_UTF8));
            imageTransferCapturesOut.Push (entry);
        }

        const imagetransfer::Stats imageTransferStats = imagetransfer::GetStats ();
        GS::ObjectState imageTransferStatsOut;
        imageTransferStatsOut.Add ("enabled", imageTransferStats.enabled);
        imageTransferStatsOut.Add ("epoch",
                                   GS::UniString (std::to_string (imageTransferStats.epoch).c_str (), CC_UTF8));
        imageTransferStatsOut.Add ("capturesOpened", GS::Int32 (imageTransferStats.capturesOpened));
        imageTransferStatsOut.Add ("capturesClosed", GS::Int32 (imageTransferStats.capturesClosed));
        imageTransferStatsOut.Add ("capturesTruncated", GS::Int32 (imageTransferStats.capturesTruncated));
        imageTransferStatsOut.Add ("capturesAbortedByResize", GS::Int32 (imageTransferStats.capturesAbortedByResize));
        imageTransferStatsOut.Add ("rootsSeen",
                                   GS::UniString (std::to_string (imageTransferStats.rootsSeen).c_str (), CC_UTF8));
        imageTransferStatsOut.Add (
            "eventsRecorded", GS::UniString (std::to_string (imageTransferStats.eventsRecorded).c_str (), CC_UTF8));
        imageTransferStatsOut.Add ("eventsDropped",
                                   GS::UniString (std::to_string (imageTransferStats.eventsDropped).c_str (), CC_UTF8));
        imageTransferStatsOut.Add (
            "eventsAfterClose", GS::UniString (std::to_string (imageTransferStats.eventsAfterClose).c_str (), CC_UTF8));
        imageTransferStatsOut.Add (
            "unmodelledWork", GS::UniString (std::to_string (imageTransferStats.unmodelledWork).c_str (), CC_UTF8));
        imageTransferStatsOut.Add (
            "trackedOverflow", GS::UniString (std::to_string (imageTransferStats.trackedOverflow).c_str (), CC_UTF8));
        imageTransferStatsOut.Add (
            "lastBackBuffer", GS::UniString (std::to_string (imageTransferStats.lastBackBuffer).c_str (), CC_UTF8));

        GS::ObjectState imageTransferOut;
        imageTransferOut.Add ("stats", imageTransferStatsOut);
        imageTransferOut.Add ("captures", imageTransferCapturesOut);
        imageTransferOut.Add ("events", imageTransferEventsOut);

        const provenance::Stats stats = provenance::GetStats ();
        const pairing::Stats pairingStats = pairing::GetStats ();
        const provenance::FirstKnownToAmbiguousDraw& draw = stats.firstKnownToAmbiguousDraw;
        GS::ObjectState firstDraw;
        firstDraw.Add ("valid", draw.valid);
        firstDraw.Add ("drawsSinceCamera", GS::UniString (std::to_string (draw.drawsSinceCamera).c_str (), CC_UTF8));
        firstDraw.Add ("drawKind", GS::UniString (DrawKindName (draw.drawKind), CC_UTF8));
        firstDraw.Add ("drawCount", GS::UniString (std::to_string (draw.drawCount).c_str (), CC_UTF8));
        firstDraw.Add ("renderTargetSlot", GS::Int32 (draw.renderTargetSlot));
        firstDraw.Add ("targetResource", GS::UniString (std::to_string (draw.targetResource).c_str (), CC_UTF8));
        firstDraw.Add ("targetScenePass", GS::UniString (std::to_string (draw.targetScenePass).c_str (), CC_UTF8));
        firstDraw.Add ("cameraPass", GS::UniString (std::to_string (draw.cameraPass).c_str (), CC_UTF8));
        firstDraw.Add ("sampledLineage", GS::UniString (SampledLineageName (draw.sampledLineage), CC_UTF8));
        firstDraw.Add ("shaderResourceSlot", GS::Int32 (draw.shaderResourceSlot));
        firstDraw.Add ("sampledResource", GS::UniString (std::to_string (draw.sampledResource).c_str (), CC_UTF8));
        firstDraw.Add ("sampledScenePass", GS::UniString (std::to_string (draw.sampledScenePass).c_str (), CC_UTF8));
        firstDraw.Add ("sampledAmbiguityMask", GS::Int32 (draw.sampledAmbiguityMask));
        firstDraw.Add ("resultingAmbiguityMask", GS::Int32 (draw.resultingAmbiguityMask));

        GS::ObjectState os;
        os.Add ("enabled", stats.enabled);
        os.Add ("hookInstalled", stats.hookInstalled);
        os.Add ("contextHookInstalled", stats.contextHookInstalled);
        os.Add ("presentHookInstalled", stats.presentHookInstalled);
        os.Add ("srvHookEnabled", stats.srvHookEnabled);
        os.Add ("contextThreadId", GS::UniString (std::to_string (stats.contextThreadId).c_str (), CC_UTF8));
        os.Add ("presentThreadId", GS::UniString (std::to_string (stats.presentThreadId).c_str (), CC_UTF8));
        os.Add ("presents", GS::UniString (std::to_string (stats.presents).c_str (), CC_UTF8));
        os.Add ("matched", GS::UniString (std::to_string (stats.matched).c_str (), CC_UTF8));
        os.Add ("mismatched", GS::UniString (std::to_string (stats.mismatched).c_str (), CC_UTF8));
        os.Add ("unknown", GS::UniString (std::to_string (stats.unknown).c_str (), CC_UTF8));
        os.Add ("ambiguous", GS::UniString (std::to_string (stats.ambiguous).c_str (), CC_UTF8));
        os.Add ("backBufferFailures", GS::UniString (std::to_string (stats.backBufferFailures).c_str (), CC_UTF8));
        os.Add ("resourceTableOverflows",
                GS::UniString (std::to_string (stats.resourceTableOverflows).c_str (), CC_UTF8));
        os.Add ("renderThreadViolations",
                GS::UniString (std::to_string (stats.renderThreadViolations).c_str (), CC_UTF8));
        os.Add ("unsupportedGpuWork", GS::UniString (std::to_string (stats.unsupportedGpuWork).c_str (), CC_UTF8));
        os.Add ("snapshotDrainTimeouts",
                GS::UniString (std::to_string (stats.snapshotDrainTimeouts).c_str (), CC_UTF8));
        os.Add ("contextHookRepairs", GS::UniString (std::to_string (stats.contextHookRepairs).c_str (), CC_UTF8));
        os.Add ("rowsOverwritten", GS::UniString (std::to_string (stats.rowsOverwritten).c_str (), CC_UTF8));
        os.Add ("nonCameraDrawTransitions",
                GS::UniString (std::to_string (stats.firstAmbiguityTransitions[0]).c_str (), CC_UTF8));
        os.Add ("sampledAmbiguousTransitions",
                GS::UniString (std::to_string (stats.firstAmbiguityTransitions[1]).c_str (), CC_UTF8));
        os.Add ("conflictingSampledPassTransitions",
                GS::UniString (std::to_string (stats.firstAmbiguityTransitions[2]).c_str (), CC_UTF8));
        os.Add ("partialCopyTransitions",
                GS::UniString (std::to_string (stats.firstAmbiguityTransitions[3]).c_str (), CC_UTF8));
        os.Add ("resourceWriteTransitions",
                GS::UniString (std::to_string (stats.firstAmbiguityTransitions[4]).c_str (), CC_UTF8));
        os.Add ("unsupportedGpuWorkTransitions",
                GS::UniString (std::to_string (stats.firstAmbiguityTransitions[5]).c_str (), CC_UTF8));
        os.Add ("secondaryCameraTargetTransitions",
                GS::UniString (std::to_string (stats.firstAmbiguityTransitions[6]).c_str (), CC_UTF8));
        os.Add ("firstKnownToAmbiguousDraw", firstDraw);
        os.Add ("resourceAmbiguousPresents",
                GS::UniString (std::to_string (stats.resourceAmbiguousPresents).c_str (), CC_UTF8));
        os.Add ("presentContextOverlaps",
                GS::UniString (std::to_string (stats.presentContextOverlaps).c_str (), CC_UTF8));
        os.Add ("contextSlotsPatched", GS::Int32 (stats.contextSlotsPatched));
        os.Add ("resourceResetPending", stats.resourceResetPending);
        os.Add ("rows", out);
        os.Add ("pairingEnabled", pairingStats.enabled);
        os.Add ("provenanceEpoch",
                GS::UniString (std::to_string (pairingStats.provenanceEpoch).c_str (), CC_UTF8));
        os.Add ("imageDrawsCommitted",
                GS::UniString (std::to_string (pairingStats.imageDrawsCommitted).c_str (), CC_UTF8));
        os.Add ("cameraSnapshots", GS::UniString (std::to_string (pairingStats.cameraSnapshots).c_str (), CC_UTF8));
        os.Add ("cameraBindings", GS::UniString (std::to_string (pairingStats.cameraBindings).c_str (), CC_UTF8));
        os.Add ("pairingPresents", GS::UniString (std::to_string (pairingStats.presents).c_str (), CC_UTF8));
        os.Add ("pairingMatched", GS::UniString (std::to_string (pairingStats.matched).c_str (), CC_UTF8));
        os.Add ("pairingMismatched", GS::UniString (std::to_string (pairingStats.mismatched).c_str (), CC_UTF8));
        os.Add ("pairingUnknown", GS::UniString (std::to_string (pairingStats.unknown).c_str (), CC_UTF8));
        os.Add ("pairingAmbiguous", GS::UniString (std::to_string (pairingStats.ambiguous).c_str (), CC_UTF8));
        os.Add ("uniqueImagePassesObserved",
                GS::UniString (std::to_string (pairingStats.uniqueImagePassesObserved).c_str (), CC_UTF8));
        os.Add ("uniqueImagePassesClassified",
                GS::UniString (std::to_string (pairingStats.uniqueImagePassesClassified).c_str (), CC_UTF8));
        os.Add ("uniqueMatched", GS::UniString (std::to_string (pairingStats.uniqueMatched).c_str (), CC_UTF8));
        os.Add ("uniqueMismatched", GS::UniString (std::to_string (pairingStats.uniqueMismatched).c_str (), CC_UTF8));
        os.Add ("uniqueUnknown", GS::UniString (std::to_string (pairingStats.uniqueUnknown).c_str (), CC_UTF8));
        os.Add ("uniqueAmbiguous", GS::UniString (std::to_string (pairingStats.uniqueAmbiguous).c_str (), CC_UTF8));
        os.Add ("duplicatePresents",
                GS::UniString (std::to_string (pairingStats.duplicatePresents).c_str (), CC_UTF8));
        os.Add ("pairingRowsOverwritten",
                GS::UniString (std::to_string (pairingStats.rowsOverwritten).c_str (), CC_UTF8));
        os.Add ("handoffs", GS::UniString (std::to_string (pairingStats.handoffs).c_str (), CC_UTF8));
        os.Add ("handoffsUnrooted", GS::UniString (std::to_string (pairingStats.handoffsUnrooted).c_str (), CC_UTF8));
        os.Add ("handoffsSameGeneration",
                GS::UniString (std::to_string (pairingStats.handoffsSameGeneration).c_str (), CC_UTF8));
        os.Add ("generationsWithMultipleRoots",
                GS::UniString (std::to_string (pairingStats.generationsWithMultipleRoots).c_str (), CC_UTF8));
        os.Add ("sceneColourChanges",
                GS::UniString (std::to_string (pairingStats.sceneColourChanges).c_str (), CC_UTF8));
        os.Add ("backBufferClears", GS::UniString (std::to_string (pairingStats.backBufferClears).c_str (), CC_UTF8));
        os.Add ("backBufferOtherWrites",
                GS::UniString (std::to_string (pairingStats.backBufferOtherWrites).c_str (), CC_UTF8));
        os.Add ("presentsWithoutBinding",
                GS::UniString (std::to_string (pairingStats.presentsWithoutBinding).c_str (), CC_UTF8));
        os.Add ("imageGeneration", GS::UniString (std::to_string (pairingStats.imageGeneration).c_str (), CC_UTF8));
        GS::Array<GS::UniString> uniqueDeltaOut;
        GS::Array<GS::UniString> repeatDeltaOut;
        for (size_t i = 0; i < pairing::kDeltaBucketCount; ++i) {
            uniqueDeltaOut.Push (GS::UniString (std::to_string (pairingStats.uniqueDelta[i]).c_str (), CC_UTF8));
            repeatDeltaOut.Push (GS::UniString (std::to_string (pairingStats.repeatDelta[i]).c_str (), CC_UTF8));
        }
        os.Add ("uniqueDelta", uniqueDeltaOut);
        os.Add ("repeatMatched", GS::UniString (std::to_string (pairingStats.repeatMatched).c_str (), CC_UTF8));
        os.Add ("repeatMismatched", GS::UniString (std::to_string (pairingStats.repeatMismatched).c_str (), CC_UTF8));
        os.Add ("repeatUnknown", GS::UniString (std::to_string (pairingStats.repeatUnknown).c_str (), CC_UTF8));
        os.Add ("repeatAmbiguous", GS::UniString (std::to_string (pairingStats.repeatAmbiguous).c_str (), CC_UTF8));
        os.Add ("repeatDelta", repeatDeltaOut);
        os.Add ("pairingRows", pairingOut);
        os.Add ("imageTransfer", imageTransferOut);

        // SWAP CHAIN / PRESENT PROFILE -- coder B's PresentProfile, surfaced here
        // (section C). Present-thread facts refreshed only on identity change.
        const presentprofile::Stats profileStats = presentprofile::GetStats ();
        GS::ObjectState swapChainOut;
        swapChainOut.Add ("known", profileStats.chain.known);
        swapChainOut.Add ("swapChain", GS::UniString (std::to_string (profileStats.chain.swapChain).c_str (), CC_UTF8));
        swapChainOut.Add ("bufferCount", GS::Int32 (profileStats.chain.bufferCount));
        swapChainOut.Add ("swapEffect", GS::Int32 (profileStats.chain.swapEffect));
        swapChainOut.Add ("bufferUsage", GS::Int32 (profileStats.chain.bufferUsage));
        swapChainOut.Add ("flags", GS::Int32 (profileStats.chain.flags));
        swapChainOut.Add ("format", GS::Int32 (profileStats.chain.format));
        swapChainOut.Add ("width", GS::Int32 (profileStats.chain.width));
        swapChainOut.Add ("height", GS::Int32 (profileStats.chain.height));
        swapChainOut.Add ("sampleCount", GS::Int32 (profileStats.chain.sampleCount));
        swapChainOut.Add ("windowed", profileStats.chain.windowed);
        swapChainOut.Add ("desc1Known", profileStats.chain.desc1Known);
        swapChainOut.Add ("scaling", GS::Int32 (profileStats.chain.scaling));
        swapChainOut.Add ("alphaMode", GS::Int32 (profileStats.chain.alphaMode));

        GS::ObjectState presentProfileOut;
        presentProfileOut.Add ("enabled", profileStats.enabled);
        presentProfileOut.Add ("presents", GS::UniString (std::to_string (profileStats.presents).c_str (), CC_UTF8));
        presentProfileOut.Add ("present1Calls",
                               GS::UniString (std::to_string (profileStats.present1Calls).c_str (), CC_UTF8));
        GS::Array<GS::UniString> syncIntervalOut;
        for (uint64_t bucket : profileStats.syncInterval)
            syncIntervalOut.Push (GS::UniString (std::to_string (bucket).c_str (), CC_UTF8));
        presentProfileOut.Add ("syncInterval", syncIntervalOut);
        presentProfileOut.Add ("doNotSequence",
                               GS::UniString (std::to_string (profileStats.doNotSequence).c_str (), CC_UTF8));
        presentProfileOut.Add ("restart", GS::UniString (std::to_string (profileStats.restart).c_str (), CC_UTF8));
        presentProfileOut.Add ("doNotWait", GS::UniString (std::to_string (profileStats.doNotWait).c_str (), CC_UTF8));
        presentProfileOut.Add ("restrictToOutput",
                               GS::UniString (std::to_string (profileStats.restrictToOutput).c_str (), CC_UTF8));
        presentProfileOut.Add ("useDuration",
                               GS::UniString (std::to_string (profileStats.useDuration).c_str (), CC_UTF8));
        presentProfileOut.Add ("allowTearing",
                               GS::UniString (std::to_string (profileStats.allowTearing).c_str (), CC_UTF8));
        presentProfileOut.Add ("otherFlags",
                               GS::UniString (std::to_string (profileStats.otherFlags).c_str (), CC_UTF8));
        presentProfileOut.Add ("flagsSeen",
                               GS::UniString (std::to_string (profileStats.flagsSeen).c_str (), CC_UTF8));
        presentProfileOut.Add ("descQueries",
                               GS::UniString (std::to_string (profileStats.descQueries).c_str (), CC_UTF8));
        presentProfileOut.Add ("descFailures",
                               GS::UniString (std::to_string (profileStats.descFailures).c_str (), CC_UTF8));
        presentProfileOut.Add ("swapChain", swapChainOut);
        os.Add ("presentProfile", presentProfileOut);

        // CAMERA AGREEMENT -- see ViewerCameraAgreementOutput.hpp.
        os.Add ("cameraAgreement", BuildCameraAgreementOutput ());
        // PRESENTED CONTENT -- see ViewerPresentedContentOutput.hpp.
        os.Add ("presentedContent", BuildPresentedContentOutput (pairingStats.provenanceEpoch));
        return os;
    }
};

// ⚠️ THE RESPONSE SCHEMA IS THREE ADJACENT RAW LITERALS. MSVC refuses one literal
// over 16380 bytes (C2026) and this one passed that in Stage 72. The pieces are
// split at top-level properties with nothing but whitespace between them, so the
// compiler, check_cpp.py, schema_check.py and the API generator all read one
// schema. Keep any new piece boundary free of comments for the same reason.
const NativeCommandRegistration kViewerPassProvenanceCommandRegistrations[] = {
    { "ViewerPassProvenance", &MakeRegisteredNativeCommand<ViewerPassProvenanceCommand>, false,
      R"json({
        "type": "object",
        "properties": {
          "enabled": {"type": "boolean"},
          "reset": {"type": "boolean"},
          "repairAfterCalls": {"type": "boolean"},
          "limit": {"type": "integer", "minimum": 1, "maximum": 1024}
        },
        "additionalProperties": false
      })json",
      R"json({
        "type": "object",
        "properties": {
          "enabled": {"type": "boolean"},
          "hookInstalled": {"type": "boolean"},
          "contextHookInstalled": {"type": "boolean"},
          "presentHookInstalled": {"type": "boolean"},
          "srvHookEnabled": {"type": "boolean"},
          "contextThreadId": {"type": "string"},
          "presentThreadId": {"type": "string"},
          "presents": {"type": "string"},
          "matched": {"type": "string"},
          "mismatched": {"type": "string"},
          "unknown": {"type": "string"},
          "ambiguous": {"type": "string"},
          "backBufferFailures": {"type": "string"},
          "resourceTableOverflows": {"type": "string"},
          "renderThreadViolations": {"type": "string"},
          "unsupportedGpuWork": {"type": "string"},
          "snapshotDrainTimeouts": {"type": "string"},
          "contextHookRepairs": {"type": "string"},
          "rowsOverwritten": {"type": "string"},
          "nonCameraDrawTransitions": {"type": "string"},
          "sampledAmbiguousTransitions": {"type": "string"},
          "conflictingSampledPassTransitions": {"type": "string"},
          "partialCopyTransitions": {"type": "string"},
          "resourceWriteTransitions": {"type": "string"},
          "unsupportedGpuWorkTransitions": {"type": "string"},
          "secondaryCameraTargetTransitions": {"type": "string"},
          "firstKnownToAmbiguousDraw": {
            "type": "object",
            "properties": {
              "valid": {"type": "boolean"},
              "drawsSinceCamera": {"type": "string"},
              "drawKind": {"type": "string", "enum": ["INDEXED", "DIRECT", "INDEXED_INSTANCED", "INSTANCED", "AUTO", "INDEXED_INSTANCED_INDIRECT", "INSTANCED_INDIRECT", "UNKNOWN"]},
              "drawCount": {"type": "string"},
              "renderTargetSlot": {"type": "integer", "minimum": -1, "maximum": 7},
              "targetResource": {"type": "string"},
              "targetScenePass": {"type": "string"},
              "cameraPass": {"type": "string"},
              "sampledLineage": {"type": "string", "enum": ["NONE", "KNOWN", "AMBIGUOUS", "CONFLICTING_KNOWN_PASSES"]},
              "shaderResourceSlot": {"type": "integer", "minimum": -1, "maximum": 127},
              "sampledResource": {"type": "string"},
              "sampledScenePass": {"type": "string"},
              "sampledAmbiguityMask": {"type": "integer", "minimum": 0, "maximum": 127},
              "resultingAmbiguityMask": {"type": "integer", "minimum": 0, "maximum": 127}
            },
            "additionalProperties": false,
            "required": ["valid", "drawsSinceCamera", "drawKind", "drawCount", "renderTargetSlot", "targetResource", "targetScenePass", "cameraPass", "sampledLineage", "shaderResourceSlot", "sampledResource", "sampledScenePass", "sampledAmbiguityMask", "resultingAmbiguityMask"]
          },
          "resourceAmbiguousPresents": {"type": "string"},
          "presentContextOverlaps": {"type": "string"},
          "contextSlotsPatched": {"type": "integer"},
          "resourceResetPending": {"type": "boolean"},
          "pairingEnabled": {"type": "boolean"},
          "provenanceEpoch": {"type": "string"},
          "imageDrawsCommitted": {"type": "string"},
          "cameraSnapshots": {"type": "string"},
          "cameraBindings": {"type": "string"},
          "pairingPresents": {"type": "string"},
          "pairingMatched": {"type": "string"},
          "pairingMismatched": {"type": "string"},
          "pairingUnknown": {"type": "string"},
          "pairingAmbiguous": {"type": "string"},
          "uniqueImagePassesObserved": {"type": "string"},
          "uniqueImagePassesClassified": {"type": "string"},
          "uniqueMatched": {"type": "string"},
          "uniqueMismatched": {"type": "string"},
          "uniqueUnknown": {"type": "string"},
          "uniqueAmbiguous": {"type": "string"},
          "duplicatePresents": {"type": "string"},
          "pairingRowsOverwritten": {"type": "string"},
          "handoffs": {"type": "string"},
          "handoffsUnrooted": {"type": "string"},
          "handoffsSameGeneration": {"type": "string"},
          "generationsWithMultipleRoots": {"type": "string"},
          "sceneColourChanges": {"type": "string"},
          "backBufferClears": {"type": "string"},
          "backBufferOtherWrites": {"type": "string"},
          "presentsWithoutBinding": {"type": "string"},
          "imageGeneration": {"type": "string"},
          "uniqueDelta": {"type": "array", "items": {"type": "string"}, "minItems": 7, "maxItems": 7},
          "repeatMatched": {"type": "string"},
          "repeatMismatched": {"type": "string"},
          "repeatUnknown": {"type": "string"},
          "repeatAmbiguous": {"type": "string"},
          "repeatDelta": {"type": "array", "items": {"type": "string"}, "minItems": 7, "maxItems": 7},
          "presentProfile": {
            "type": "object",
            "properties": {
              "enabled": {"type": "boolean"},
              "presents": {"type": "string"},
              "present1Calls": {"type": "string"},
              "syncInterval": {"type": "array", "items": {"type": "string"}, "minItems": 5, "maxItems": 5},
              "doNotSequence": {"type": "string"},
              "restart": {"type": "string"},
              "doNotWait": {"type": "string"},
              "restrictToOutput": {"type": "string"},
              "useDuration": {"type": "string"},
              "allowTearing": {"type": "string"},
              "otherFlags": {"type": "string"},
              "flagsSeen": {"type": "string"},
              "descQueries": {"type": "string"},
              "descFailures": {"type": "string"},
              "swapChain": {
                "type": "object",
                "properties": {
                  "known": {"type": "boolean"},
                  "swapChain": {"type": "string"},
                  "bufferCount": {"type": "integer", "minimum": 0, "maximum": 16},
                  "swapEffect": {"type": "integer", "minimum": 0, "maximum": 4},
                  "bufferUsage": {"type": "integer", "minimum": 0, "maximum": 65535},
                  "flags": {"type": "integer", "minimum": 0, "maximum": 65535},
                  "format": {"type": "integer", "minimum": 0, "maximum": 255},
                  "width": {"type": "integer", "minimum": 0, "maximum": 65535},
                  "height": {"type": "integer", "minimum": 0, "maximum": 65535},
                  "sampleCount": {"type": "integer", "minimum": 0, "maximum": 32},
                  "windowed": {"type": "boolean"},
                  "desc1Known": {"type": "boolean"},
                  "scaling": {"type": "integer", "minimum": 0, "maximum": 4},
                  "alphaMode": {"type": "integer", "minimum": 0, "maximum": 4}
                },
                "additionalProperties": false,
                "required": ["known", "swapChain", "bufferCount", "swapEffect", "bufferUsage", "flags", "format", "width", "height", "sampleCount", "windowed", "desc1Known", "scaling", "alphaMode"]
              }
            },
            "additionalProperties": false,
            "required": ["enabled", "presents", "present1Calls", "syncInterval", "doNotSequence", "restart", "doNotWait", "restrictToOutput", "useDuration", "allowTearing", "otherFlags", "flagsSeen", "descQueries", "descFailures", "swapChain"]
          },
          "rows": {
            "type": "array",
            "items": {
              "type": "object",
              "properties": {
                "present": {"type": "string"},
                "imagePass": {"type": "string"},
                "cameraPass": {"type": "string"},
                "delta": {"type": "string"},
                "cameraHash": {"type": "string"},
                "backBuffer": {"type": "string"},
                "resourceState": {"type": "string", "enum": ["UNKNOWN", "KNOWN", "AMBIGUOUS"]},
                "resourceAmbiguityMask": {"type": "integer", "minimum": 0, "maximum": 127},
                "presentContextOverlap": {"type": "boolean"},
                "relation": {"type": "string", "enum": ["MATCH", "MISMATCH", "UNKNOWN", "AMBIGUOUS"]}
              },
              "additionalProperties": false,
              "required": ["present", "imagePass", "cameraPass", "delta", "cameraHash", "backBuffer", "resourceState", "resourceAmbiguityMask", "presentContextOverlap", "relation"]
            }
          },
)json" R"json(
          "pairingRows": {
            "type": "array",
            "items": {
              "type": "object",
              "properties": {
                "provenanceEpoch": {"type": "string"},
                "eventSerial": {"type": "string"},
                "presentSerial": {"type": "string"},
                "imagePass": {"type": "string"},
                "imageRootEventSerial": {"type": "string"},
                "imageSourceResource": {"type": "string"},
                "imageModelGeneration": {"type": "string"},
                "overlayCameraSerial": {"type": "string"},
                "cameraSourcePass": {"type": "string"},
                "cameraSnapshotEventSerial": {"type": "string"},
                "cameraAdoptEventSerial": {"type": "string"},
                "ambiguityEventSerial": {"type": "string"},
                "cameraMetadataPass": {"type": "string"},
                "backBuffer": {"type": "string"},
                "delta": {"type": "string"},
                "relation": {"type": "string", "enum": ["MATCH", "MISMATCH", "UNKNOWN", "AMBIGUOUS"]},
                "imageOnBackBuffer": {"type": "boolean"},
                "cameraCoherent": {"type": "boolean"},
                "presentContextOverlap": {"type": "boolean"},
                "handoffSerial": {"type": "string"},
                "imageGeneration": {"type": "string"},
                "imageRooted": {"type": "boolean"},
                "cameraImageGeneration": {"type": "string"},
                "metadataStale": {"type": "boolean"},
                "backBufferState": {"type": "string", "enum": ["NONE", "HANDOFF", "CLEARED", "MIXED"]}
              },
              "additionalProperties": false,
              "required": ["provenanceEpoch", "eventSerial", "presentSerial", "imagePass", "imageRootEventSerial", "imageSourceResource", "imageModelGeneration", "overlayCameraSerial", "cameraSourcePass", "cameraSnapshotEventSerial", "cameraAdoptEventSerial", "ambiguityEventSerial", "cameraMetadataPass", "backBuffer", "delta", "relation", "imageOnBackBuffer", "cameraCoherent", "presentContextOverlap", "handoffSerial", "imageGeneration", "imageRooted", "cameraImageGeneration", "metadataStale", "backBufferState"]
            }
          },
)json" R"json(
          "cameraAgreement": {
            "type": "object",
            "properties": {
              "enabled": {"type": "boolean"},
              "imagesOpened": {"type": "string"},
              "imagesSkipped": {"type": "string"},
              "imagesRead": {"type": "string"},
              "imagesConsecutive": {"type": "string"},
              "drawsTruncated": {"type": "string"},
              "keysTruncated": {"type": "string"},
              "readbacksPending": {"type": "string"},
              "readbackFailures": {"type": "string"},
              "createFailures": {"type": "string"},
              "rootMissing": {"type": "string"},
              "rootPoseMissing": {"type": "string"},
              "referenceMissing": {"type": "string"},
              "imagesMoving": {"type": "string"},
              "rootSame": {"type": "string"},
              "rootPrevious": {"type": "string"},
              "rootAhead": {"type": "string"},
              "rootNeither": {"type": "string"},
              "rootAngleSumDegrees": {"type": "string"},
              "referenceStepSumDegrees": {"type": "string"},
              "keyCount": {"type": "integer", "minimum": 0, "maximum": 24},
              "keys": {
                "type": "array",
                "maxItems": 24,
                "items": {
                  "type": "object",
                  "properties": {
                    "count": {"type": "string"},
                    "ordinal": {"type": "integer", "minimum": 0, "maximum": 31},
                    "kind": {"type": "integer", "minimum": 0, "maximum": 7},
                    "seen": {"type": "string"},
                    "posed": {"type": "string"},
                    "poseWindow": {"type": "integer", "minimum": -1, "maximum": 3},
                    "formsB1": {"type": "array", "minItems": 8, "maxItems": 8, "items": {"type": "string"}},
                    "formsB2": {"type": "array", "minItems": 8, "maxItems": 8, "items": {"type": "string"}},
                    "moving": {"type": "string"},
                    "rootSame": {"type": "string"},
                    "rootPrevious": {"type": "string"},
                    "rootAhead": {"type": "string"},
                    "rootNeither": {"type": "string"},
                    "referenceMoving": {"type": "string"},
                    "vsReferenceSame": {"type": "string"},
                    "vsReferencePrevious": {"type": "string"},
                    "vsReferenceAhead": {"type": "string"},
                    "vsReferenceNeither": {"type": "string"},
                    "wasRoot": {"type": "string"},
                    "wasReference": {"type": "string"},
                    "stepSumDegrees": {"type": "string"},
                    "rootAngleSumDegrees": {"type": "string"},
                    "rootEyeSum": {"type": "string"},
                    "rootEyeSamples": {"type": "string"}
                  },
                  "additionalProperties": false,
                  "required": ["count", "ordinal", "kind", "seen", "posed", "poseWindow", "formsB1", "formsB2", "moving", "rootSame", "rootPrevious", "rootAhead", "rootNeither", "referenceMoving", "vsReferenceSame", "vsReferencePrevious", "vsReferenceAhead", "vsReferenceNeither", "wasRoot", "wasReference", "stepSumDegrees", "rootAngleSumDegrees", "rootEyeSum", "rootEyeSamples"]
                }
              },
              "dump": {
                "type": "array",
                "maxItems": 4,
                "items": {
                  "type": "object",
                  "properties": {
                    "generation": {"type": "string"},
                    "draws": {
                      "type": "array",
                      "maxItems": 32,
                      "items": {
                        "type": "object",
                        "properties": {
                          "count": {"type": "string"},
                          "ordinal": {"type": "integer", "minimum": 0, "maximum": 31},
                          "kind": {"type": "integer", "minimum": 0, "maximum": 7},
                          "root": {"type": "boolean"},
                          "reference": {"type": "boolean"},
                          "renderTarget": {"type": "string"},
                          "windows": {
                            "type": "array",
                            "minItems": 4,
                            "maxItems": 4,
                            "items": {
                              "type": "object",
                              "properties": {
                                "slot": {"type": "integer", "minimum": 0, "maximum": 3},
                                "bound": {"type": "boolean"},
                                "form": {"type": "integer", "minimum": 0, "maximum": 7},
                                "buffer": {"type": "string"},
                                "firstConstant": {"type": "string"},
                                "numConstants": {"type": "string"},
                                "bytesCopied": {"type": "integer", "minimum": 0, "maximum": 64},
                                "values": {"type": "array", "minItems": 16, "maxItems": 16, "items": {"type": "string"}}
                              },
                              "additionalProperties": false,
                              "required": ["slot", "bound", "form", "buffer", "firstConstant", "numConstants", "bytesCopied", "values"]
                            }
                          }
                        },
                        "additionalProperties": false,
                        "required": ["count", "ordinal", "kind", "root", "reference", "renderTarget", "windows"]
                      }
                    }
                  },
                  "additionalProperties": false,
                  "required": ["generation", "draws"]
                }
              }
            },
            "additionalProperties": false,
            "required": ["enabled", "imagesOpened", "imagesSkipped", "imagesRead", "imagesConsecutive", "drawsTruncated", "keysTruncated", "readbacksPending", "readbackFailures", "createFailures", "rootMissing", "rootPoseMissing", "referenceMissing", "imagesMoving", "rootSame", "rootPrevious", "rootAhead", "rootNeither", "rootAngleSumDegrees", "referenceStepSumDegrees", "keyCount", "keys", "dump"]
          },
          "presentedContent": {
            "type": "object",
            "properties": {
              "enabled": {"type": "boolean"}, "presentsSampled": {"type": "string"}, "presentsProcessed": {"type": "string"},
              "slotsBusy": {"type": "string"}, "readbacksPending": {"type": "string"}, "readbackFailures": {"type": "string"},
              "createFailures": {"type": "string"}, "abandoned": {"type": "string"}, "chainBreaks": {"type": "string"},
              "targetChanges": {"type": "string"}, "composited": {"type": "string"}, "compositedChanged": {"type": "string"},
              "repeats": {"type": "string"}, "repeatSame": {"type": "string"}, "repeatOther": {"type": "string"},
              "repeatUndecidable": {"type": "string"}, "repeatUnidentified": {"type": "string"}, "firstRepeats": {"type": "string"},
              "firstRepeatSame": {"type": "string"}, "firstRepeatOther": {"type": "string"}, "firstRepeatUnidentified": {"type": "string"},
              "overlayBound": {"type": "string"},
              "trueDelta": {"type": "array", "minItems": 5, "maxItems": 5, "items": {"type": "string"}},
              "screenSeconds": {"type": "array", "minItems": 5, "maxItems": 5, "items": {"type": "string"}},
              "trueDeltaUnknown": {"type": "string"}, "screenSecondsUnknown": {"type": "string"}, "bookkeepingDisagrees": {"type": "string"},
              "framesReady": {"type": "integer", "minimum": 0, "maximum": 6},
              "coverage": {"type": "array", "maxItems": 6, "items": {"type": "object", "properties": {
                "relation": {"type": "string"}, "presents": {"type": "string"}, "calls": {"type": "string"}, "draws": {"type": "string"},
                "noDraws": {"type": "string"}, "repairs": {"type": "string"}}, "additionalProperties": false,
                "required": ["relation", "presents", "calls", "draws", "noDraws", "repairs"]}},
              "presentsWithRepairAfterCalls": {"type": "string"},
              "slotRepairs": {"type": "array", "maxItems": 28, "items": {"type": "object", "properties": {
                "slot": {"type": "string"}, "repairs": {"type": "string"}}, "additionalProperties": false, "required": ["slot", "repairs"]}},
              "frames": {"type": "array", "maxItems": 6, "items": {"type": "object", "properties": {
                "presentSerial": {"type": "string"}, "seconds": {"type": "string"}, "handoffSerial": {"type": "string"},
                "imageGeneration": {"type": "string"}, "cameraImageGeneration": {"type": "string"}, "cameraBound": {"type": "boolean"},
                "relation": {"type": "string", "enum": ["UNKNOWN", "COMPOSITED", "SAME_BUFFER", "OTHER_BUFFER", "UNDECIDABLE", "UNIDENTIFIED"]},
                "displayedKnown": {"type": "boolean"}, "displayed": {"type": "string"}, "x": {"type": "integer", "minimum": 0},
                "y": {"type": "integer", "minimum": 0}, "width": {"type": "integer", "minimum": 0}, "height": {"type": "integer", "minimum": 0},
                "path": {"type": "string"}, "pathBefore": {"type": "string"}},
                "additionalProperties": false,
                "required": ["presentSerial", "seconds", "handoffSerial", "imageGeneration", "cameraImageGeneration", "cameraBound", "relation", "displayedKnown", "displayed", "x", "y", "width", "height", "path", "pathBefore"]}}
            },
            "additionalProperties": false,
            "required": ["enabled", "presentsSampled", "presentsProcessed", "slotsBusy", "readbacksPending", "readbackFailures", "createFailures", "abandoned", "chainBreaks", "targetChanges", "composited", "compositedChanged", "repeats", "repeatSame", "repeatOther", "repeatUndecidable", "repeatUnidentified", "firstRepeats", "firstRepeatSame", "firstRepeatOther", "firstRepeatUnidentified", "overlayBound", "trueDelta", "screenSeconds", "trueDeltaUnknown", "screenSecondsUnknown", "bookkeepingDisagrees", "framesReady", "coverage", "presentsWithRepairAfterCalls", "slotRepairs", "frames"]
          },
)json" R"json(
          "imageTransfer": {
            "type": "object",
            "properties": {
              "stats": {
                "type": "object",
                "properties": {
                  "enabled": {"type": "boolean"},
                  "epoch": {"type": "string"},
                  "capturesOpened": {"type": "integer", "minimum": 0, "maximum": 4},
                  "capturesClosed": {"type": "integer", "minimum": 0, "maximum": 4},
                  "capturesTruncated": {"type": "integer", "minimum": 0, "maximum": 4},
                  "capturesAbortedByResize": {"type": "integer", "minimum": 0, "maximum": 4},
                  "rootsSeen": {"type": "string"},
                  "eventsRecorded": {"type": "string"},
                  "eventsDropped": {"type": "string"},
                  "eventsAfterClose": {"type": "string"},
                  "unmodelledWork": {"type": "string"},
                  "trackedOverflow": {"type": "string"},
                  "lastBackBuffer": {"type": "string"}
                },
                "additionalProperties": false,
                "required": ["enabled", "epoch", "capturesOpened", "capturesClosed", "capturesTruncated", "capturesAbortedByResize", "rootsSeen", "eventsRecorded", "eventsDropped", "eventsAfterClose", "unmodelledWork", "trackedOverflow", "lastBackBuffer"]
              },
              "captures": {
                "type": "array",
                "items": {
                  "type": "object",
                  "properties": {
                    "capture": {"type": "integer", "minimum": 0, "maximum": 3},
                    "rootPass": {"type": "string"},
                    "rootResource": {"type": "string"},
                    "rootEventSerial": {"type": "string"},
                    "openOrderSerial": {"type": "string"},
                    "closeOrderSerial": {"type": "string"},
                    "closeReason": {"type": "string", "enum": ["OPEN", "PRESENTS", "FULL", "RESIZE"]},
                    "presentsSeen": {"type": "integer", "minimum": 0, "maximum": 8},
                    "drawsSinceRoot": {"type": "string"},
                    "rootWrites": {"type": "string"},
                    "unmodelledWork": {"type": "string"},
                    "trackedCount": {"type": "integer", "minimum": 0, "maximum": 8},
                    "trackedOverflow": {"type": "string"},
                    "eventsRecorded": {"type": "integer", "minimum": 0, "maximum": 96},
                    "eventsDropped": {"type": "string"}
                  },
                  "additionalProperties": false,
                  "required": ["capture", "rootPass", "rootResource", "rootEventSerial", "openOrderSerial", "closeOrderSerial", "closeReason", "presentsSeen", "drawsSinceRoot", "rootWrites", "unmodelledWork", "trackedCount", "trackedOverflow", "eventsRecorded", "eventsDropped"]
                }
              },
              "events": {
                "type": "array",
                "items": {
                  "type": "object",
                  "properties": {
                    "epoch": {"type": "string"},
                    "capture": {"type": "integer", "minimum": 0, "maximum": 3},
                    "orderSerial": {"type": "string"},
                    "role": {"type": "string", "enum": ["CONTEXT", "PRESENT"]},
                    "kind": {"type": "string", "enum": ["ROOT", "NEXT_ROOT", "DRAW", "COPY", "PARTIAL_COPY", "CLEAR", "RESOURCE_WRITE", "UNMODELLED_WORK", "PRESENT_BEGIN", "PRESENT_END"]},
                    "drawKind": {"type": "string", "enum": ["INDEXED", "DIRECT", "INDEXED_INSTANCED", "INSTANCED", "AUTO", "INDEXED_INSTANCED_INDIRECT", "INSTANCED_INDIRECT", "UNKNOWN"]},
                    "drawCount": {"type": "string"},
                    "drawsSinceRoot": {"type": "string"},
                    "scenePass": {"type": "string"},
                    "rtv0": {"type": "string"},
                    "rtvCount": {"type": "integer", "minimum": 0, "maximum": 8},
                    "srv0": {"type": "string"},
                    "srv1": {"type": "string"},
                    "srv2": {"type": "string"},
                    "srv3": {"type": "string"},
                    "trackedSrvMask": {"type": "string"},
                    "trackedSrvHits": {"type": "integer", "minimum": 0, "maximum": 128},
                    "source": {"type": "string"},
                    "destination": {"type": "string"},
                    "resource": {"type": "string"},
                    "backBuffer": {"type": "string"},
                    "writesBackBuffer": {"type": "boolean"},
                    "readsTracked": {"type": "boolean"},
                    "readsRoot": {"type": "boolean"},
                    "trackedResource": {"type": "string"},
                    "trackedAdded": {"type": "integer", "minimum": 0, "maximum": 8},
                    "parentResource": {"type": "string"},
                    "succeeded": {"type": "boolean"}
                  },
                  "additionalProperties": false,
                  "required": ["epoch", "capture", "orderSerial", "role", "kind", "drawKind", "drawCount", "drawsSinceRoot", "scenePass", "rtv0", "rtvCount", "srv0", "srv1", "srv2", "srv3", "trackedSrvMask", "trackedSrvHits", "source", "destination", "resource", "backBuffer", "writesBackBuffer", "readsTracked", "readsRoot", "trackedResource", "trackedAdded", "parentResource", "succeeded"]
                }
              }
            },
            "additionalProperties": false,
            "required": ["stats", "captures", "events"]
          }
        },
        "additionalProperties": false,
        "required": ["enabled", "hookInstalled", "contextHookInstalled", "presentHookInstalled", "srvHookEnabled", "contextThreadId", "presentThreadId", "presents", "matched", "mismatched", "unknown", "ambiguous", "backBufferFailures", "resourceTableOverflows", "renderThreadViolations", "unsupportedGpuWork", "snapshotDrainTimeouts", "contextHookRepairs", "rowsOverwritten", "nonCameraDrawTransitions", "sampledAmbiguousTransitions", "conflictingSampledPassTransitions", "partialCopyTransitions", "resourceWriteTransitions", "unsupportedGpuWorkTransitions", "secondaryCameraTargetTransitions", "firstKnownToAmbiguousDraw", "resourceAmbiguousPresents", "presentContextOverlaps", "contextSlotsPatched", "resourceResetPending", "rows", "pairingEnabled", "provenanceEpoch", "imageDrawsCommitted", "cameraSnapshots", "cameraBindings", "pairingPresents", "pairingMatched", "pairingMismatched", "pairingUnknown", "pairingAmbiguous", "uniqueImagePassesObserved", "uniqueImagePassesClassified", "uniqueMatched", "uniqueMismatched", "uniqueUnknown", "uniqueAmbiguous", "duplicatePresents", "pairingRowsOverwritten", "handoffs", "handoffsUnrooted", "handoffsSameGeneration", "generationsWithMultipleRoots", "sceneColourChanges", "backBufferClears", "backBufferOtherWrites", "presentsWithoutBinding", "imageGeneration", "uniqueDelta", "repeatMatched", "repeatMismatched", "repeatUnknown", "repeatAmbiguous", "repeatDelta", "presentProfile", "cameraAgreement", "presentedContent", "pairingRows", "imageTransfer"]
      })json" }
};

} // namespace

NativeCommandRegistrations GetViewerPassProvenanceCommandRegistrations ()
{
    return MakeRegistrationView (kViewerPassProvenanceCommandRegistrations);
}

} // namespace geomsrv
