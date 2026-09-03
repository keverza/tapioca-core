#ifndef EVP_NODEGRAPH_LISTNODES_HPP
#define EVP_NODEGRAPH_LISTNODES_HPP

// Reading INTO one list: its length, one item out of it, its order.
//
// ⚠️ NOT THE SAME FAMILY AS `tree.*`, AND THE LINE BETWEEN THEM IS THE POINT.
// A `tree.*` node reshapes the collection itself - how many branches there are
// and which items sit in each - so it must see the whole tree and opts out of
// lifting (NodeType::treeBody). A `list.*` node does not care how many branches
// exist: it answers one question about ONE branch, and the runtime runs it once
// per branch for free. So these are ordinary value bodies with a
// `ValueType::List` port, and lifting does the rest.
//
// That is why a Length node wired to a grafted tree of forty branches reports
// forty ones rather than a single forty, with no code here saying so.

#include "NodeGraph/Evaluator.hpp"
#include "NodeGraph/NodeRegistry.hpp"

namespace evp::nodegraph {

void RegisterListNodes (NodeRegistry& registry);

bool ExecuteListNode (const Node& node, const ValueMap& inputs, const NodeExecutionContext& context, ValueMap& outputs,
                      std::string& error);

bool IsListNodeType (const std::string& nodeTypeId);

} // namespace evp::nodegraph

#endif
