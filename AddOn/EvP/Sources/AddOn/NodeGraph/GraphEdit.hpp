#ifndef EVP_NODEGRAPH_GRAPHEDIT_HPP
#define EVP_NODEGRAPH_GRAPHEDIT_HPP

#include "NodeGraph/Graph.hpp"

#include <string>
#include <variant>
#include <vector>

namespace evp::nodegraph {

struct AddNodeEdit {
    Node node;
};

struct RemoveNodeEdit {
    NodeId nodeId;
};

struct RemoveElementsEdit {
    std::vector<NodeId> nodeIds;
    std::vector<Edge> edges;
};

struct ConnectEdit {
    Edge edge;
};

struct DisconnectEdit {
    Edge edge;
};

struct SetParameterEdit {
    NodeId nodeId;
    std::string parameterId;
    Value value;
};

// Stage F5: a mode change is an EDIT, not a side channel.
//
// ⚠️ IT GOES THROUGH ApplyEdit FOR THE SAME REASON A WIRE DOES. Disabling a node
// changes what the graph computes exactly as deleting an edge does, so it has to
// be validated against the registry, rejected atomically, bump the revision and
// return the dirty closure. A separate "set mode" call that skipped that
// boundary would be a second way to mutate the document, with its own answer to
// every question this one has already settled.
struct SetExecutionModeEdit {
    NodeId nodeId;
    ExecutionMode mode = ExecutionMode::Enabled;
};

// Stage F4's release. An edit for the same reason: it changes what consumers
// read and therefore what the graph computes.
//
// ⚠️ ApplyEdit CANNOT FULLY VALIDATE THIS ONE. Whether there is anything staged
// is evaluator state, not document state, so GraphRuntimeState::Apply asks the
// evaluator BEFORE calling here - see Evaluator::CanRelease. What ApplyEdit
// checks is the half it can see: the node exists and is actually holding.
struct ReleaseHoldingEdit {
    NodeId nodeId;
};

struct GraphEdit {
    using Data = std::variant<AddNodeEdit, RemoveNodeEdit, RemoveElementsEdit, ConnectEdit, DisconnectEdit,
                              SetParameterEdit, SetExecutionModeEdit, ReleaseHoldingEdit>;
    Data data;
};

struct EditResult {
    bool accepted = false;

    // Stable machine-readable rejection reason, empty when accepted. Prose moves
    // between builds and translations; a client's branch must not.
    std::string code;

    std::string error;
    std::vector<NodeId> dirtyNodes;
    uint64_t revision = 0;
};

class NodeRegistry;

EditResult ApplyEdit (GraphDocument& document, const NodeRegistry& registry, const GraphEdit& edit);

} // namespace evp::nodegraph

#endif
