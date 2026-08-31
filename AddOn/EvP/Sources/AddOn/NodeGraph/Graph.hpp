#ifndef EVP_NODEGRAPH_GRAPH_HPP
#define EVP_NODEGRAPH_GRAPH_HPP

#include "NodeGraph/Value.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace evp::nodegraph {

using NodeId = std::string;
using PortId = std::string;
using GraphId = std::string;

// Run identity lives with the other identity types so the event stream can name
// a run without depending on the execution context that creates one.
using RunId = uint64_t;

constexpr RunId kNoRun = 0;

// Stage F: what the user has told the graph to do with this node, as opposed to
// what happened to it on the last run.
//
// ⚠️ A MODE IS DOCUMENT STATE AND A STATUS IS RUN STATE. They are separate types
// because they have separate lifetimes: a mode is authored, persists with the
// graph and survives a restart; a status is produced by an evaluation and is
// session-only. Collapsing them - "the node is disabled" as a status - is what
// makes a reload silently re-enable everything the user switched off.
enum class ExecutionMode {
    // Normal evaluation and caching.
    Enabled,

    // Does not execute and publishes no output. Consumers become Blocked with a
    // dependency reason rather than Error: nothing is broken.
    Disabled,

    // Does not execute; the type's declared input-to-output mappings forward
    // compatible values, so consumers still run. Legal ONLY for a type that
    // declares unambiguous mappings - see NodeType::bypassMappings.
    Bypassed,

    // Data Dam. The node executes, but its result is STAGED rather than
    // published; consumers keep seeing the last released value until an explicit
    // release promotes the staged one. Legal only for a hold-capable type.
    Holding,
};

const char* ExecutionModeName (ExecutionMode mode);

// Parses the wire spelling. False for anything else, so an unknown mode from a
// client is a rejection rather than a silent fall back to Enabled.
bool ParseExecutionMode (const std::string& name, ExecutionMode& mode);

struct Node {
    NodeId id;
    std::string nodeType;
    std::map<std::string, Value> parameters;

    // Persisted with the graph. See ExecutionMode.
    ExecutionMode executionMode = ExecutionMode::Enabled;
};

struct Edge {
    NodeId sourceNode;
    PortId sourcePort;
    NodeId targetNode;
    PortId targetPort;
};

class NodeRegistry;
struct GraphEdit;
struct EditResult;

class GraphDocument {
  public:
    const std::map<NodeId, Node>& Nodes () const
    {
        return nodes_;
    }
    const std::vector<Edge>& Edges () const
    {
        return edges_;
    }
    const Node* FindNode (const NodeId& nodeId) const;
    uint64_t Revision () const
    {
        return revision_;
    }

  private:
    friend EditResult ApplyEdit (GraphDocument&, const NodeRegistry&, const GraphEdit&);

    std::map<NodeId, Node> nodes_;
    std::vector<Edge> edges_;
    uint64_t revision_ = 0;
};

} // namespace evp::nodegraph

#endif
