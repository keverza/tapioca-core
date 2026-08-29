#ifndef EVP_NODEGRAPH_ARCHICADNODES_HPP
#define EVP_NODEGRAPH_ARCHICADNODES_HPP

// The Archicad node family.
//
// Written entirely against IArchicadHost, so it is DevKit-free and covered by
// the offline suite with a stub host. That is the point: the nodes - where the
// behaviour a user actually sees lives - are testable without Archicad, and the
// only untestable code is the thin ACAPI implementation of the interface.

#include "NodeGraph/Evaluator.hpp"
#include "NodeGraph/NodeRegistry.hpp"

namespace evp::nodegraph {

void RegisterArchicadNodes (NodeRegistry& registry);

// Executes an Archicad-domain node. Returns false with `error` set for anything
// this family does not implement, so the dispatcher can tell "not mine" from
// "mine and it failed".
bool ExecuteArchicadNode (const Node& node, const ValueMap& inputs, const NodeExecutionContext& context,
                          ValueMap& outputs, std::string& error);

bool IsArchicadNodeType (const std::string& nodeTypeId);

} // namespace evp::nodegraph

#endif
