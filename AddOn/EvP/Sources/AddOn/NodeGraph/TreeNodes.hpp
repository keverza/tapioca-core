#ifndef EVP_NODEGRAPH_TREENODES_HPP
#define EVP_NODEGRAPH_TREENODES_HPP

// The `tree.*` node family: nodes whose whole job IS the shape of the data.
//
// Every other family in this directory computes a value and lets NodeLifting
// walk it over the trees - that is the rule, and NodeLifting.hpp explains why
// it has to be. This family is the declared exception (see NodeType::treeBody):
// a Flatten, a Graft, a Simplify run PER ITEM would be the identity function,
// because there is no shape left to change once the walk has already reduced
// the node to one item at a time. So these types set `treeBody` and receive the
// whole input trees at once, with no per-item loop at all.
//
// ⚠️ NOT ROUTED THROUGH ExecuteRuntimeNode. Every other family's Execute*
// function is what NodeExecution.cpp's dispatcher calls as the per-value body
// RunLiftedNode lifts. A tree-native type carries its own body on the
// NodeType, and RunLiftedNode short-circuits to it before lifting is even
// considered (see NodeLifting.cpp) - so this file is registration and bodies,
// and there is no ExecuteTreeNode to plug into that dispatcher.
//
// What is deliberately NOT here: tree.zip and tree.crossProduct. Lacing two
// trees together needs a named matching policy (short/longest/cross - the same
// question DataTreeOps.hpp §"What is deliberately NOT here" defers), and adding
// them ahead of that decision would bake in a guess instead of a policy.

#include "NodeGraph/NodeRegistry.hpp"

namespace evp::nodegraph {

void RegisterTreeNodes (NodeRegistry& registry);

bool IsTreeNodeType (const std::string& nodeTypeId);

} // namespace evp::nodegraph

#endif
