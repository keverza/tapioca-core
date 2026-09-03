#include "NodeGraph/NodeExecution.hpp"

#include "NodeGraph/ArchicadNodes.hpp"
#include "NodeGraph/BuiltinNodes.hpp"
#include "NodeGraph/GeometryNodes.hpp"
#include "NodeGraph/ListNodes.hpp"
#include "NodeGraph/ScriptNodes.hpp"
#include "NodeGraph/ScriptRuntime.hpp"
#include "NodeGraph/TreeNodes.hpp"

namespace evp::nodegraph {

NodeRegistry MakeRuntimeNodeRegistry ()
{
    NodeRegistry registry = MakeBuiltinNodeRegistry ();
    RegisterGeometryNodes (registry);
    RegisterListNodes (registry);
    RegisterArchicadNodes (registry);
    RegisterScriptNodes (registry);
    RegisterTreeNodes (registry);
    // Installed here rather than by a static initialiser, so the engine exists
    // exactly when a registry that has script nodes in it does - including in the
    // offline suite, which is the whole reason the JS engine is embedded rather
    // than borrowed from the WebView. Idempotent: it stores one pointer.
    InstallJavaScriptRuntime ();
    return registry;
}

bool ExecuteRuntimeNode (const Node& node, const ValueMap& inputs, const NodeExecutionContext& context,
                         ValueMap& outputs, std::string& error)
{
    if (IsArchicadNodeType (node.nodeType))
        return ExecuteArchicadNode (node, inputs, context, outputs, error);
    if (IsGeometryNodeType (node.nodeType))
        return ExecuteGeometryNode (node, inputs, context, outputs, error);
    if (IsListNodeType (node.nodeType))
        return ExecuteListNode (node, inputs, context, outputs, error);
    if (IsScriptNodeType (node.nodeType))
        return ExecuteScriptNode (node, inputs, context, outputs, error);
    return ExecuteBuiltinNode (node, inputs, context, outputs, error);
}

} // namespace evp::nodegraph
