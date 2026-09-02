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

// The parallel list of element TYPE IDS captured with the guids.
//
// ⚠️ CAPTURED, NOT LOOKED UP, AND THAT IS WHY THE NODE IS STILL Pure. The
// selection set's whole design is that evaluating it reads nothing from the
// host, so a graph does not go dirty every time the user clicks in the model.
// Grouping the set into per-type containers needs each element's type, which IS
// a host read - so it is done ONCE, on the button press that already crossed to
// the host, and stored beside the guids. The containers then draw from the
// document, offline, with no round trip and no re-evaluation.
//
// The honest cost: the types are as old as the capture. An element retyped in
// Archicad since is shown under its old container until the user presses Update
// - the same staleness the set itself already has, and visible in the same way.
extern const char* const kSelectionTypesParameter;

// The type id prefix every container node's id starts with; the remainder is an
// ElementTypeDescriptor::id. `archicad.container.wall`.
extern const char* const kElementContainerPrefix;

// The container node for `elementTypeId`, or an empty string when the catalog
// gives that type no container.
std::string ElementContainerNodeType (const std::string& elementTypeId);

// The element type a container node holds, or an empty string when the id is
// not a container node at all.
std::string ElementTypeOfContainerNode (const std::string& nodeTypeId);

// The set a selection node holds, read out of its stored parameter, and the
// same set on the way back in. Non-element entries are dropped rather than
// failing: the parameter is validated on the way in, so anything else in there
// came from a hand-edited file and losing it is better than refusing the graph.
std::vector<ArchicadElementRef> ElementsFromValue (const Value& value);
Value ValueFromElements (const std::vector<ArchicadElementRef>& elements);

// The same round trip for the captured type ids. A non-string entry reads back
// as the unclassified id rather than being dropped, so the list stays PARALLEL
// to the guids - a shifted type list would file every element under its
// neighbour's container, which looks plausible and is entirely wrong.
std::vector<std::string> TypesFromValue (const Value& value);
Value ValueFromTypes (const std::vector<std::string>& types);

void RegisterArchicadNodes (NodeRegistry& registry);

// Executes an Archicad-domain node. Returns false with `error` set for anything
// this family does not implement, so the dispatcher can tell "not mine" from
// "mine and it failed".
bool ExecuteArchicadNode (const Node& node, const ValueMap& inputs, const NodeExecutionContext& context,
                          ValueMap& outputs, std::string& error);

bool IsArchicadNodeType (const std::string& nodeTypeId);

} // namespace evp::nodegraph

#endif
