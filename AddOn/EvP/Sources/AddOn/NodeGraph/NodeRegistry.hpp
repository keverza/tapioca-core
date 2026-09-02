#ifndef EVP_NODEGRAPH_NODEREGISTRY_HPP
#define EVP_NODEGRAPH_NODEREGISTRY_HPP

#include "NodeGraph/Graph.hpp"
#include "NodeGraph/NodeType.hpp"

#include <map>
#include <string>

namespace evp::nodegraph {

class NodeRegistry {
  public:
    bool Register (NodeType nodeType, std::string& error);
    const NodeType* Find (const std::string& nodeTypeId) const;
    const std::map<std::string, NodeType>& Types () const
    {
        return types_;
    }

  private:
    std::map<std::string, NodeType> types_;
};

const PortSchema* FindInput (const NodeType& nodeType, const std::string& portId);
const PortSchema* FindOutput (const NodeType& nodeType, const std::string& portId);
const ParameterSchema* FindParameter (const NodeType& nodeType, const std::string& parameterId);

// ---------------------------------------------------------------------------
// The ONE way to ask what ports a node actually has.
//
// â ï¸ READ PORTS THROUGH THESE, NEVER OFF NodeType DIRECTLY, anywhere a
// concrete node is in hand. The type-only overloads above stay because the
// CATALOG has no node to consult - a client drawing the picker is asking what a
// type looks like before one exists. Every other reader has a node, and a reader
// that skips these silently sees zero ports on a script node: its edges validate
// against an empty list, its inputs are never gathered, and its outputs are all
// rejected as invalid. That failure is quiet, which is why the seam is narrow.
//
// A type that does not set instancePorts ignores whatever the node carries, so a
// hand-edited document cannot bolt ports onto an Add.
const std::vector<PortSchema>& ResolvedInputs (const Node& node, const NodeType& nodeType);
const std::vector<PortSchema>& ResolvedOutputs (const Node& node, const NodeType& nodeType);
const PortSchema* FindInput (const Node& node, const NodeType& nodeType, const std::string& portId);
const PortSchema* FindOutput (const Node& node, const NodeType& nodeType, const std::string& portId);

} // namespace evp::nodegraph

#endif
