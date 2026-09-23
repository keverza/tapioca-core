#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/ViewerPassProvenanceCommands.hpp"

#include "ArchViz/Dxgi/PassProvenance.hpp"
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

        bool reset = false;
        bool enabled = false;
        const bool hasEnabled = params.Contains ("enabled");
        if (params.Contains ("reset"))
            params.Get ("reset", reset);
        if (hasEnabled)
            params.Get ("enabled", enabled);
        const bool restoreEnabled = reset && provenance::Enabled () && !hasEnabled;
        bool drained = true;
        if (reset || (hasEnabled && !enabled)) {
            pairing::SetEnabled (false);
            drained = provenance::SetEnabled (false);
        }
        if (reset && drained) {
            provenance::Reset ();
            pairing::Reset ();
        }
        bool transitionSucceeded = drained;
        if (hasEnabled && enabled && drained) {
            transitionSucceeded = provenance::SetEnabled (true);
            pairing::SetEnabled (transitionSucceeded);
        }
        else if (restoreEnabled && drained) {
            transitionSucceeded = provenance::SetEnabled (true);
            pairing::SetEnabled (transitionSucceeded);
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
            pairingOut.Push (entry);
        }

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
        os.Add ("pairingRows", pairingOut);
        return os;
    }
};

const NativeCommandRegistration kViewerPassProvenanceCommandRegistrations[] = {
    { "ViewerPassProvenance", &MakeRegisteredNativeCommand<ViewerPassProvenanceCommand>, false,
      R"json({
        "type": "object",
        "properties": {
          "enabled": {"type": "boolean"},
          "reset": {"type": "boolean"},
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
                "presentContextOverlap": {"type": "boolean"}
              },
              "additionalProperties": false,
              "required": ["provenanceEpoch", "eventSerial", "presentSerial", "imagePass", "imageRootEventSerial", "imageSourceResource", "imageModelGeneration", "overlayCameraSerial", "cameraSourcePass", "cameraSnapshotEventSerial", "cameraAdoptEventSerial", "ambiguityEventSerial", "cameraMetadataPass", "backBuffer", "delta", "relation", "imageOnBackBuffer", "cameraCoherent", "presentContextOverlap"]
            }
          }
        },
        "additionalProperties": false,
        "required": ["enabled", "hookInstalled", "contextHookInstalled", "presentHookInstalled", "srvHookEnabled", "contextThreadId", "presentThreadId", "presents", "matched", "mismatched", "unknown", "ambiguous", "backBufferFailures", "resourceTableOverflows", "renderThreadViolations", "unsupportedGpuWork", "snapshotDrainTimeouts", "contextHookRepairs", "rowsOverwritten", "nonCameraDrawTransitions", "sampledAmbiguousTransitions", "conflictingSampledPassTransitions", "partialCopyTransitions", "resourceWriteTransitions", "unsupportedGpuWorkTransitions", "secondaryCameraTargetTransitions", "firstKnownToAmbiguousDraw", "resourceAmbiguousPresents", "presentContextOverlaps", "contextSlotsPatched", "resourceResetPending", "rows", "pairingEnabled", "provenanceEpoch", "imageDrawsCommitted", "cameraSnapshots", "cameraBindings", "pairingPresents", "pairingMatched", "pairingMismatched", "pairingUnknown", "pairingAmbiguous", "uniqueImagePassesObserved", "uniqueImagePassesClassified", "uniqueMatched", "uniqueMismatched", "uniqueUnknown", "uniqueAmbiguous", "duplicatePresents", "pairingRowsOverwritten", "pairingRows"]
      })json" }
};

} // namespace

NativeCommandRegistrations GetViewerPassProvenanceCommandRegistrations ()
{
    return MakeRegistrationView (kViewerPassProvenanceCommandRegistrations);
}

} // namespace geomsrv
