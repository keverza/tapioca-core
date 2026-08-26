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

struct GraphEdit {
    using Data = std::variant<AddNodeEdit, RemoveNodeEdit, ConnectEdit, DisconnectEdit, SetParameterEdit>;
    Data data;
};

struct EditResult {
    bool accepted = false;
    std::string error;
    std::vector<NodeId> dirtyNodes;
    uint64_t revision = 0;
};

class NodeRegistry;

EditResult ApplyEdit (GraphDocument& document, const NodeRegistry& registry, const GraphEdit& edit);

} // namespace evp::nodegraph

#endif
