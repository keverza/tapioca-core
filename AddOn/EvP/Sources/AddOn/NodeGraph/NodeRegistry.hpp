#ifndef EVP_NODEGRAPH_NODEREGISTRY_HPP
#define EVP_NODEGRAPH_NODEREGISTRY_HPP

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

} // namespace evp::nodegraph

#endif
