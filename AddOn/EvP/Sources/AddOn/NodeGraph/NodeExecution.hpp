#ifndef EVP_NODEGRAPH_NODEEXECUTION_HPP
#define EVP_NODEGRAPH_NODEEXECUTION_HPP

// The one executor the runtime installs: pure built-ins plus the Archicad
// family, dispatched by node type. Node families register here rather than the
// evaluator learning about each one.

#include "NodeGraph/Evaluator.hpp"
#include "NodeGraph/NodeRegistry.hpp"

namespace evp::nodegraph {

// Every node type the runtime serves.
NodeRegistry MakeRuntimeNodeRegistry ();

bool ExecuteRuntimeNode (const Node& node, const ValueMap& inputs, const NodeExecutionContext& context,
                         ValueMap& outputs, std::string& error);

} // namespace evp::nodegraph

#endif
