#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/NodeGraphSelectionCommands.hpp"

#include "NativeCommands/NodeGraphCommandSupport.hpp"

#include <string>

namespace geomsrv {
namespace {

// ---------------------------------------------------------------------------
// The selection-set node's five actions.
//
// The same vocabulary as the command palette's selection rows
// (Palette/SelectionSetPanel.cpp), deliberately: a user who has captured a
// selection for a command should not have to learn a second set of words to
// capture one for a graph.
//
//   update    replace the set with what is selected in Archicad now
//   add       add the current selection to the set
//   remove    take the current selection out of the set
//   reselect  select the set's elements in Archicad
//   clear     empty the set
//
// ⚠️ AN ACTION IS NOT AN EVALUATION, and the difference is the whole reason
// this is a verb rather than a node effect. A graph that reached out and changed
// the user's selection during a run is a defect - which is what
// EvaluationRequest::allowSideEffects refuses. A button the user pressed is a
// deliberate act, so `reselect` is permitted here and nowhere else.
//
// ⚠️ AND THE ACTION RUNS WHAT IT AFFECTS. Update without a following evaluation
// would change the set and leave every consumer showing the previous answer
// until somebody pressed Evaluate. The runtime evaluates the terminal nodes
// downstream of this one - not the whole document, which would cook unrelated
// branches.
// ---------------------------------------------------------------------------

constexpr const char kSelectionActionInputSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1},"nodeId":{"type":"string","minLength":1},"action":{"type":"string","enum":["update","add","remove","reselect","clear"]}},"additionalProperties":false,"required":["nodeId","action"]})json";

constexpr const char kSelectionActionResponseSchema[] =
    R"json({"type":"object","properties":{"ok":{"type":"boolean"},"error":{"type":"string"},"graphId":{"type":"string"},"nodeId":{"type":"string"},"action":{"type":"string"},"count":{"type":"integer","minimum":0},"changed":{"type":"integer","minimum":0},"missing":{"type":"array","items":{"type":"string"}},"revision":{"type":"integer","minimum":0},"evaluated":{"type":"boolean"},"executedCount":{"type":"integer","minimum":0},"evaluationError":{"type":"string"}},"additionalProperties":false,"required":["ok","error","graphId","nodeId","action","count","changed","missing","revision","evaluated","executedCount","evaluationError"]})json";

bool ReadAction (const GS::ObjectState& params, graph::GraphRuntimeState::SelectionAction& action,
                 std::string& name)
{
    GS::UniString text;
    if (!params.Get ("action", text))
        return false;
    name = GraphUtf8 (text);
    using Action = graph::GraphRuntimeState::SelectionAction;
    if (name == "update") action = Action::Update;
    else if (name == "add") action = Action::Add;
    else if (name == "remove") action = Action::Remove;
    else if (name == "reselect") action = Action::Reselect;
    else if (name == "clear") action = Action::Clear;
    else return false;
    return true;
}

class GraphSelectionActionCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString nodeIdText;
        if (!params.Get ("nodeId", nodeIdText) || nodeIdText.IsEmpty ())
            return NativeCommandResult::Failure (GS::UniString ("nodeId is required", CC_UTF8));

        graph::GraphRuntimeState::SelectionAction action {};
        std::string actionName;
        if (!ReadAction (params, action, actionName))
            return NativeCommandResult::Failure (
                GS::UniString ("action must be update, add, remove, reselect or clear", CC_UTF8));

        const graph::GraphId graphId = ReadGraphIdParam (params);
        const graph::NodeId nodeId = GraphUtf8 (nodeIdText);
        const graph::GraphRuntimeState::SelectionActionResult result =
            graph::GraphRuntimeState::Get ().ApplySelectionAction (graphId, nodeId, action);

        // A refused action is a REPORTED outcome, not a failed command: "the set
        // names three elements this project no longer has" is something the
        // editor renders beside the node, and a command failure carries only a
        // string.
        GS::Array<GS::UniString> missing;
        for (const std::string& guid : result.missing)
            missing.Push (GraphText (guid));

        GS::ObjectState response;
        response.Add ("ok", result.ok);
        response.Add ("error", GraphText (result.error));
        response.Add ("graphId", GraphText (graphId));
        response.Add ("nodeId", GraphText (nodeId));
        response.Add ("action", GraphText (actionName));
        response.Add ("count", static_cast<GS::Int64> (result.count));
        response.Add ("changed", static_cast<GS::Int64> (result.changed));
        response.Add ("missing", missing);
        response.Add ("revision", static_cast<GS::Int64> (result.revision));
        response.Add ("evaluated", result.evaluation.has_value ());
        response.Add ("executedCount",
                      static_cast<GS::Int64> (result.evaluation.has_value () ? result.evaluation->executedCount : 0));
        // Reported separately from `error`: the set can change correctly and the
        // graph downstream of it still fail, and collapsing the two would make a
        // successful capture look like a failed one.
        response.Add ("evaluationError",
                      GraphText (result.evaluation.has_value () ? result.evaluation->error : std::string {}));
        return response;
    }
};

const NativeCommandRegistration registrations[] = {
    { "GraphSelectionAction", &MakeRegisteredNativeCommand<GraphSelectionActionCommand>, false,
      kSelectionActionInputSchema, kSelectionActionResponseSchema },
};

} // namespace

NativeCommandRegistrations GetNodeGraphSelectionCommandRegistrations ()
{
    return MakeRegistrationView (registrations);
}

} // namespace geomsrv
