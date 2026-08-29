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

struct Node {
    NodeId id;
    std::string nodeType;
    std::map<std::string, Value> parameters;
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
