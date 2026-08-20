#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/SnapshotCommands.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/CommandUtils.hpp"

#include "Geometry/GeometryExtractor.hpp"
#include "Geometry/MeshStore.hpp"
#include "Geometry/QueryEngine.hpp"      // QueryIndexCache — invalidated on release
#include "Metadata/MetadataExtractor.hpp"
#include "Metadata/MetadataStore.hpp"
#include "Screenshot/ScreenshotStore.hpp"
#include "Server/ServerState.hpp"

#include <chrono>
#include <memory>

namespace geomsrv {

namespace {

uint64_t g_nextSnapshotId = 1;

// Parse the `meta` param: false / "none" / true / "basic" / "full".
// NOTE `true` maps to BASIC, not FULL. Properties + classifications require
// GetPropertyDefinitions + GetPropertyValues + GetPropertyValueString PER ELEMENT,
// which froze Archicad on a large model. Opt into that explicitly with "full".
MetaLevel ParseMetaLevel (const GS::ObjectState& params)
{
    GS::UniString s;
    if (params.Get ("meta", s)) {
        if (s == "full")  return MetaLevel::Full;
        if (s == "basic") return MetaLevel::Basic;
        return MetaLevel::None;
    }
    bool b = false;
    if (params.Get ("meta", b))
        return b ? MetaLevel::Basic : MetaLevel::None;
    return MetaLevel::None;
}

// ---------------------------------------------------------------------------
// EvP.BuildSnapshot { scope: "all"|"selection", meta: bool }
// Extracts geometry on the main thread and publishes it for the HTTP data plane.
// Metadata is OPT-IN: it walks every element's property definitions/values and is
// by far the most expensive part, so we do not pay for it unless asked.
// ---------------------------------------------------------------------------
class BuildSnapshotCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "BuildSnapshot"; }

    // Long operation on a big model -> show Archicad's progress window, which also
    // gives the user a Cancel button. Silence is right for a fast command; a frozen
    // UI with no feedback and no way out is not.
    bool IsProcessWindowVisible () const override { return true; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl& pc) const override
    {
        GS::UniString scope ("all");
        params.Get ("scope", scope);
        const bool selectionOnly = (scope == "selection");
        const MetaLevel level = ParseMetaLevel (params);

        // Element-type filter. This is the single biggest lever on a big model:
        // on one real project 90% of all triangles were furniture + lights, none of
        // which matter for an envelope. Types are ModelerAPI::Element::Type.
        std::vector<int32_t> excludeTypes;
        GS::Array<GS::Int32> ex;
        if (params.Get ("excludeTypes", ex))
            for (GS::Int32 v : ex) excludeTypes.push_back (static_cast<int32_t> (v));

        pc.SetProcessName ("EvP: extracting geometry");

        const uint64_t id = g_nextSnapshotId++;
        auto snap = selectionOnly ? ExtractSelectedElements (id) : ExtractAllElements (id);

        if (!snap) {
            return NativeCommandResult::Failure (selectionOnly ? "could not read the selection"
                                                               : "could not read the 3D model - is a project open?");
        }

        GS::ObjectState os;

        // Drop excluded types before anything downstream pays for them.
        size_t droppedElems = 0, droppedTris = 0;
        if (!excludeTypes.empty ()) {
            auto keep = std::make_shared<Snapshot> ();
            keep->id = snap->id;
            keep->scope = snap->scope;
            for (auto& m : snap->meshes) {
                if (std::find (excludeTypes.begin (), excludeTypes.end (), m.elemType) != excludeTypes.end ()) {
                    ++droppedElems;
                    droppedTris += m.TriangleCount ();
                    continue;
                }
                keep->meshes.push_back (std::move (m));
            }
            snap = keep;
        }

        MeshStore::Get ().Publish (snap);
        ServerState::Get ().snapshotId.store (snap->id);

        bool cancelled = false;
        if (level != MetaLevel::None) {
            // Only for elements actually IN the snapshot — not every element in the
            // project. (A model with 874 meshes had 15,342 elements: a 17x waste.)
            std::vector<std::string> guids;
            guids.reserve (snap->meshes.size ());
            for (const auto& m : snap->meshes)
                guids.push_back (m.guid);

            pc.SetProcessName (level == MetaLevel::Full
                ? "EvP: metadata + properties"
                : "EvP: metadata");
            pc.SetProcessPhaseNum (static_cast<Int32> (guids.size ()));

            auto meta = ExtractMetadataFor (
                guids, level,
                [&pc] { return pc.TestBreak (); },                                  // Cancel button
                [&pc] (size_t done, size_t) { pc.SetProcessPhaseValue (static_cast<Int32> (done)); });

            if (meta)
                MetadataStore::Get ().Publish (meta);
            else
                cancelled = true;          // user cancelled: geometry stands, metadata doesn't
        }
        if (level == MetaLevel::None || cancelled)
            MetadataStore::Get ().Release ();   // stale metadata would lie about this snapshot

        os.Add ("snapshotId",    static_cast<GS::Int64> (snap->id));
        os.Add ("scope",         GS::UniString (snap->scope.c_str ()));
        os.Add ("elementCount",  static_cast<GS::Int64> (snap->meshes.size ()));
        os.Add ("vertexCount",   static_cast<GS::Int64> (snap->TotalVertices ()));
        os.Add ("triangleCount", static_cast<GS::Int64> (snap->TotalTriangles ()));
        os.Add ("hasMetadata",   level != MetaLevel::None && !cancelled);
        os.Add ("metaLevel",     GS::UniString (cancelled ? "cancelled"
                                              : level == MetaLevel::Full  ? "full"
                                              : level == MetaLevel::Basic ? "basic" : "none"));
        os.Add ("metadataCancelled", cancelled);
        if (!excludeTypes.empty ()) {
            os.Add ("droppedElements",  static_cast<GS::Int64> (droppedElems));
            os.Add ("droppedTriangles", static_cast<GS::Int64> (droppedTris));
        }
        AddMemory (os);
        return os;
    }
};

// ---------------------------------------------------------------------------
// EvP.ReleaseSnapshot {}
// Hands the memory back and puts the server to sleep. Pure C++ teardown — no
// ACAPI — but exposed here so Python drives the whole lifecycle from one channel.
// ---------------------------------------------------------------------------
class ReleaseSnapshotCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "ReleaseSnapshot"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        const size_t before = RetainedBytes ();

        QueryIndexCache::Get ().Release ();   // BVH first (it references the meshes)
        MeshStore::Get ().Release ();
        MetadataStore::Get ().Release ();
        ScreenshotStore::Get ().Release ();
        ServerState::Get ().snapshotId.store (0);

        GS::ObjectState os;
        os.Add ("freedBytes", static_cast<GS::Int64> (before - RetainedBytes ()));
        AddMemory (os);
        return os;
    }
};
// ---------------------------------------------------------------------------
// EvP.GetStatus {} — cheap introspection (also proves the bridge).
// ---------------------------------------------------------------------------
class GetStatusCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetStatus"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        ServerState& st = ServerState::Get ();
        GS::ObjectState os;
        os.Add ("serverRunning", st.serverRunning.load ());
        os.Add ("port",          static_cast<GS::Int64> (st.port.load ()));
        os.Add ("modelOpen",     st.modelOpen.load ());
        os.Add ("snapshotId",    static_cast<GS::Int64> (st.snapshotId.load ()));
        AddMemory (os);
        return os;
    }
};
// ---------------------------------------------------------------------------
// EvP.GetSnapshotInfo {} -> the mesh table of the live snapshot.
// What a script needs before acquiring zero-copy buffers: how many meshes there
// are, and what each one is. Flat parallel arrays rather than nested records, and
// deliberately so (§E16.0): this is bulk numeric data feeding numpy, which wants
// exactly this shape. Element-RECORD reads (GetElementInfo, GetElementDetails) went
// the other way and return nested objects — do not "unify" them with this.
//
// (Historical note: this comment used to claim GS::Array<GS::ObjectState>
// serialization was "unproven". It was never tested, then cited twice more as if it
// were a finding. The now-archived Get3DStyles command disproved it in Archicad on
// 2026-07-25. The flat shape here is now kept on its own merits, not on that claim.)
// ---------------------------------------------------------------------------
class GetSnapshotInfoCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetSnapshotInfo"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        const auto snapshot = MeshStore::Get ().Current ();
        if (snapshot == nullptr)
            return NativeCommandResult::Failure ("no snapshot is live - call EvP.BuildSnapshot first");

        GS::ObjectState os;

        GS::Array<GS::UniString> guids;
        GS::Array<GS::Int32>     elemTypes, vertexCounts, triangleCounts;
        for (const Mesh& mesh : snapshot->meshes) {
            guids.Push (GS::UniString (mesh.guid.c_str (), CC_UTF8));
            elemTypes.Push ((GS::Int32) mesh.elemType);
            vertexCounts.Push ((GS::Int32) mesh.VertexCount ());
            triangleCounts.Push ((GS::Int32) mesh.TriangleCount ());
        }

        os.Add ("snapshotId", (GS::Int64) snapshot->id);
        os.Add ("scope", GS::UniString (snapshot->scope.c_str (), CC_UTF8));
        os.Add ("meshCount", (GS::Int32) snapshot->meshes.size ());
        os.Add ("guids", guids);
        os.Add ("elemTypes", elemTypes);
        os.Add ("vertexCounts", vertexCounts);
        os.Add ("triangleCounts", triangleCounts);
        AddMemory (os);
        return os;
    }
};

const NativeCommandRegistration kSnapshotCommandRegistrations[] = {
    { "BuildSnapshot", &MakeRegisteredNativeCommand<BuildSnapshotCommand>, false,
      R"json({
            "type":"object",
            "properties":{
                "scope":{"type":"string","enum":["all","selection"]},
                "excludeTypes":{"type":"array","items":{"type":"integer"}},
                "meta":{"oneOf":[
                    {"type":"boolean"},
                    {"type":"string","enum":["none","basic","full"]}
                ]}
            },
            "additionalProperties":false
        })json",
      R"json({
            "type":"object",
            "properties":{
                "snapshotId":{"type":"integer"},
                "scope":{"type":"string","enum":["all","selection"]},
                "elementCount":{"type":"integer"},
                "vertexCount":{"type":"integer"},
                "triangleCount":{"type":"integer"},
                "hasMetadata":{"type":"boolean"},
                "metaLevel":{"type":"string","enum":["none","basic","full","cancelled"]},
                "metadataCancelled":{"type":"boolean"},
                "droppedElements":{"type":"integer"},
                "droppedTriangles":{"type":"integer"},
                "retainedBytes":{"type":"integer"}
            },
            "additionalProperties":false,
            "required":["snapshotId","scope","elementCount","vertexCount","triangleCount","hasMetadata","metaLevel","metadataCancelled","retainedBytes"]
        })json" },
    { "ReleaseSnapshot", &MakeRegisteredNativeCommand<ReleaseSnapshotCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({
            "type":"object",
            "properties":{"freedBytes":{"type":"integer"},"retainedBytes":{"type":"integer"}},
            "additionalProperties":false,
            "required":["freedBytes","retainedBytes"]
        })json" },
    { "GetSnapshotInfo", &MakeRegisteredNativeCommand<GetSnapshotInfoCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({
            "type":"object",
            "properties":{
                "snapshotId":{"type":"integer"},
                "scope":{"type":"string","enum":["all","selection"]},
                "meshCount":{"type":"integer"},
                "guids":{"type":"array","items":{"type":"string"}},
                "elemTypes":{"type":"array","items":{"type":"integer"}},
                "vertexCounts":{"type":"array","items":{"type":"integer"}},
                "triangleCounts":{"type":"array","items":{"type":"integer"}},
                "retainedBytes":{"type":"integer"}
            },
            "additionalProperties":false,
            "required":["snapshotId","scope","meshCount","guids","elemTypes","vertexCounts","triangleCounts","retainedBytes"]
        })json" },
    { "GetStatus", &MakeRegisteredNativeCommand<GetStatusCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({
            "type":"object",
            "properties":{
                "serverRunning":{"type":"boolean"},
                "port":{"type":"integer"},
                "modelOpen":{"type":"boolean"},
                "snapshotId":{"type":"integer"},
                "retainedBytes":{"type":"integer"}
            },
            "additionalProperties":false,
            "required":["serverRunning","port","modelOpen","snapshotId","retainedBytes"]
        })json" }
};

}   // namespace

NativeCommandRegistrations GetSnapshotCommandRegistrations ()
{
    return MakeRegistrationView (kSnapshotCommandRegistrations);
}

GSErrCode InstallSnapshotJsonCommands ()
{
    GSErrCode err = ACAPI_AddOnAddOnCommunication_InstallAddOnCommandHandler (
        GS::NewOwned<RegisteredNativeCommand<BuildSnapshotCommand>> (kSnapshotCommandRegistrations[0]));
    if (err != NoError) return err;

    err = ACAPI_AddOnAddOnCommunication_InstallAddOnCommandHandler (
        GS::NewOwned<RegisteredNativeCommand<ReleaseSnapshotCommand>> (kSnapshotCommandRegistrations[1]));
    if (err != NoError) return err;

    return ACAPI_AddOnAddOnCommunication_InstallAddOnCommandHandler (
        GS::NewOwned<RegisteredNativeCommand<GetStatusCommand>> (kSnapshotCommandRegistrations[3]));
}

} // namespace geomsrv
