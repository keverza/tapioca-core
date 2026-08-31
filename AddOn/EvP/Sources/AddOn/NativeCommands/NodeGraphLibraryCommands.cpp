#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/NodeGraphLibraryCommands.hpp"

#include "NativeCommands/NodeGraphCommandSupport.hpp"

#include "NodeGraph/GraphStore.hpp"

#include <map>
#include <string>

namespace geomsrv {
namespace {

constexpr const char kLibraryListInputSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1}},"additionalProperties":false,"required":[]})json";

// ---------------------------------------------------------------------------
// The workflow library.
//
// A save and a load are REPORTED outcomes rather than command failures, exactly
// as an evaluation is: "there is no graph called that" is an answer a client
// renders, and a command failure carries only a string.
// ---------------------------------------------------------------------------

// Layout crosses as strings by design. The runtime carries it and never reads a
// member of it, so an editor can add a field without a schema change and without
// the evaluator acquiring an opinion about node positions.
constexpr const char kLibrarySaveInputSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1},"name":{"type":"string","minLength":1,"maxLength":128},"label":{"type":"string"},"description":{"type":"string"},"nodeLayout":{"type":"array","items":{"type":"object","properties":{"nodeId":{"type":"string","minLength":1},"fields":{"type":"array","items":{"type":"object","properties":{"key":{"type":"string","minLength":1},"value":{"type":"string"}},"additionalProperties":false,"required":["key","value"]}}},"additionalProperties":false,"required":["nodeId","fields"]}}},"additionalProperties":false,"required":["name"]})json";

constexpr const char kLibraryLoadInputSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1},"name":{"type":"string","minLength":1,"maxLength":128}},"additionalProperties":false,"required":["name"]})json";

// Spelled out rather than aliased to the load schema: the API generator reads
// these as literals, and an alias is a schema it cannot resolve.
constexpr const char kLibraryDeleteInputSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1},"name":{"type":"string","minLength":1,"maxLength":128}},"additionalProperties":false,"required":["name"]})json";

constexpr const char kLibraryStatusResponseSchema[] =
    R"json({"type":"object","properties":{"ok":{"type":"boolean"},"status":{"type":"string","enum":["ok","notFound","invalid","ioFailed","invalidId"]},"error":{"type":"string"},"name":{"type":"string"},"graphId":{"type":"string"}},"additionalProperties":false,"required":["ok","status","error","name","graphId"]})json";

constexpr const char kLibraryLoadResponseSchema[] =
    R"json({"type":"object","properties":{"ok":{"type":"boolean"},"status":{"type":"string","enum":["ok","notFound","invalid","ioFailed","invalidId"]},"error":{"type":"string"},"name":{"type":"string"},"graphId":{"type":"string"},"revision":{"type":"integer","minimum":0},"label":{"type":"string"},"description":{"type":"string"},"nodeLayout":{"type":"array","items":{"type":"object","properties":{"nodeId":{"type":"string"},"fields":{"type":"array","items":{"type":"object","properties":{"key":{"type":"string"},"value":{"type":"string"}},"additionalProperties":false,"required":["key","value"]}}},"additionalProperties":false,"required":["nodeId","fields"]}}},"additionalProperties":false,"required":["ok","status","error","name","graphId","revision","label","description","nodeLayout"]})json";

constexpr const char kLibraryListResponseSchema[] =
    R"json({"type":"object","properties":{"location":{"type":"string"},"graphs":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"label":{"type":"string"},"description":{"type":"string"},"nodeCount":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["name","label","description","nodeCount"]}}},"additionalProperties":false,"required":["location","graphs"]})json";

std::string ReadLibraryName (const GS::ObjectState& params)
{
    GS::UniString name;
    params.Get ("name", name);
    return GraphUtf8 (name);
}

GS::Array<GS::ObjectState> LayoutStates (const graph::GraphMetadata& metadata)
{
    GS::Array<GS::ObjectState> layout;
    for (const auto& [nodeId, fields] : metadata.nodeLayout) {
        GS::Array<GS::ObjectState> encoded;
        for (const auto& [key, value] : fields) {
            GS::ObjectState field;
            field.Add ("key", GraphText (key));
            field.Add ("value", GraphText (value));
            encoded.Push (field);
        }
        GS::ObjectState record;
        record.Add ("nodeId", GraphText (nodeId));
        record.Add ("fields", encoded);
        layout.Push (record);
    }
    return layout;
}

void AddStoreResult (GS::ObjectState& response, const graph::StoreResult& result, const std::string& name,
                     const graph::GraphId& graphId)
{
    response.Add ("ok", result.Ok ());
    response.Add ("status", graph::StoreStatusName (result.status));
    response.Add ("error", GraphText (result.error));
    response.Add ("name", GraphText (name));
    response.Add ("graphId", GraphText (graphId));
}

class GraphLibrarySaveCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        const graph::GraphId graphId = ReadGraphIdParam (params);
        const std::string name = ReadLibraryName (params);

        // Metadata arrives with the save when the client sends it, and is
        // otherwise whatever the runtime is already carrying - so a client that
        // does not track layout cannot wipe the layout another one stored.
        graph::GraphMetadata metadata = graph::GraphRuntimeState::Get ().Metadata (graphId);
        GS::UniString text;
        if (params.Get ("label", text))
            metadata.label = GraphUtf8 (text);
        if (params.Get ("description", text))
            metadata.description = GraphUtf8 (text);

        GS::Array<GS::ObjectState> layout;
        if (params.Get ("nodeLayout", layout)) {
            metadata.nodeLayout.clear ();
            for (const GS::ObjectState& record : layout) {
                GS::UniString nodeId;
                if (!record.Get ("nodeId", nodeId))
                    continue;
                GS::Array<GS::ObjectState> fields;
                record.Get ("fields", fields);
                std::map<std::string, std::string> entry;
                for (const GS::ObjectState& field : fields) {
                    GS::UniString key, value;
                    if (field.Get ("key", key) && field.Get ("value", value))
                        entry.emplace (GraphUtf8 (key), GraphUtf8 (value));
                }
                metadata.nodeLayout.emplace (GraphUtf8 (nodeId), std::move (entry));
            }
        }
        graph::GraphRuntimeState::Get ().SetMetadata (graphId, metadata);

        const graph::StoreResult result = graph::GraphRuntimeState::Get ().SaveToLibrary (graphId, name);
        GS::ObjectState response;
        AddStoreResult (response, result, name, graphId);
        return response;
    }
};

class GraphLibraryLoadCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        const graph::GraphId graphId = ReadGraphIdParam (params);
        const std::string name = ReadLibraryName (params);

        const graph::StoreResult result = graph::GraphRuntimeState::Get ().LoadFromLibrary (name, graphId);

        GS::ObjectState response;
        AddStoreResult (response, result, name, graphId);
        // The revision and the layout let a client redraw without a second call,
        // which is the whole reconstruct-from-the-runtime flow of §33.
        const graph::GraphMetadata metadata = graph::GraphRuntimeState::Get ().Metadata (graphId);
        response.Add ("revision",
                      static_cast<GS::Int64> (graph::GraphRuntimeState::Get ().Document (graphId).Revision ()));
        response.Add ("label", GraphText (metadata.label));
        response.Add ("description", GraphText (metadata.description));
        response.Add ("nodeLayout", LayoutStates (metadata));
        return response;
    }
};

class GraphLibraryDeleteCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        const std::string name = ReadLibraryName (params);
        const graph::StoreResult result = graph::GraphRuntimeState::Get ().Library ().Delete (name);
        GS::ObjectState response;
        AddStoreResult (response, result, name, ReadGraphIdParam (params));
        return response;
    }
};

class GraphLibraryListCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        GS::Array<GS::ObjectState> graphs;
        for (const graph::StoredGraphInfo& info : graph::GraphRuntimeState::Get ().Library ().List ()) {
            GS::ObjectState record;
            record.Add ("name", GraphText (info.graphId));
            record.Add ("label", GraphText (info.label));
            record.Add ("description", GraphText (info.description));
            record.Add ("nodeCount", static_cast<GS::Int64> (info.nodeCount));
            graphs.Push (record);
        }
        GS::ObjectState response;
        // Reported so a user can find the folder, and so a support question
        // about a missing workflow is answerable without guessing.
        response.Add ("location", GraphText (graph::FileGraphStore::DefaultWorkflowDirectory ()));
        response.Add ("graphs", graphs);
        return response;
    }
};


const NativeCommandRegistration registrations[] = {
    { "GraphLibrarySave", &MakeRegisteredNativeCommand<GraphLibrarySaveCommand>, false, kLibrarySaveInputSchema,
      kLibraryStatusResponseSchema },
    { "GraphLibraryLoad", &MakeRegisteredNativeCommand<GraphLibraryLoadCommand>, false, kLibraryLoadInputSchema,
      kLibraryLoadResponseSchema },
    { "GraphLibraryDelete", &MakeRegisteredNativeCommand<GraphLibraryDeleteCommand>, false,
      kLibraryDeleteInputSchema, kLibraryStatusResponseSchema },
    { "GraphLibraryList", &MakeRegisteredNativeCommand<GraphLibraryListCommand>, false, kLibraryListInputSchema,
      kLibraryListResponseSchema },
};

} // namespace

NativeCommandRegistrations GetNodeGraphLibraryCommandRegistrations ()
{
    return MakeRegistrationView (registrations);
}

} // namespace geomsrv
