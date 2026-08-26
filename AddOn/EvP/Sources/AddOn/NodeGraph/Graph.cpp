#include "NodeGraph/Graph.hpp"

namespace evp::nodegraph {

const Node* GraphDocument::FindNode (const NodeId& nodeId) const
{
    const auto iterator = nodes_.find (nodeId);
    return iterator == nodes_.end () ? nullptr : &iterator->second;
}

} // namespace evp::nodegraph
