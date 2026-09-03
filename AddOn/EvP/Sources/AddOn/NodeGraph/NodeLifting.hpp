#ifndef EVP_NODEGRAPH_NODELIFTING_HPP
#define EVP_NODEGRAPH_NODELIFTING_HPP

// Running a per-value node body over tree-valued ports (HANDOFF §7.1.2 decision
// 1, §8.2, §8.3).
//
// An edge carries a tree. A node body takes one value per port and returns one
// value per port. This file is the join between the two: it derives each port's
// data contract from its declaration, plans the walk with BuildIterationPlan,
// calls the body once per iteration, and assembles what came back into output
// trees.
//
// ⚠️ THE BODIES DO NOT CHANGE, AND THAT IS THE POINT. About a hundred of them
// exist; rewriting each to loop over a tree would be a hundred chances to write
// the same loop slightly differently, and the loop is exactly the part that has
// to be identical everywhere. So the loop lives here, once, and a body stays a
// function from values to values.
//
// THE CONTRACT IS DERIVED, NOT RE-DECLARED. A port already says what it is:
// `ValueType::List` means it deals in lists, `ValueType::Absent` means it takes
// anything, `required` means a value must be there, `acceptsMultiple` means it
// fans in. Adding a second set of fields saying the same thing again would give
// every port two declarations that can disagree, so the mapping below is a
// function of the existing one. A port that later needs something the mapping
// cannot express (tree access, a null-tolerant argument) gains an explicit
// field then, and not before.

#include "NodeGraph/Data/AnyTree.hpp"
#include "NodeGraph/Data/TreeIteration.hpp"
#include "NodeGraph/Graph.hpp"
#include "NodeGraph/NodeType.hpp"

#include <functional>
#include <map>
#include <string>

namespace evp::nodegraph {

struct NodeExecutionContext;

using TreeMap = std::map<std::string, data::TreeValue>;
using ValueMap = std::map<std::string, Value>;

// ---- the derived port contract ---------------------------------------------

// `Absent` (wildcard) and `List` both become Any: neither says what the items
// are. Every other declaration names its item type exactly.
data::ItemType PortItemType (const PortSchema& port);

// A port declared `List` consumes or produces a whole list per iteration;
// everything else works one item at a time.
data::PortAccess PortAccessOf (const PortSchema& port);

// An optional input tolerates a missing site; a required one does not. No port
// declares null-tolerance yet, so a null argument to a required port skips the
// iteration rather than reaching the body as a defaulted value.
data::InputRequirement PortRequirement (const PortSchema& port);

// ---- values in, values out --------------------------------------------------

// One value as a tree, for an internalised parameter reaching a port with no
// edge. A `Value::List` becomes one list at {0}; anything else becomes one item
// at {0}. Fails when the value cannot be an item of `itemType`.
bool TreeFromValue (const Value& value, data::ItemType itemType, data::TreeValue& result, std::string& error);

// A tree as one value, for the places that still speak Value: the browser
// projection, the panel renderer, a bypass mapping. A tree of exactly one item
// projects to that item; anything else projects to a `Value::List` in canonical
// traversal order. `maxItems` bounds it - an inspector that renders a million
// items is a hang, not an inspector - and `truncated` says whether it bit.
Value ProjectTreeToValue (const data::TreeValue& tree, size_t maxItems, bool& truncated);
Value ProjectTreeToValue (const data::TreeValue& tree);

// Total items across every list, which is what a node's itemCount reports.
size_t TreeItemCount (const data::TreeValue& tree);

// What a client is sent for one published output (§7.1.2 decision 4): a bounded
// projection and the two facts that make it honest - how many items there
// really are, and whether it was cut. A node may publish a million items and
// the palette must stay usable in exactly that case, so the bound is the
// runtime's policy rather than each client's.
struct ProjectedOutput {
    Value value;
    size_t itemCount = 0;
    bool truncated = false;
};

ProjectedOutput ProjectOutput (const data::TreeValue& tree, size_t maxItems);

// ---- the lift ---------------------------------------------------------------

using NodeBodyExecutor =
    std::function<bool (const Node&, const ValueMap&, const NodeExecutionContext&, ValueMap&, std::string&)>;

struct LiftReport {
    size_t iterationCount = 0;

    // Iterations whose inputs did not satisfy their ports. The body did not run
    // and each output took a null item at that site, so the output keeps the
    // shape of the input that drove it.
    size_t skippedCount = 0;
};

// Runs `body` over `inputs` and assembles `outputs`. Returns false when the
// body failed, when a body returned an item of the wrong type for its port, or
// when the plan could not be built; `error` names the port and the site.
bool RunLiftedNode (const NodeType& nodeType, const Node& node, const TreeMap& inputs,
                    const NodeExecutionContext& context, const NodeBodyExecutor& body, TreeMap& outputs,
                    LiftReport& report, std::string& error);

} // namespace evp::nodegraph

#endif
