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

// The selection-set node's type id and the parameter holding its elements.
// Named here because the command that operates the five buttons has to address
// both, and a second spelling of either is a bug waiting for a rename.
extern const char* const kSelectionSetNodeType;
extern const char* const kSelectionSetParameter;

// The set a selection node holds, read out of its stored parameter, and the
// same set on the way back in. Non-element entries are dropped rather than
// failing: the parameter is validated on the way in, so anything else in there
// came from a hand-edited file and losing it is better than refusing the graph.
std::vector<ArchicadElementRef> ElementsFromValue (const Value& value);
Value ValueFromElements (const std::vector<ArchicadElementRef>& elements);

void RegisterArchicadNodes (NodeRegistry& registry);

// Executes an Archicad-domain node. Returns false with `error` set for anything
// this family does not implement, so the dispatcher can tell "not mine" from
// "mine and it failed".
bool ExecuteArchicadNode (const Node& node, const ValueMap& inputs, const NodeExecutionContext& context,
                          ValueMap& outputs, std::string& error);

bool IsArchicadNodeType (const std::string& nodeTypeId);

} // namespace evp::nodegraph

#endif
