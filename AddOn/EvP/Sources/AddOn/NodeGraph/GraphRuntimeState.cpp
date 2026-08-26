#include "NodeGraph/GraphRuntimeState.hpp"

#include "NodeGraph/BuiltinNodes.hpp"

namespace evp::nodegraph {

GraphRuntimeState& GraphRuntimeState::Get ()
{
    static GraphRuntimeState state;
    return state;
}

GraphRuntimeState::GraphRuntimeState () : registry_ (MakeBuiltinNodeRegistry ())
{
}

NodeRegistry GraphRuntimeState::Catalog () const
{
    std::lock_guard lock (mutex_);
    return registry_;
}

GraphDocument GraphRuntimeState::Document () const
{
    std::lock_guard lock (mutex_);
    return document_;
}

EditResult GraphRuntimeState::Apply (const GraphEdit& edit)
{
    std::lock_guard lock (mutex_);
    EditResult result = ApplyEdit (document_, registry_, edit);
    if (result.accepted)
        evaluator_.Invalidate (document_, result.dirtyNodes);
    return result;
}

EvaluationSummary GraphRuntimeState::Evaluate ()
{
    std::lock_guard lock (mutex_);
    EvaluationSummary summary;
    summary.revision = document_.Revision ();
    summary.nodeCount = document_.Nodes ().size ();
    summary.succeeded = evaluator_.Evaluate (document_, registry_, ExecuteBuiltinNode, summary.error);
    return summary;
}

ResultsSnapshot GraphRuntimeState::Results () const
{
    std::lock_guard lock (mutex_);
    ResultsSnapshot snapshot;
    snapshot.revision = document_.Revision ();
    for (const auto& [nodeId, node] : document_.Nodes ()) {
        (void) node;
        snapshot.nodes.push_back ({ nodeId, evaluator_.Status (nodeId), evaluator_.Result (nodeId) });
    }
    return snapshot;
}

} // namespace evp::nodegraph
