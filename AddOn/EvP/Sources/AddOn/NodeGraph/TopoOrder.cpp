#include "NodeGraph/TopoOrder.hpp"

#include <map>
#include <set>

namespace evp::nodegraph {

TopoResult BuildTopoOrder (const GraphDocument& document)
{
    std::map<NodeId, size_t> inDegree;
    std::map<NodeId, std::vector<NodeId>> adjacency;
    for (const auto& [nodeId, node] : document.Nodes ()) {
        (void) node;
        inDegree[nodeId] = 0;
    }
    for (const Edge& edge : document.Edges ()) {
        if (!inDegree.contains (edge.sourceNode) || !inDegree.contains (edge.targetNode))
            continue;
        adjacency[edge.sourceNode].push_back (edge.targetNode);
        ++inDegree[edge.targetNode];
    }

    std::set<NodeId> ready;
    for (const auto& [nodeId, degree] : inDegree)
        if (degree == 0)
            ready.insert (nodeId);

    TopoResult result;
    while (!ready.empty ()) {
        const NodeId nodeId = *ready.begin ();
        ready.erase (ready.begin ());
        result.order.push_back (nodeId);
        for (const NodeId& target : adjacency[nodeId]) {
            if (--inDegree[target] == 0)
                ready.insert (target);
        }
    }

    if (result.order.size () != document.Nodes ().size ()) {
        for (const auto& [nodeId, degree] : inDegree)
            if (degree != 0)
                result.cyclicNodes.push_back (nodeId);
    }
    return result;
}

} // namespace evp::nodegraph
