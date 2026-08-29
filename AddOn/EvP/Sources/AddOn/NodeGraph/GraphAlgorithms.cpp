#include "NodeGraph/GraphAlgorithms.hpp"

#include <map>
#include <set>
#include <vector>

namespace evp::nodegraph {
namespace {

// One breadth-first walk, parameterized by direction. Iterative: see the header.
std::vector<NodeId> Closure (const GraphDocument& document, const std::vector<NodeId>& seeds, bool followUpstream)
{
    std::map<NodeId, std::vector<NodeId>> adjacency;
    for (const Edge& edge : document.Edges ()) {
        if (document.FindNode (edge.sourceNode) == nullptr || document.FindNode (edge.targetNode) == nullptr)
            continue;
        if (followUpstream)
            adjacency[edge.targetNode].push_back (edge.sourceNode);
        else
            adjacency[edge.sourceNode].push_back (edge.targetNode);
    }

    std::set<NodeId> visited;
    std::vector<NodeId> pending;
    for (const NodeId& seed : seeds) {
        if (document.FindNode (seed) != nullptr && visited.insert (seed).second)
            pending.push_back (seed);
    }

    std::vector<NodeId> result;
    while (!pending.empty ()) {
        const NodeId nodeId = pending.back ();
        pending.pop_back ();
        result.push_back (nodeId);
        const auto neighbours = adjacency.find (nodeId);
        if (neighbours == adjacency.end ())
            continue;
        for (const NodeId& neighbour : neighbours->second) {
            if (visited.insert (neighbour).second)
                pending.push_back (neighbour);
        }
    }
    return result;
}

} // namespace

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
    // Kahn, drained one whole level at a time so the level partition falls out
    // of the same pass rather than needing a second longest-path walk.
    while (!ready.empty ()) {
        std::vector<NodeId> level (ready.begin (), ready.end ());
        ready.clear ();
        for (const NodeId& nodeId : level) {
            result.order.push_back (nodeId);
            const auto targets = adjacency.find (nodeId);
            if (targets == adjacency.end ())
                continue;
            for (const NodeId& target : targets->second) {
                if (--inDegree[target] == 0)
                    ready.insert (target);
            }
        }
        result.levels.push_back (std::move (level));
    }

    if (result.order.size () != document.Nodes ().size ()) {
        for (const auto& [nodeId, degree] : inDegree)
            if (degree != 0)
                result.cyclicNodes.push_back (nodeId);
    }
    return result;
}

std::vector<NodeId> UpstreamClosure (const GraphDocument& document, const std::vector<NodeId>& targets)
{
    return Closure (document, targets, true);
}

std::vector<NodeId> DownstreamClosure (const GraphDocument& document, const std::vector<NodeId>& roots)
{
    return Closure (document, roots, false);
}

std::vector<NodeId> TerminalNodes (const GraphDocument& document)
{
    std::set<NodeId> hasOutgoing;
    for (const Edge& edge : document.Edges ()) {
        if (document.FindNode (edge.sourceNode) != nullptr)
            hasOutgoing.insert (edge.sourceNode);
    }
    std::vector<NodeId> result;
    for (const auto& [nodeId, node] : document.Nodes ()) {
        (void) node;
        if (!hasOutgoing.contains (nodeId))
            result.push_back (nodeId);
    }
    return result;
}

} // namespace evp::nodegraph
