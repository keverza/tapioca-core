#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/NodeGraphCameraCommands.hpp"

#include "NativeCommands/NodeGraphCommandSupport.hpp"

#include <string>

namespace geomsrv {
namespace {

// ---------------------------------------------------------------------------
// The camera-set node's four actions.
//
//   add       append Archicad's 3D window camera to the list
//   remove    drop the camera at `index`
//   restore   point Archicad's 3D window at the camera at `index`
//   clear     empty the list
//
// ⚠️ FOUR WORDS, NOT THE SELECTION SET'S FIVE, AND THE MISSING ONE IS THE
// POINT. There is no `update`: a selection has one obvious current value to
// replace the set with, and a camera list is built one viewpoint at a time -
// orbit, add, orbit, add. An Update button here would either do nothing or throw
// away every camera but the newest, and both are worse than not offering it.
//
// ⚠️ AN ACTION IS NOT AN EVALUATION. The same rule the selection actions are
// written under: a graph that moved the user's 3D window during a run would be a
// defect, which is what EvaluationRequest::allowSideEffects refuses. `restore`
// writes the host because the user pressed a button that says so.
//
// ⚠️ AND THE ACTION RUNS WHAT IT AFFECTS. Add without a following evaluation
// would capture a camera and leave every consumer showing the previous list
// until somebody pressed Evaluate. The runtime evaluates the terminal nodes
// downstream of this one and nothing else.
// ---------------------------------------------------------------------------

constexpr const char kCameraActionInputSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1},"nodeId":{"type":"string","minLength":1},"action":{"type":"string","enum":["add","remove","restore","clear"]},"index":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["nodeId","action"]})json";

constexpr const char kCameraActionResponseSchema[] =
    R"json({"type":"object","properties":{"ok":{"type":"boolean"},"error":{"type":"string"},"graphId":{"type":"string"},"nodeId":{"type":"string"},"action":{"type":"string"},"count":{"type":"integer","minimum":0},"changed":{"type":"integer","minimum":0},"threeDWindowInFront":{"type":"boolean"},"revision":{"type":"integer","minimum":0},"evaluated":{"type":"boolean"},"executedCount":{"type":"integer","minimum":0},"evaluationError":{"type":"string"}},"additionalProperties":false,"required":["ok","error","graphId","nodeId","action","count","changed","threeDWindowInFront","revision","evaluated","executedCount","evaluationError"]})json";

bool ReadAction (const GS::ObjectState& params, graph::GraphRuntimeState::CameraAction& action, std::string& name)
{
    GS::UniString text;
    if (!params.Get ("action", text))
        return false;
    name = GraphUtf8 (text);
    using Action = graph::GraphRuntimeState::CameraAction;
    if (name == "add")
        action = Action::Add;
    else if (name == "remove")
        action = Action::Remove;
    else if (name == "restore")
        action = Action::Restore;
    else if (name == "clear")
        action = Action::Clear;
    else
        return false;
    return true;
}

class GraphCameraActionCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString nodeIdText;
        if (!params.Get ("nodeId", nodeIdText) || nodeIdText.IsEmpty ())
            return NativeCommandResult::Failure (GS::UniString ("nodeId is required", CC_UTF8));

        graph::GraphRuntimeState::CameraAction action {};
        std::string actionName;
        if (!ReadAction (params, action, actionName))
            return NativeCommandResult::Failure (
                GS::UniString ("action must be add, remove, restore or clear", CC_UTF8));

        // Absent means zero, which only `add` and `clear` may rely on - and both
        // ignore it. Remove and Restore always send one, and the runtime refuses
        // an index past the end rather than clamping it.
        GS::Int64 index = 0;
        params.Get ("index", index);
        if (index < 0)
            return NativeCommandResult::Failure (GS::UniString ("index cannot be negative", CC_UTF8));

        const graph::GraphId graphId = ReadGraphIdParam (params);
        const graph::NodeId nodeId = GraphUtf8 (nodeIdText);
        const graph::GraphRuntimeState::CameraActionResult result =
            graph::GraphRuntimeState::Get ().ApplyCameraAction (graphId, nodeId, action, static_cast<size_t> (index));

        // A refused action is a REPORTED outcome, not a failed command: "the 3D
        // window is axonometric" is a sentence the editor draws beside the node,
        // and a command failure carries only a string.
        GS::ObjectState response;
        response.Add ("ok", result.ok);
        response.Add ("error", GraphText (result.error));
        response.Add ("graphId", GraphText (graphId));
        response.Add ("nodeId", GraphText (nodeId));
        response.Add ("action", GraphText (actionName));
        response.Add ("count", static_cast<GS::Int64> (result.count));
        response.Add ("changed", static_cast<GS::Int64> (result.changed));
        // Restore only, and a success with a caveat rather than an error: the
        // projection changed, but the window showing it is not the one in front.
        response.Add ("threeDWindowInFront", result.threeDWindowInFront);
        response.Add ("revision", static_cast<GS::Int64> (result.revision));
        response.Add ("evaluated", result.evaluation.has_value ());
        response.Add ("executedCount",
                      static_cast<GS::Int64> (result.evaluation.has_value () ? result.evaluation->executedCount : 0));
        // Reported separately from `error`, as with the selection set: the list
        // can change correctly and the graph downstream of it still fail, and
        // collapsing the two would make a successful capture look like a failure.
        response.Add ("evaluationError",
                      GraphText (result.evaluation.has_value () ? result.evaluation->error : std::string {}));
        return response;
    }
};

const NativeCommandRegistration registrations[] = {
    { "GraphCameraAction", &MakeRegisteredNativeCommand<GraphCameraActionCommand>, false, kCameraActionInputSchema,
      kCameraActionResponseSchema },
};

} // namespace

NativeCommandRegistrations GetNodeGraphCameraCommandRegistrations ()
{
    return MakeRegistrationView (registrations);
}

} // namespace geomsrv
