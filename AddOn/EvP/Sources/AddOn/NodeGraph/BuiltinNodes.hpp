#ifndef EVP_NODEGRAPH_BUILTINNODES_HPP
#define EVP_NODEGRAPH_BUILTINNODES_HPP

#include "NodeGraph/Evaluator.hpp"
#include "NodeGraph/NodeRegistry.hpp"

namespace evp::nodegraph {

NodeRegistry MakeBuiltinNodeRegistry ();
bool ExecuteBuiltinNode (const Node& node, const ValueMap& inputs, const NodeExecutionContext& context,
                         ValueMap& outputs, std::string& error);

} // namespace evp::nodegraph

#endif
