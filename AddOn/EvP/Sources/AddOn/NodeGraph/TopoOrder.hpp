#ifndef EVP_NODEGRAPH_TOPOORDER_HPP
#define EVP_NODEGRAPH_TOPOORDER_HPP

#include "NodeGraph/Graph.hpp"

#include <vector>

namespace evp::nodegraph {

struct TopoResult {
    std::vector<NodeId> order;
    std::vector<NodeId> cyclicNodes;

    bool IsAcyclic () const
    {
        return cyclicNodes.empty ();
    }
};

TopoResult BuildTopoOrder (const GraphDocument& document);

} // namespace evp::nodegraph

#endif
