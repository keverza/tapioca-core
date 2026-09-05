#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/NodeGraphEditCommands.hpp"

#include "NativeCommands/NodeGraphCommandSupport.hpp"
#include "NativeCommands/NodeGraphValueEncoding.hpp"

#include "NodeGraph/GraphRuntimeState.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

// Everything that MOVES the graph document: one edit, a batched all-or-nothing
// transaction, undo and redo, and the eraser.
//
// ⚠️ SPLIT OUT OF NodeGraphCommands.cpp RATHER THAN GRANTED AN EXCEPTION. That
// file crossed the architecture check's line cap when the transaction verbs
// landed, and the cap is there to stop exactly this file becoming the place
// every graph verb goes. The seam is real rather than arbitrary: these are the
// verbs that take the document lock and bump the revision, and they share one
// decoder, one response shape and one rejection envelope. The QUERY verbs -
// catalog, state, results, events, dependencies - stay next door and read.

namespace geomsrv {
namespace {

constexpr const char kGraphInputSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1}},"additionalProperties":false,"required":[]})json";

constexpr const char kEditInputSchema[] =
    R"json({"oneOf":[{"type":"object","properties":{"graphId":{"type":"string","minLength":1},"editKind":{"const":"addNode"},"nodeId":{"type":"string","minLength":1},"alias":{"type":"string","minLength":1,"maxLength":120},"nodeType":{"type":"string","minLength":1},"parameters":{"type":"array","items":{"type":"object","properties":{"parameterId":{"type":"string","minLength":1},"value":{"$ref":"#/$defs/parameterValue"}},"additionalProperties":false,"required":["parameterId","value"]}},"numberValue":{"type":"number"},"label":{"type":"string","maxLength":120},"coalesceKey":{"type":"string","maxLength":200}},"additionalProperties":false,"required":["editKind","nodeType"]},{"type":"object","properties":{"graphId":{"type":"string","minLength":1},"editKind":{"const":"removeNode"},"nodeId":{"type":"string","minLength":1},"label":{"type":"string","maxLength":120},"coalesceKey":{"type":"string","maxLength":200}},"additionalProperties":false,"required":["editKind","nodeId"]},{"type":"object","properties":{"graphId":{"type":"string","minLength":1},"editKind":{"enum":["connect","disconnect"]},"sourceNode":{"type":"string","minLength":1},"sourcePort":{"type":"string","minLength":1},"targetNode":{"type":"string","minLength":1},"targetPort":{"type":"string","minLength":1},"label":{"type":"string","maxLength":120},"coalesceKey":{"type":"string","maxLength":200}},"additionalProperties":false,"required":["editKind","sourceNode","sourcePort","targetNode","targetPort"]},{"type":"object","properties":{"graphId":{"type":"string","minLength":1},"editKind":{"const":"setParam"},"nodeId":{"type":"string","minLength":1},"parameterId":{"type":"string","minLength":1},"value":{"$ref":"#/$defs/parameterValue"},"numberValue":{"type":"number"},"label":{"type":"string","maxLength":120},"coalesceKey":{"type":"string","maxLength":200}},"additionalProperties":false,"required":["editKind","nodeId","parameterId"]},{"type":"object","properties":{"graphId":{"type":"string","minLength":1},"editKind":{"const":"setExecutionMode"},"nodeId":{"type":"string","minLength":1},"mode":{"type":"string","enum":["enabled","disabled","bypassed","holding"]},"label":{"type":"string","maxLength":120},"coalesceKey":{"type":"string","maxLength":200}},"additionalProperties":false,"required":["editKind","nodeId","mode"]},{"type":"object","properties":{"graphId":{"type":"string","minLength":1},"editKind":{"const":"setPortModifier"},"nodeId":{"type":"string","minLength":1},"portId":{"type":"string","minLength":1},"modifier":{"type":"string","enum":["none","flatten","graft","simplify","reverse","round","normalise"]},"label":{"type":"string","maxLength":120},"coalesceKey":{"type":"string","maxLength":200}},"additionalProperties":false,"required":["editKind","nodeId","portId","modifier"]},{"type":"object","properties":{"graphId":{"type":"string","minLength":1},"editKind":{"const":"releaseHolding"},"nodeId":{"type":"string","minLength":1},"label":{"type":"string","maxLength":120},"coalesceKey":{"type":"string","maxLength":200}},"additionalProperties":false,"required":["editKind","nodeId"]}],"$defs":{"leafValue":{"type":"object","properties":{"valueType":{"type":"string","enum":["absent","bool","integer","double","string","point3","polyline","polygon","mesh","archicadElementRef","list"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"}},"itemCount":{"type":"integer","minimum":0},"truncated":{"type":"boolean"}},"additionalProperties":false,"required":["valueType"]},"value":{"type":"object","properties":{"valueType":{"type":"string","enum":["absent","bool","integer","double","string","point3","polyline","polygon","mesh","archicadElementRef","list"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"}},"indices":{"type":"array","items":{"type":"integer","minimum":0}},"itemCount":{"type":"integer","minimum":0},"truncated":{"type":"boolean"},"items":{"type":"array","items":{"$ref":"#/$defs/leafValue"}}},"additionalProperties":false,"required":["valueType"]},"parameterValue":{"type":"object","properties":{"valueType":{"type":"string","enum":["bool","integer","double","string","point3","archicadElementRef"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"additionalProperties":false,"required":["valueType"]}}})json";
constexpr const char kEditResponseSchema[] =
    R"json({"type":"object","properties":{"revision":{"type":"integer","minimum":1},"dirtyNodes":{"type":"array","items":{"type":"string"}},"assignedNodes":{"type":"array","items":{"type":"object","properties":{"alias":{"type":"string"},"nodeId":{"type":"string","minLength":1}},"additionalProperties":false,"required":["alias","nodeId"]}}},"additionalProperties":false,"required":["revision","dirtyNodes","assignedNodes"]})json";
constexpr const char kEditsInputSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1},"expectedRevision":{"type":"integer","minimum":0},"label":{"type":"string","maxLength":120},"coalesceKey":{"type":"string","maxLength":200},"edits":{"type":"array","minItems":1,"maxItems":512,"items":{"oneOf":[{"type":"object","properties":{"editKind":{"const":"addNode"},"nodeId":{"type":"string","minLength":1},"alias":{"type":"string","minLength":1,"maxLength":120},"nodeType":{"type":"string","minLength":1},"parameters":{"type":"array","items":{"type":"object","properties":{"parameterId":{"type":"string","minLength":1},"value":{"$ref":"#/$defs/parameterValue"}},"additionalProperties":false,"required":["parameterId","value"]}},"numberValue":{"type":"number"}},"additionalProperties":false,"required":["editKind","nodeType"]},{"type":"object","properties":{"editKind":{"const":"removeNode"},"nodeId":{"type":"string","minLength":1}},"additionalProperties":false,"required":["editKind","nodeId"]},{"type":"object","properties":{"editKind":{"enum":["connect","disconnect"]},"sourceNode":{"type":"string","minLength":1},"sourcePort":{"type":"string","minLength":1},"targetNode":{"type":"string","minLength":1},"targetPort":{"type":"string","minLength":1}},"additionalProperties":false,"required":["editKind","sourceNode","sourcePort","targetNode","targetPort"]},{"type":"object","properties":{"editKind":{"const":"setParam"},"nodeId":{"type":"string","minLength":1},"parameterId":{"type":"string","minLength":1},"value":{"$ref":"#/$defs/parameterValue"},"numberValue":{"type":"number"}},"additionalProperties":false,"required":["editKind","nodeId","parameterId"]},{"type":"object","properties":{"editKind":{"const":"setExecutionMode"},"nodeId":{"type":"string","minLength":1},"mode":{"type":"string","enum":["enabled","disabled","bypassed","holding"]}},"additionalProperties":false,"required":["editKind","nodeId","mode"]},{"type":"object","properties":{"editKind":{"const":"setPortModifier"},"nodeId":{"type":"string","minLength":1},"portId":{"type":"string","minLength":1},"modifier":{"type":"string","enum":["none","flatten","graft","simplify","reverse","round","normalise"]}},"additionalProperties":false,"required":["editKind","nodeId","portId","modifier"]},{"type":"object","properties":{"editKind":{"const":"releaseHolding"},"nodeId":{"type":"string","minLength":1}},"additionalProperties":false,"required":["editKind","nodeId"]}]}}},"additionalProperties":false,"required":["edits"],"$defs":{"leafValue":{"type":"object","properties":{"valueType":{"type":"string","enum":["absent","bool","integer","double","string","point3","polyline","polygon","mesh","archicadElementRef","list"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"}},"itemCount":{"type":"integer","minimum":0},"truncated":{"type":"boolean"}},"additionalProperties":false,"required":["valueType"]},"value":{"type":"object","properties":{"valueType":{"type":"string","enum":["absent","bool","integer","double","string","point3","polyline","polygon","mesh","archicadElementRef","list"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"}},"indices":{"type":"array","items":{"type":"integer","minimum":0}},"itemCount":{"type":"integer","minimum":0},"truncated":{"type":"boolean"},"items":{"type":"array","items":{"$ref":"#/$defs/leafValue"}}},"additionalProperties":false,"required":["valueType"]},"parameterValue":{"type":"object","properties":{"valueType":{"type":"string","enum":["bool","integer","double","string","point3","archicadElementRef"]},"bool":{"type":"boolean"},"number":{"type":"number"},"text":{"type":"string"},"numbers":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"additionalProperties":false,"required":["valueType"]}}})json";
constexpr const char kHistoryDepthInputSchema[] =

    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1},"depth":{"type":"integer","minimum":1,"maximum":200}},"additionalProperties":false,"required":["depth"]})json";



constexpr const char kHistoryStateResponseSchema[] =
    R"json({"type":"object","properties":{"canUndo":{"type":"boolean"},"canRedo":{"type":"boolean"},"undoLabel":{"type":"string"},"redoLabel":{"type":"string"},"depth":{"type":"integer","minimum":1},"undoCount":{"type":"integer","minimum":0},"redoCount":{"type":"integer","minimum":0},"stepsRecorded":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["canUndo","canRedo","undoLabel","redoLabel","depth","undoCount","redoCount","stepsRecorded"]})json";
constexpr const char kEraseElementsInputSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1},"nodeIds":{"type":"array","items":{"type":"string","minLength":1}},"edges":{"type":"array","items":{"type":"object","properties":{"sourceNode":{"type":"string","minLength":1},"sourcePort":{"type":"string","minLength":1},"targetNode":{"type":"string","minLength":1},"targetPort":{"type":"string","minLength":1}},"additionalProperties":false,"required":["sourceNode","sourcePort","targetNode","targetPort"]}}},"additionalProperties":false,"required":["nodeIds","edges"]})json";
// Decode ONE edit object into a GraphEdit.
//
// ⚠️ SHARED BY THE SINGLE-EDIT VERB AND THE BATCHED TRANSACTION, ON PURPOSE. Two
// decoders would be two answers to "what is a legal edit", and the batch would
// quietly accept or refuse things the single verb does not. An edit expressible
// one way is expressible the other, by construction.
//
// Returns false and fills `error` rather than a NativeCommandResult, because a
// member of a batch is not a command result - the caller has to say WHICH edit
// refused before it can answer at all.
bool DecodeEdit (const GS::ObjectState& params, graph::GraphEdit& edit, std::string& error)
{
    GS::UniString editKindValue;
    params.Get ("editKind", editKindValue);
    const std::string editKind = GraphUtf8 (editKindValue);

    if (editKind == "addNode") {
        // ⚠️ `nodeId` IS OPTIONAL, AND CLIENTS SHOULD NOT SEND IT. An absent id
        // means the runtime names the node, which is what makes identity the
        // document's business rather than a browser's. It is still accepted so a
        // graph restored from a file keeps the names it was saved with; `alias`
        // is what a client uses to refer to a node it is creating in the same
        // transaction.
        GS::UniString nodeId, alias, nodeType;
        params.Get ("nodeId", nodeId);
        params.Get ("alias", alias);
        params.Get ("nodeType", nodeType);
        graph::Node node { GraphUtf8 (nodeId), GraphUtf8 (nodeType) };

        GS::Array<GS::ObjectState> parameters;
        if (params.Get ("parameters", parameters)) {
            for (const GS::ObjectState& parameter : parameters) {
                GS::UniString parameterId;
                GS::ObjectState valueState;
                graph::Value value;
                if (!parameter.Get ("parameterId", parameterId) || !parameter.Get ("value", valueState) ||
                    !DecodeParameterValue (valueState, value, error)) {
                    if (error.empty ())
                        error = "invalid parameter";
                    return false;
                }
                node.parameters.insert_or_assign (GraphUtf8 (parameterId), std::move (value));
            }
        }
        double numberValue = 0.0;
        if (params.Get ("numberValue", numberValue))
            node.parameters.insert_or_assign ("value", graph::Value (numberValue));
        edit.data = graph::AddNodeEdit { std::move (node), GraphUtf8 (alias) };
        return true;
    }
    if (editKind == "removeNode") {
        GS::UniString nodeId;
        params.Get ("nodeId", nodeId);
        edit.data = graph::RemoveNodeEdit { GraphUtf8 (nodeId) };
        return true;
    }
    if (editKind == "connect") {
        edit.data = graph::ConnectEdit { ReadEdge (params) };
        return true;
    }
    if (editKind == "disconnect") {
        edit.data = graph::DisconnectEdit { ReadEdge (params) };
        return true;
    }
    // Stage F5. Deliberately ON THIS ENDPOINT rather than as verbs of their
    // own: a mode change and a release move the document exactly as a wire
    // does, so they get the same validation, the same revision and the same
    // dirty-closure answer. Two endpoints would be two answers.
    if (editKind == "setExecutionMode") {
        GS::UniString nodeId, modeName;
        params.Get ("nodeId", nodeId);
        params.Get ("mode", modeName);
        graph::ExecutionMode mode = graph::ExecutionMode::Enabled;
        if (!graph::ParseExecutionMode (GraphUtf8 (modeName), mode)) {
            error = "unknown execution mode: " + GraphUtf8 (modeName);
            return false;
        }
        edit.data = graph::SetExecutionModeEdit { GraphUtf8 (nodeId), mode };
        return true;
    }
    // Here for the same reason setExecutionMode is: a modifier moves the
    // document exactly as a wire does, so it gets the same validation, the
    // same revision and the same dirty-closure answer.
    if (editKind == "setPortModifier") {
        GS::UniString nodeId, portId, modifierName;
        params.Get ("nodeId", nodeId);
        params.Get ("portId", portId);
        params.Get ("modifier", modifierName);
        graph::PortModifier modifier = graph::PortModifier::None;
        if (!graph::ParsePortModifier (GraphUtf8 (modifierName), modifier)) {
            error = "unknown port modifier: " + GraphUtf8 (modifierName);
            return false;
        }
        edit.data = graph::SetPortModifierEdit { GraphUtf8 (nodeId), GraphUtf8 (portId), modifier };
        return true;
    }
    if (editKind == "releaseHolding") {
        GS::UniString nodeId;
        params.Get ("nodeId", nodeId);
        edit.data = graph::ReleaseHoldingEdit { GraphUtf8 (nodeId) };
        return true;
    }

    GS::UniString nodeId, parameterId;
    params.Get ("nodeId", nodeId);
    params.Get ("parameterId", parameterId);
    graph::Value value;
    GS::ObjectState valueState;
    double numberValue = 0.0;
    if (params.Get ("value", valueState)) {
        if (!DecodeParameterValue (valueState, value, error))
            return false;
    }
    else if (params.Get ("numberValue", numberValue)) {
        value = graph::Value (numberValue);
    }
    else {
        error = "setParam requires a value";
        return false;
    }
    edit.data = graph::SetParameterEdit { GraphUtf8 (nodeId), GraphUtf8 (parameterId), std::move (value) };
    return true;
}
// The label and coalesce key a transaction carries into the undo stack. Both are
// the CLIENT's to choose: only it knows that two hundred setParam edits were one
// drag of one slider, and only it can name the gesture in the user's words.
void ReadHistoryHints (const GS::ObjectState& params, std::string& label, std::string& coalesceKey)
{
    GS::UniString labelValue, coalesceValue;
    if (params.Get ("label", labelValue))
        label = GraphUtf8 (labelValue);
    if (params.Get ("coalesceKey", coalesceValue))
        coalesceKey = GraphUtf8 (coalesceValue);
}
// One response shape for both edit verbs.
//
// `assignedNodes` IS THE ANSWER TO "WHAT IS MY NODE CALLED". A client no longer
// names the nodes it adds, so this is the only way it learns the ids the runtime
// chose - and the only way a paste can attach its layout to the right nodes.
// Always present, empty when the caller named everything itself.
GS::ObjectState EditResponse (uint64_t revision, const std::vector<graph::NodeId>& dirtyNodes,
                              const std::vector<std::pair<std::string, graph::NodeId>>& assignedNodes)
{
    GS::Array<GS::UniString> dirty;
    for (const graph::NodeId& nodeId : dirtyNodes)
        dirty.Push (GraphText (nodeId));

    GS::Array<GS::ObjectState> assigned;
    for (const auto& [alias, nodeId] : assignedNodes) {
        GS::ObjectState entry;
        entry.Add ("alias", GraphText (alias));
        entry.Add ("nodeId", GraphText (nodeId));
        assigned.Push (entry);
    }

    GS::ObjectState response;
    response.Add ("revision", static_cast<GS::Int64> (revision));
    response.Add ("dirtyNodes", dirty);
    response.Add ("assignedNodes", assigned);
    return response;
}
class GraphApplyEditCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        graph::GraphEdit edit;
        std::string error;
        if (!DecodeEdit (params, edit, error))
            return NativeCommandResult::Failure (GraphText (error));

        std::string label, coalesceKey;
        ReadHistoryHints (params, label, coalesceKey);

        const graph::EditResult result =
            graph::GraphRuntimeState::Get ().Apply (ReadGraphIdParam (params), edit, label, coalesceKey);
        if (!result.accepted) {
            // The code goes into the failure text until Phase 0 gives this
            // endpoint a structured rejection envelope. It is prefixed rather
            // than dropped so a rejection is already greppable, and so the
            // client migration to a `code` field is a parse change rather than
            // a runtime change.
            const std::string reported = result.code.empty () ? result.error : "[" + result.code + "] " + result.error;
            return NativeCommandResult::Failure (GraphText (reported));
        }
        std::vector<std::pair<std::string, graph::NodeId>> assigned;
        if (!result.assignedNodeId.empty ())
            assigned.emplace_back (std::string (), result.assignedNodeId);
        return EditResponse (result.revision, result.dirtyNodes, assigned);
    }
};
// A rejection a client can branch on: the stable code, the prose, and - when one
// member of the batch was the problem - which one.
NativeCommandResult BatchFailure (const graph::BatchEditResult& result)
{
    const std::string reported = result.code.empty () ? result.error : "[" + result.code + "] " + result.error;
    return NativeCommandResult::Failure (GraphText (reported));
}
class GraphApplyEditsCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Array<GS::ObjectState> editStates;
        params.Get ("edits", editStates);

        std::vector<graph::GraphEdit> edits;
        edits.reserve (editStates.GetSize ());
        for (USize index = 0; index < editStates.GetSize (); ++index) {
            graph::GraphEdit edit;
            std::string error;
            if (!DecodeEdit (editStates[index], edit, error))
                return NativeCommandResult::Failure (
                    GraphText ("edit " + std::to_string (index) + ": " + error));
            edits.push_back (std::move (edit));
        }

        // ⚠️ ABSENT MEANS "DO NOT CHECK", NOT "EXPECT ZERO". A caller that has no
        // revision to quote - a test, a script - must still be able to edit; a
        // zero default would refuse every one of them against a document that has
        // ever been touched.
        std::optional<uint64_t> expectedRevision;
        GS::Int64 expected = 0;
        if (params.Get ("expectedRevision", expected) && expected >= 0)
            expectedRevision = static_cast<uint64_t> (expected);

        std::string label, coalesceKey;
        ReadHistoryHints (params, label, coalesceKey);

        const graph::BatchEditResult result = graph::GraphRuntimeState::Get ().ApplyBatch (
            ReadGraphIdParam (params), expectedRevision, edits, label, coalesceKey);
        if (!result.accepted)
            return BatchFailure (result);
        return EditResponse (result.revision, result.dirtyNodes, result.assignedNodes);
    }
};
class GraphUndoCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        const graph::BatchEditResult result = graph::GraphRuntimeState::Get ().Undo (ReadGraphIdParam (params));
        if (!result.accepted)
            return BatchFailure (result);
        return EditResponse (result.revision, result.dirtyNodes, result.assignedNodes);
    }
};
class GraphRedoCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        const graph::BatchEditResult result = graph::GraphRuntimeState::Get ().Redo (ReadGraphIdParam (params));
        if (!result.accepted)
            return BatchFailure (result);
        return EditResponse (result.revision, result.dirtyNodes, result.assignedNodes);
    }
};
// One shape for both verbs that answer with history, so a field added here
// cannot reach one client and not the other.
GS::ObjectState EncodeHistory (const graph::HistoryState& state)
{
    GS::ObjectState response;
    response.Add ("canUndo", state.canUndo);
    response.Add ("canRedo", state.canRedo);
    response.Add ("undoLabel", GraphText (state.undoLabel));
    response.Add ("redoLabel", GraphText (state.redoLabel));
    // The value ACTUALLY in force, which is the clamped one - a client that
    // asked for 5000 needs to know it got 200 rather than show its request.
    response.Add ("depth", static_cast<GS::Int64> (state.depth));
    response.Add ("undoCount", static_cast<GS::Int64> (state.undoCount));
    response.Add ("redoCount", static_cast<GS::Int64> (state.redoCount));
    // See HistoryState::stepsRecorded: this is what lets a client interleave
    // its own metadata history with the document's.
    response.Add ("stepsRecorded", static_cast<GS::Int64> (state.stepsRecorded));
    return response;
}

class GraphGetHistoryCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        return EncodeHistory (graph::GraphRuntimeState::Get ().History (ReadGraphIdParam (params)));
    }
};
class GraphSetHistoryDepthCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Int64 depth = 0;
        if (!params.Get ("depth", depth) || depth < 0)
            return NativeCommandResult::Failure (GraphText ("depth must be a positive number of steps"));
        graph::GraphRuntimeState::Get ().SetHistoryDepth (ReadGraphIdParam (params), static_cast<size_t> (depth));
        return EncodeHistory (graph::GraphRuntimeState::Get ().History (ReadGraphIdParam (params)));
    }
};

class GraphEraseElementsCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Array<GS::UniString> nodeIds;
        GS::Array<GS::ObjectState> edgeStates;
        params.Get ("nodeIds", nodeIds);
        params.Get ("edges", edgeStates);

        graph::RemoveElementsEdit remove;
        remove.nodeIds.reserve (nodeIds.GetSize ());
        remove.edges.reserve (edgeStates.GetSize ());
        for (const GS::UniString& nodeId : nodeIds)
            remove.nodeIds.push_back (GraphUtf8 (nodeId));
        for (const GS::ObjectState& edge : edgeStates)
            remove.edges.push_back (ReadEdge (edge));

        const graph::EditResult result =
            graph::GraphRuntimeState::Get ().Apply (ReadGraphIdParam (params), graph::GraphEdit { std::move (remove) });
        if (!result.accepted)
            return NativeCommandResult::Failure (GraphText (result.error));

        GS::Array<GS::UniString> dirtyNodes;
        for (const graph::NodeId& nodeId : result.dirtyNodes)
            dirtyNodes.Push (GraphText (nodeId));
        GS::ObjectState response;
        response.Add ("revision", static_cast<GS::Int64> (result.revision));
        response.Add ("dirtyNodes", dirtyNodes);
        return response;
    }
};

const NativeCommandRegistration registrations[] = {
    { "GraphApplyEdit", &MakeRegisteredNativeCommand<GraphApplyEditCommand>, false, kEditInputSchema,
      kEditResponseSchema },
    { "GraphApplyEdits", &MakeRegisteredNativeCommand<GraphApplyEditsCommand>, false, kEditsInputSchema,
      kEditResponseSchema },
    { "GraphUndo", &MakeRegisteredNativeCommand<GraphUndoCommand>, false, kGraphInputSchema, kEditResponseSchema },
    { "GraphRedo", &MakeRegisteredNativeCommand<GraphRedoCommand>, false, kGraphInputSchema, kEditResponseSchema },
    { "GraphGetHistory", &MakeRegisteredNativeCommand<GraphGetHistoryCommand>, false, kGraphInputSchema,
      kHistoryStateResponseSchema },
    { "GraphSetHistoryDepth", &MakeRegisteredNativeCommand<GraphSetHistoryDepthCommand>, false,
      kHistoryDepthInputSchema, kHistoryStateResponseSchema },
    { "GraphEraseElements", &MakeRegisteredNativeCommand<GraphEraseElementsCommand>, false, kEraseElementsInputSchema,
      kEditResponseSchema },
};

} // namespace

NativeCommandRegistrations GetNodeGraphEditCommandRegistrations ()
{
    return MakeRegistrationView (registrations);
}

} // namespace geomsrv
