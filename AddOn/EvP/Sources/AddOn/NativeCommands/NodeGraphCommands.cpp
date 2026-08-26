#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/NodeGraphCommands.hpp"

#include "NodeGraph/GraphRuntimeState.hpp"

#include <string>

namespace geomsrv {
namespace {

namespace graph = evp::nodegraph;

constexpr const char kEmptyInputSchema[] =
    R"json({"type":"object","properties":{},"additionalProperties":false,"required":[]})json";
constexpr const char kCatalogResponseSchema[] =
    R"json({"type":"object","properties":{"nodeTypes":{"type":"array","items":{"type":"object","properties":{"nodeType":{"type":"string"},"label":{"type":"string"},"category":{"type":"string"},"description":{"type":"string"},"executionDomain":{"type":"string","enum":["worker","archicadMainThread","renderThread"]},"inputs":{"type":"array","items":{"type":"object","properties":{"portId":{"type":"string"},"label":{"type":"string"},"valueType":{"type":"string"},"required":{"type":"boolean"},"acceptsMultiple":{"type":"boolean"}},"additionalProperties":false,"required":["portId","label","valueType","required","acceptsMultiple"]}},"outputs":{"type":"array","items":{"type":"object","properties":{"portId":{"type":"string"},"label":{"type":"string"},"valueType":{"type":"string"}},"additionalProperties":false,"required":["portId","label","valueType"]}},"parameters":{"type":"array","items":{"type":"object","properties":{"parameterId":{"type":"string"},"label":{"type":"string"},"valueType":{"type":"string"},"required":{"type":"boolean"}},"additionalProperties":false,"required":["parameterId","label","valueType","required"]}}},"additionalProperties":false,"required":["nodeType","label","category","description","executionDomain","inputs","outputs","parameters"]}}},"additionalProperties":false,"required":["nodeTypes"]})json";
constexpr const char kStateResponseSchema[] =
    R"json({"type":"object","properties":{"revision":{"type":"integer","minimum":0},"nodes":{"type":"array","items":{"type":"object","properties":{"nodeId":{"type":"string"},"nodeType":{"type":"string"},"parameters":{"type":"array","items":{"type":"object","properties":{"parameterId":{"type":"string"},"valueType":{"type":"string"},"numberValue":{"type":"number"}},"additionalProperties":false,"required":["parameterId","valueType"]}}},"additionalProperties":false,"required":["nodeId","nodeType","parameters"]}},"edges":{"type":"array","items":{"type":"object","properties":{"sourceNode":{"type":"string"},"sourcePort":{"type":"string"},"targetNode":{"type":"string"},"targetPort":{"type":"string"}},"additionalProperties":false,"required":["sourceNode","sourcePort","targetNode","targetPort"]}}},"additionalProperties":false,"required":["revision","nodes","edges"]})json";
constexpr const char kEditInputSchema[] =
    R"json({"oneOf":[{"type":"object","properties":{"editKind":{"const":"addNode"},"nodeId":{"type":"string","minLength":1},"nodeType":{"type":"string","minLength":1},"numberValue":{"type":"number"}},"additionalProperties":false,"required":["editKind","nodeId","nodeType"]},{"type":"object","properties":{"editKind":{"const":"removeNode"},"nodeId":{"type":"string","minLength":1}},"additionalProperties":false,"required":["editKind","nodeId"]},{"type":"object","properties":{"editKind":{"enum":["connect","disconnect"]},"sourceNode":{"type":"string","minLength":1},"sourcePort":{"type":"string","minLength":1},"targetNode":{"type":"string","minLength":1},"targetPort":{"type":"string","minLength":1}},"additionalProperties":false,"required":["editKind","sourceNode","sourcePort","targetNode","targetPort"]},{"type":"object","properties":{"editKind":{"const":"setParam"},"nodeId":{"type":"string","minLength":1},"parameterId":{"type":"string","minLength":1},"numberValue":{"type":"number"}},"additionalProperties":false,"required":["editKind","nodeId","parameterId","numberValue"]}]})json";
constexpr const char kEditResponseSchema[] =
    R"json({"type":"object","properties":{"revision":{"type":"integer","minimum":1},"dirtyNodes":{"type":"array","items":{"type":"string"}}},"additionalProperties":false,"required":["revision","dirtyNodes"]})json";
constexpr const char kEvaluateResponseSchema[] =
    R"json({"type":"object","properties":{"revision":{"type":"integer","minimum":0},"nodes":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["revision","nodes"]})json";
constexpr const char kResultsResponseSchema[] =
    R"json({"type":"object","properties":{"revision":{"type":"integer","minimum":0},"results":{"type":"array","items":{"type":"object","properties":{"nodeId":{"type":"string"},"status":{"type":"string","enum":["dirty","complete","failed","blocked"]},"message":{"type":"string"},"durationMilliseconds":{"type":"number","minimum":0},"itemCount":{"type":"integer","minimum":0},"previewAvailable":{"type":"boolean"},"outputs":{"type":"array","items":{"type":"object","properties":{"portId":{"type":"string"},"valueType":{"type":"string"},"numberValue":{"type":"number"},"itemCount":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["portId","valueType"]}}},"additionalProperties":false,"required":["nodeId","status","message","durationMilliseconds","itemCount","previewAvailable","outputs"]}}},"additionalProperties":false,"required":["revision","results"]})json";

std::string Utf8 (const GS::UniString& value)
{
    return value.ToCStr (0, GS::MaxUSize, CC_UTF8).Get ();
}

GS::UniString Text (const std::string& value)
{
    return GS::UniString (value.c_str (), CC_UTF8);
}

const char* ValueTypeName (graph::ValueType valueType)
{
    constexpr const char* names[] = { "absent", "bool",     "integer", "double", "string",
                                      "point3", "polyline", "polygon", "mesh",   "archicadElementRef",
                                      "list" };
    return names[static_cast<size_t> (valueType)];
}

const char* DomainName (graph::ExecutionDomain domain)
{
    switch (domain) {
        case graph::ExecutionDomain::Worker:
            return "worker";
        case graph::ExecutionDomain::ArchicadMainThread:
            return "archicadMainThread";
        case graph::ExecutionDomain::RenderThread:
            return "renderThread";
    }
    return "worker";
}

const char* StatusName (graph::NodeExecutionState state)
{
    switch (state) {
        case graph::NodeExecutionState::Dirty:
            return "dirty";
        case graph::NodeExecutionState::Complete:
            return "complete";
        case graph::NodeExecutionState::Failed:
            return "failed";
        case graph::NodeExecutionState::Blocked:
            return "blocked";
    }
    return "dirty";
}

GS::ObjectState PortState (const graph::PortSchema& port, bool includeInputFields)
{
    GS::ObjectState state;
    state.Add ("portId", Text (port.id));
    state.Add ("label", Text (port.label));
    state.Add ("valueType", ValueTypeName (port.valueType));
    if (includeInputFields) {
        state.Add ("required", port.required);
        state.Add ("acceptsMultiple", port.acceptsMultiple);
    }
    return state;
}

graph::Edge ReadEdge (const GS::ObjectState& params)
{
    GS::UniString sourceNode, sourcePort, targetNode, targetPort;
    params.Get ("sourceNode", sourceNode);
    params.Get ("sourcePort", sourcePort);
    params.Get ("targetNode", targetNode);
    params.Get ("targetPort", targetPort);
    return { Utf8 (sourceNode), Utf8 (sourcePort), Utf8 (targetNode), Utf8 (targetPort) };
}

class GateFreeGraphCommand : public MainThreadCommand {
  public:
    bool NeedsMainThread () const override
    {
        return false;
    }
};

class GraphGetNodeTypesCommand : public GateFreeGraphCommand {
  public:
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        GS::Array<GS::ObjectState> records;
        const graph::NodeRegistry registry = graph::GraphRuntimeState::Get ().Catalog ();
        for (const auto& [nodeTypeId, nodeType] : registry.Types ()) {
            GS::ObjectState record;
            record.Add ("nodeType", Text (nodeTypeId));
            record.Add ("label", Text (nodeType.label));
            record.Add ("category", Text (nodeType.category));
            record.Add ("description", Text (nodeType.description));
            record.Add ("executionDomain", DomainName (nodeType.executionDomain));
            GS::Array<GS::ObjectState> inputs, outputs, parameters;
            for (const graph::PortSchema& port : nodeType.inputs)
                inputs.Push (PortState (port, true));
            for (const graph::PortSchema& port : nodeType.outputs)
                outputs.Push (PortState (port, false));
            for (const graph::ParameterSchema& parameter : nodeType.parameters) {
                GS::ObjectState state;
                state.Add ("parameterId", Text (parameter.id));
                state.Add ("label", Text (parameter.label));
                state.Add ("valueType", ValueTypeName (parameter.valueType));
                state.Add ("required", parameter.required);
                parameters.Push (std::move (state));
            }
            record.Add ("inputs", inputs);
            record.Add ("outputs", outputs);
            record.Add ("parameters", parameters);
            records.Push (std::move (record));
        }
        GS::ObjectState response;
        response.Add ("nodeTypes", records);
        return response;
    }
};

class GraphGetStateCommand : public GateFreeGraphCommand {
  public:
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        const graph::GraphDocument document = graph::GraphRuntimeState::Get ().Document ();
        GS::Array<GS::ObjectState> nodes, edges;
        for (const auto& [nodeId, node] : document.Nodes ()) {
            GS::ObjectState record;
            record.Add ("nodeId", Text (nodeId));
            record.Add ("nodeType", Text (node.nodeType));
            GS::Array<GS::ObjectState> parameters;
            for (const auto& [parameterId, value] : node.parameters) {
                GS::ObjectState parameter;
                parameter.Add ("parameterId", Text (parameterId));
                parameter.Add ("valueType", ValueTypeName (value.Type ()));
                if (value.Type () == graph::ValueType::Double)
                    parameter.Add ("numberValue", std::get<double> (value.DataValue ()));
                parameters.Push (std::move (parameter));
            }
            record.Add ("parameters", parameters);
            nodes.Push (std::move (record));
        }
        for (const graph::Edge& edge : document.Edges ()) {
            GS::ObjectState record;
            record.Add ("sourceNode", Text (edge.sourceNode));
            record.Add ("sourcePort", Text (edge.sourcePort));
            record.Add ("targetNode", Text (edge.targetNode));
            record.Add ("targetPort", Text (edge.targetPort));
            edges.Push (std::move (record));
        }
        GS::ObjectState response;
        response.Add ("revision", static_cast<GS::Int64> (document.Revision ()));
        response.Add ("nodes", nodes);
        response.Add ("edges", edges);
        return response;
    }
};

class GraphApplyEditCommand : public GateFreeGraphCommand {
  public:
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString editKindValue;
        params.Get ("editKind", editKindValue);
        const std::string editKind = Utf8 (editKindValue);
        graph::GraphEdit edit;
        if (editKind == "addNode") {
            GS::UniString nodeId, nodeType;
            params.Get ("nodeId", nodeId);
            params.Get ("nodeType", nodeType);
            graph::Node node { Utf8 (nodeId), Utf8 (nodeType) };
            double numberValue = 0.0;
            if (params.Get ("numberValue", numberValue))
                node.parameters.emplace ("value", graph::Value (numberValue));
            edit.data = graph::AddNodeEdit { std::move (node) };
        }
        else if (editKind == "removeNode") {
            GS::UniString nodeId;
            params.Get ("nodeId", nodeId);
            edit.data = graph::RemoveNodeEdit { Utf8 (nodeId) };
        }
        else if (editKind == "connect")
            edit.data = graph::ConnectEdit { ReadEdge (params) };
        else if (editKind == "disconnect")
            edit.data = graph::DisconnectEdit { ReadEdge (params) };
        else {
            GS::UniString nodeId, parameterId;
            double numberValue = 0.0;
            params.Get ("nodeId", nodeId);
            params.Get ("parameterId", parameterId);
            params.Get ("numberValue", numberValue);
            edit.data = graph::SetParameterEdit { Utf8 (nodeId), Utf8 (parameterId), graph::Value (numberValue) };
        }

        const graph::EditResult result = graph::GraphRuntimeState::Get ().Apply (edit);
        if (!result.accepted)
            return NativeCommandResult::Failure (Text (result.error));
        GS::Array<GS::UniString> dirtyNodes;
        for (const graph::NodeId& nodeId : result.dirtyNodes)
            dirtyNodes.Push (Text (nodeId));
        GS::ObjectState response;
        response.Add ("revision", static_cast<GS::Int64> (result.revision));
        response.Add ("dirtyNodes", dirtyNodes);
        return response;
    }
};

class GraphEvaluateCommand : public GateFreeGraphCommand {
  public:
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        const graph::EvaluationSummary summary = graph::GraphRuntimeState::Get ().Evaluate ();
        if (!summary.succeeded)
            return NativeCommandResult::Failure (Text (summary.error));
        GS::ObjectState response;
        response.Add ("revision", static_cast<GS::Int64> (summary.revision));
        response.Add ("nodes", static_cast<GS::Int32> (summary.nodeCount));
        return response;
    }
};

class GraphGetNodeResultsCommand : public GateFreeGraphCommand {
  public:
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        const graph::ResultsSnapshot snapshot = graph::GraphRuntimeState::Get ().Results ();
        GS::Array<GS::ObjectState> results;
        for (const graph::RuntimeNodeResult& nodeResult : snapshot.nodes) {
            const graph::NodeStatus& status = nodeResult.status;
            const std::shared_ptr<const graph::NodeResult>& result = nodeResult.result;
            GS::ObjectState record;
            record.Add ("nodeId", Text (nodeResult.nodeId));
            record.Add ("status", StatusName (status.state));
            record.Add ("message", Text (status.message));
            record.Add ("durationMilliseconds", result ? result->durationMilliseconds : 0.0);
            record.Add ("previewAvailable", false);
            GS::Array<GS::ObjectState> outputs;
            GS::Int32 itemCount = 0;
            if (result) {
                for (const auto& [portId, value] : result->outputs) {
                    GS::ObjectState output;
                    output.Add ("portId", Text (portId));
                    output.Add ("valueType", ValueTypeName (value.Type ()));
                    if (value.Type () == graph::ValueType::Double) {
                        output.Add ("numberValue", std::get<double> (value.DataValue ()));
                        ++itemCount;
                    }
                    else if (value.Type () == graph::ValueType::List) {
                        const auto size = std::get<graph::Value::List> (value.DataValue ()).size ();
                        output.Add ("itemCount", static_cast<GS::Int32> (size));
                        itemCount += static_cast<GS::Int32> (size);
                    }
                    outputs.Push (std::move (output));
                }
            }
            record.Add ("itemCount", itemCount);
            record.Add ("outputs", outputs);
            results.Push (std::move (record));
        }
        GS::ObjectState response;
        response.Add ("revision", static_cast<GS::Int64> (snapshot.revision));
        response.Add ("results", results);
        return response;
    }
};

const NativeCommandRegistration registrations[] = {
    { "GraphGetNodeTypes", &MakeRegisteredNativeCommand<GraphGetNodeTypesCommand>, false, kEmptyInputSchema,
      kCatalogResponseSchema },
    { "GraphGetState", &MakeRegisteredNativeCommand<GraphGetStateCommand>, false, kEmptyInputSchema,
      kStateResponseSchema },
    { "GraphApplyEdit", &MakeRegisteredNativeCommand<GraphApplyEditCommand>, false, kEditInputSchema,
      kEditResponseSchema },
    { "GraphEvaluate", &MakeRegisteredNativeCommand<GraphEvaluateCommand>, false, kEmptyInputSchema,
      kEvaluateResponseSchema },
    { "GraphGetNodeResults", &MakeRegisteredNativeCommand<GraphGetNodeResultsCommand>, false, kEmptyInputSchema,
      kResultsResponseSchema },
};

} // namespace

NativeCommandRegistrations GetNodeGraphCommandRegistrations ()
{
    return MakeRegistrationView (registrations);
}

} // namespace geomsrv
