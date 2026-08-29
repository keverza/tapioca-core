#include "NodeGraph/NodeExecution.hpp"

#include "NodeGraph/ArchicadNodes.hpp"
#include "NodeGraph/BuiltinNodes.hpp"

namespace evp::nodegraph {

NodeRegistry MakeRuntimeNodeRegistry ()
{
    NodeRegistry registry = MakeBuiltinNodeRegistry ();
    RegisterArchicadNodes (registry);
    return registry;
}

bool ExecuteRuntimeNode (const Node& node, const ValueMap& inputs, const NodeExecutionContext& context,
                         ValueMap& outputs, std::string& error)
{
    if (IsArchicadNodeType (node.nodeType))
        return ExecuteArchicadNode (node, inputs, context, outputs, error);
    return ExecuteBuiltinNode (node, inputs, context, outputs, error);
}

} // namespace evp::nodegraph
