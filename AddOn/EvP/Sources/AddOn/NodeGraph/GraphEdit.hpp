#ifndef EVP_NODEGRAPH_GRAPHEDIT_HPP
#define EVP_NODEGRAPH_GRAPHEDIT_HPP

#include "NodeGraph/Graph.hpp"

#include <map>
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

// A script node's file was read and its header parsed; this is the node catching
// up with what the file now says.
//
// â ï¸ IT IS AN EDIT FOR THE SAME REASON A WIRE IS. Reshaping a node's ports
// changes what the graph computes and what connections are legal, so it has to be
// validated against the registry, applied atomically, bump the revision and
// return the dirty closure. A "just poke the ports" path next to ApplyEdit would
// be a second way to mutate the document, with its own answer to every question
// this one has already settled - and it is the path that would run on a file
// watcher's thread, which is the worst possible place for a second answer.
//
// â ï¸ EDGES TO PORTS THAT NO LONGER FIT ARE DROPPED, AND THE USER IS TOLD.
// This is the one edit that removes connections the user did not ask to remove,
// because renaming an argument in VSCode is a thing people do without thinking
// about the canvas. The alternative - refusing the reload while any edge would
// break - leaves the node running yesterday's code with today's file on screen,
// which is worse and much harder to notice. `droppedEdges` carries what went, so
// the editor can say so rather than the wires just being gone.
struct SetScriptInterfaceEdit {
    NodeId nodeId;
    std::vector<PortSchema> inputs;
    std::vector<PortSchema> outputs;

    // Literal defaults from the header, applied as internalised input values for
    // ports the node does not already carry one for. Existing values are LEFT
    // ALONE: a number the user typed into the node outranks the default in the
    // file, exactly as it would for any other type in the catalog.
    std::map<std::string, Value> defaults;

    // Folded into the node's parameters so the evaluator's cache key changes when
    // the file does. Without it a saved script would reload, reshape nothing (the
    // ports being unchanged), and serve the previous run's cached result.
    std::string sourceHash;
};

struct GraphEdit {
    using Data = std::variant<AddNodeEdit, RemoveNodeEdit, RemoveElementsEdit, ConnectEdit, DisconnectEdit,
                              SetParameterEdit, SetExecutionModeEdit, ReleaseHoldingEdit, SetScriptInterfaceEdit>;
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

    // Edges this edit removed that the user did not name. Empty for every edit
    // but SetScriptInterfaceEdit, which is the only one that can do it - see
    // there for why it is allowed to at all.
    std::vector<Edge> droppedEdges;
};

class NodeRegistry;

EditResult ApplyEdit (GraphDocument& document, const NodeRegistry& registry, const GraphEdit& edit);

} // namespace evp::nodegraph

#endif
