#include "NodeGraph/NodeExecution.hpp"

#include "NodeGraph/ArchicadNodes.hpp"
#include "NodeGraph/BuiltinNodes.hpp"
#include "NodeGraph/GeometryNodes.hpp"

namespace evp::nodegraph {

NodeRegistry MakeRuntimeNodeRegistry ()
{
    NodeRegistry registry = MakeBuiltinNodeRegistry ();
    RegisterGeometryNodes (registry);
    RegisterArchicadNodes (registry);
    return registry;
}

bool ExecuteRuntimeNode (const Node& node, const ValueMap& inputs, const NodeExecutionContext& context,
                         ValueMap& outputs, std::string& error)
{
    if (IsArchicadNodeType (node.nodeType))
        return ExecuteArchicadNode (node, inputs, context, outputs, error);
    if (IsGeometryNodeType (node.nodeType))
        return ExecuteGeometryNode (node, inputs, context, outputs, error);
    return ExecuteBuiltinNode (node, inputs, context, outputs, error);
}

} // namespace evp::nodegraph
