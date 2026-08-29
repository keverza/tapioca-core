#ifndef EVP_NODEGRAPH_GRAPHALGORITHMS_HPP
#define EVP_NODEGRAPH_GRAPHALGORITHMS_HPP

// DAG traversal for the graph runtime. Hand-rolled and iterative on purpose:
// ADR-007 adds no dependency for ~150 lines of stable traversal, and every walk
// here is a loop rather than a recursion so a pathological document cannot
// overflow the stack inside Archicad's process.

#include "NodeGraph/Graph.hpp"

#include <vector>

namespace evp::nodegraph {

struct TopoResult {
    // Every node of the document in a dependency-respecting order.
    std::vector<NodeId> order;

    // order, partitioned into groups whose members have no dependency on each
    // other. This is the unit of parallelism the worker pool consumes.
    std::vector<std::vector<NodeId>> levels;

    std::vector<NodeId> cyclicNodes;

    bool IsAcyclic () const
    {
        return cyclicNodes.empty ();
    }
};

TopoResult BuildTopoOrder (const GraphDocument& document);

// Every node `targets` transitively depend on, including the targets themselves.
// Unknown ids are ignored rather than reported: the caller validates targets.
std::vector<NodeId> UpstreamClosure (const GraphDocument& document, const std::vector<NodeId>& targets);

// Every node that transitively depends on `roots`, including the roots. This is
// the dirty set of an edit.
std::vector<NodeId> DownstreamClosure (const GraphDocument& document, const std::vector<NodeId>& roots);

// Nodes with no outgoing edge. The default evaluation target set: evaluating a
// document with no explicit target means "produce everything it produces",
// which is still narrower than "cook every node" once branches are disconnected.
std::vector<NodeId> TerminalNodes (const GraphDocument& document);

} // namespace evp::nodegraph

#endif
