#ifndef EVP_NODEGRAPH_DATA_TREEITERATION_HPP
#define EVP_NODEGRAPH_DATA_TREEITERATION_HPP

// How a node body with per-item logic is run over tree inputs (HANDOFF 8.2,
// 8.3, evidence in 56.9).
//
// THIS IS A POLICY, NOT A DEFAULT. §8.3 is explicit that `DataTree` performs no
// automatic matching: a node that evaluates per item declares that it wants this
// walk, in its registry entry, and a node that is tree-aware never asks for it.
// Nothing in DataTree/DataTreeOps calls into this file.
//
// The policy implemented here is the ordinal one, because it is the only
// matching family the reference actually ships (56.9):
//
//   * PATHS ARE NEVER COMPARED. Inputs are walked by integer index - list
//     index and item index - so two trees pair by position, not by name. Trying
//     to pair {0;1} with {0;1} across differently shaped trees is a different
//     policy, and it is not this one.
//   * SHORTER INPUTS CLAMP TO THEIR LAST ITEM, which is longest-list matching.
//     An input that should instead stop early is shrunk by the CALLER before
//     iterating (§8.3 puts shortest/longest in per-list operations, not in the
//     engine), so "shortest" is a visible step rather than a hidden mode.
//   * OUTPUT PATHS ARE ADOPTED FROM ONE GUIDE INPUT, never invented: the input
//     with the most lists (tie-break: the longest list). Grafting is the
//     caller's business afterwards.
//   * A PATH THAT PRODUCES NO ITERATION STILL PRODUCES A PATH. `emptyPaths`
//     carries them, so a node cannot quietly delete a branch by having nothing
//     to do on it (7.5).
//
// The engine plans; it does not run. It hands back what to read for each
// iteration and where the answer goes, so the evaluator keeps ownership of
// threading, cancellation and fault barriers.

#include "NodeGraph/Data/DataTree.hpp"

#include <string>
#include <vector>

namespace evp::nodegraph::data {

// What shape of the input a node body consumes. Orthogonal to the item type,
// and it does not change what crosses the wire: every edge carries a tree (8.2).
enum class PortAccess {
    Item,
    List,
    Tree,
};

const char* PortAccessName (PortAccess access);

// What the port tolerates at a site the walk lands on.
enum class InputRequirement {
    // A value must be there. A missing site or a null item skips the iteration
    // and writes a null item at the output site, so the output keeps its shape.
    MustExist,

    // A null item is a legal argument; a missing site still skips.
    MayBeNull,

    // Both are legal. The body runs with the argument marked absent.
    MayBeMissing,
};

struct IterationInput {
    // Never null. An input with no edge is the caller's problem to resolve
    // (from an internalised value) before planning: 7.5 keeps "absent" a port
    // state, not a tree state.
    const IDataTree* tree = nullptr;

    PortAccess access = PortAccess::Item;
    InputRequirement requirement = InputRequirement::MustExist;
};

// Where one input sits for one iteration.
struct InputCursor {
    size_t listIndex = 0;

    // Meaningless for List and Tree access.
    size_t itemIndex = 0;

    // False when the walk landed past the end of this input, and the port
    // tolerated it (MayBeMissing). The body must treat the argument as absent.
    bool present = true;

    // A present site holding no value. Only reachable when the port declared
    // MayBeNull or MayBeMissing.
    bool isNull = false;
};

struct Iteration {
    // Adopted from the guide input (see the header comment).
    DataPath outputPath;

    // One per declared input, in declaration order.
    std::vector<InputCursor> cursors;

    // False when a MustExist/MayBeNull port had nothing at its site. The body
    // must NOT run; the output site takes a null item. `unsatisfiedInput` names
    // the port, for the diagnostic.
    bool satisfied = true;
    size_t unsatisfiedInput = 0;
};

struct IterationPlan {
    std::vector<Iteration> iterations;

    // Paths the guide contributes that produce no iteration at all - an empty
    // list on the guide. The caller writes an empty list at each, so filtering
    // to nothing keeps the branch.
    std::vector<DataPath> emptyPaths;

    // Index of the input whose paths the output adopts. Meaningless when every
    // input is Tree access, in which case there is one iteration at {0}.
    size_t guideInput = 0;
};

// §8.3's named item-match policies, and the ONLY two the engine implements.
//
// ⚠️ NAMED, BECAUSE §8.3 FORBIDS GUESSING BETWEEN THEM. "Shortest" and
// "longest" are not two shades of one behaviour: pairing three points with two
// numbers yields two results or three, and no runtime-global default can be
// right for both. So the policy is a parameter of this one engine rather than a
// second engine, which is what makes the explicit `tree.zip` node and the
// implicit lift agree BY CONSTRUCTION instead of by imitation.
//
// A policy name is a cache and file-format contract: renaming one changes what
// a saved graph means. Add a policy here rather than reimplementing matching
// anywhere else.
enum class ItemMatch {
    // A short input keeps handing back its LAST item. The implicit rule every
    // lifted node uses, so that a scalar node fed 3 points and 1 number applies
    // that number three times rather than producing one result.
    Longest,

    // The walk stops at the shortest input. Pairing is then total - every
    // result used a real item from every input - which is what a node asking to
    // zip two collections means, and never repeats an item to fill.
    Shortest,
};

const char* ItemMatchName (ItemMatch match);

struct IterationPolicy {
    ItemMatch itemMatch = ItemMatch::Longest;
};

// Fails only on a caller error: no inputs, or an input with no tree. A tree
// being empty is not an error - it plans zero iterations.
bool BuildIterationPlan (const std::vector<IterationInput>& inputs, const IterationPolicy& policy, IterationPlan& plan,
                         std::string& error);

// Longest, which is the implicit rule for every lifted node.
bool BuildIterationPlan (const std::vector<IterationInput>& inputs, IterationPlan& plan, std::string& error);

// The list a cursor addresses, or nullptr when the cursor is absent.
const IDataList* CursorList (const IterationInput& input, const InputCursor& cursor);

} // namespace evp::nodegraph::data

#endif
