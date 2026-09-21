#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/ViewerPassProvenanceCommands.hpp"

#include "ArchViz/Dxgi/PassProvenance.hpp"

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

class ViewerPassProvenanceCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "ViewerPassProvenance";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        namespace provenance = av::dxgi::passprovenance;

        bool reset = false;
        bool enabled = false;
        const bool hasEnabled = params.Contains ("enabled");
        if (params.Contains ("reset"))
            params.Get ("reset", reset);
        if (hasEnabled)
            params.Get ("enabled", enabled);
        const bool restoreEnabled = reset && provenance::Enabled () && !hasEnabled;
        bool drained = true;
        if (reset || (hasEnabled && !enabled))
            drained = provenance::SetEnabled (false);
        if (reset && drained)
            provenance::Reset ();
        bool transitionSucceeded = drained;
        if (hasEnabled && enabled && drained)
            transitionSucceeded = provenance::SetEnabled (true);
        else if (restoreEnabled && drained)
            transitionSucceeded = provenance::SetEnabled (true);

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

        const provenance::Stats stats = provenance::GetStats ();
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
        os.Add ("resourceAmbiguousPresents",
                GS::UniString (std::to_string (stats.resourceAmbiguousPresents).c_str (), CC_UTF8));
        os.Add ("presentContextOverlaps",
                GS::UniString (std::to_string (stats.presentContextOverlaps).c_str (), CC_UTF8));
        os.Add ("contextSlotsPatched", GS::Int32 (stats.contextSlotsPatched));
        os.Add ("resourceResetPending", stats.resourceResetPending);
        os.Add ("rows", out);
        return os;
    }
};

const NativeCommandRegistration
    kViewerPassProvenanceCommandRegistrations
        [] = {
            { "ViewerPassProvenance",
              &MakeRegisteredNativeCommand<ViewerPassProvenanceCommand>, false, R"json({"type":"object","properties":{"enabled":{"type":"boolean"},"reset":{"type":"boolean"},"limit":{"type":"integer","minimum":1,"maximum":1024}},"additionalProperties":false})json", R"json({"type":"object","properties":{"enabled":{"type":"boolean"},"hookInstalled":{"type":"boolean"},"contextHookInstalled":{"type":"boolean"},"presentHookInstalled":{"type":"boolean"},"srvHookEnabled":{"type":"boolean"},"contextThreadId":{"type":"string"},"presentThreadId":{"type":"string"},"presents":{"type":"string"},"matched":{"type":"string"},"mismatched":{"type":"string"},"unknown":{"type":"string"},"ambiguous":{"type":"string"},"backBufferFailures":{"type":"string"},"resourceTableOverflows":{"type":"string"},"renderThreadViolations":{"type":"string"},"unsupportedGpuWork":{"type":"string"},"snapshotDrainTimeouts":{"type":"string"},"contextHookRepairs":{"type":"string"},"rowsOverwritten":{"type":"string"},"nonCameraDrawTransitions":{"type":"string"},"sampledAmbiguousTransitions":{"type":"string"},"conflictingSampledPassTransitions":{"type":"string"},"partialCopyTransitions":{"type":"string"},"resourceWriteTransitions":{"type":"string"},"unsupportedGpuWorkTransitions":{"type":"string"},"secondaryCameraTargetTransitions":{"type":"string"},"resourceAmbiguousPresents":{"type":"string"},"presentContextOverlaps":{"type":"string"},"contextSlotsPatched":{"type":"integer"},"resourceResetPending":{"type":"boolean"},"rows":{"type":"array","items":{"type":"object","properties":{"present":{"type":"string"},"imagePass":{"type":"string"},"cameraPass":{"type":"string"},"delta":{"type":"string"},"cameraHash":{"type":"string"},"backBuffer":{"type":"string"},"resourceState":{"type":"string","enum":["UNKNOWN","KNOWN","AMBIGUOUS"]},"resourceAmbiguityMask":{"type":"integer","minimum":0,"maximum":127},"presentContextOverlap":{"type":"boolean"},"relation":{"type":"string","enum":["MATCH","MISMATCH","UNKNOWN","AMBIGUOUS"]}},"additionalProperties":false,"required":["present","imagePass","cameraPass","delta","cameraHash","backBuffer","resourceState","resourceAmbiguityMask","presentContextOverlap","relation"]}}},"additionalProperties":false,"required":["enabled","hookInstalled","contextHookInstalled","presentHookInstalled","srvHookEnabled","contextThreadId","presentThreadId","presents","matched","mismatched","unknown","ambiguous","backBufferFailures","resourceTableOverflows","renderThreadViolations","unsupportedGpuWork","snapshotDrainTimeouts","contextHookRepairs","rowsOverwritten","nonCameraDrawTransitions","sampledAmbiguousTransitions","conflictingSampledPassTransitions","partialCopyTransitions","resourceWriteTransitions","unsupportedGpuWorkTransitions","secondaryCameraTargetTransitions","resourceAmbiguousPresents","presentContextOverlaps","contextSlotsPatched","resourceResetPending","rows"]})json" },
        };

} // namespace

NativeCommandRegistrations GetViewerPassProvenanceCommandRegistrations ()
{
    return MakeRegistrationView (kViewerPassProvenanceCommandRegistrations);
}

} // namespace geomsrv
