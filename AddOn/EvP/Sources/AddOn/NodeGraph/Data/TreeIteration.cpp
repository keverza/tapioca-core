#include "NodeGraph/Data/TreeIteration.hpp"

#include <algorithm>

namespace evp::nodegraph::data {
namespace {

bool WalksLists (PortAccess access)
{
    return access == PortAccess::Item || access == PortAccess::List;
}

// Clamping is the whole of longest-list matching: an input that has run out
// keeps handing back its last list or its last item rather than ending the
// walk. An input that should end the walk is shrunk before planning.
size_t ClampIndex (size_t index, size_t count)
{
    if (count == 0)
        return 0;
    return std::min (index, count - 1);
}

// Most lists, tie-break longest list, among the inputs that are walked. A
// Tree-access input never guides: it is consumed whole every iteration, so its
// shape says nothing about how many iterations there are.
size_t FindGuide (const std::vector<IterationInput>& inputs)
{
    size_t guide = 0;
    size_t bestLists = 0;
    size_t bestLongest = 0;
    bool found = false;

    for (size_t index = 0; index < inputs.size (); ++index) {
        if (!WalksLists (inputs[index].access))
            continue;

        const IDataTree& tree = *inputs[index].tree;
        size_t longest = 0;
        for (size_t listIndex = 0; listIndex < tree.ListCount (); ++listIndex)
            longest = std::max (longest, tree.ListAt (listIndex).Size ());

        const bool better =
            !found || tree.ListCount () > bestLists || (tree.ListCount () == bestLists && longest > bestLongest);
        if (better) {
            guide = index;
            bestLists = tree.ListCount ();
            bestLongest = longest;
            found = true;
        }
    }
    return guide;
}

} // namespace

const char* PortAccessName (PortAccess access)
{
    switch (access) {
        case PortAccess::Item:
            return "item";
        case PortAccess::List:
            return "list";
        case PortAccess::Tree:
            return "tree";
    }
    return "unknown";
}

const IDataList* CursorList (const IterationInput& input, const InputCursor& cursor)
{
    if (!cursor.present || input.tree == nullptr)
        return nullptr;
    if (cursor.listIndex >= input.tree->ListCount ())
        return nullptr;
    return &input.tree->ListAt (cursor.listIndex);
}

bool BuildIterationPlan (const std::vector<IterationInput>& inputs, const IterationPolicy& policy, IterationPlan& plan,
                         std::string& error)
{
    if (inputs.empty ()) {
        error = "An iteration plan needs at least one input";
        return false;
    }
    for (size_t index = 0; index < inputs.size (); ++index) {
        if (inputs[index].tree == nullptr) {
            error = "Input " + std::to_string (index) + " has no tree";
            return false;
        }
    }

    plan = IterationPlan {};
    plan.guideInput = FindGuide (inputs);

    // How many list steps the walk takes: the widest walked input. Tree-access
    // inputs are excluded, so a node whose inputs are all Tree access runs once.
    size_t listSteps = 0;
    bool anyWalked = false;
    for (const IterationInput& input : inputs) {
        if (!WalksLists (input.access))
            continue;
        anyWalked = true;
        listSteps = std::max (listSteps, input.tree->ListCount ());
    }

    if (!anyWalked) {
        Iteration iteration;
        iteration.outputPath = DataPath::Zero ();
        iteration.cursors.assign (inputs.size (), InputCursor {});
        plan.iterations.push_back (std::move (iteration));
        return true;
    }

    if (listSteps == 0) {
        // Every walked input is empty. If they are all OPTIONAL, the node still
        // runs once with nothing: a node whose only inputs are unwired optional
        // ones has always produced its answer from its parameters, and silently
        // not running it would turn a source node into a dead one. If any input
        // is required, there is genuinely nothing to compute and the walk is
        // empty.
        const bool allOptional = std::all_of (inputs.begin (), inputs.end (), [] (const IterationInput& input) {
            return !WalksLists (input.access) || input.requirement == InputRequirement::MayBeMissing;
        });
        if (!allOptional)
            return true;

        Iteration iteration;
        iteration.outputPath = DataPath::Zero ();
        iteration.cursors.assign (inputs.size (), InputCursor { 0, 0, false, false });
        plan.iterations.push_back (std::move (iteration));
        return true;
    }

    const IDataTree& guide = *inputs[plan.guideInput].tree;

    for (size_t listStep = 0; listStep < listSteps; ++listStep) {
        // Item steps for this list: the longest list any Item-access input has
        // here. List and Tree access consume a whole list or a whole tree, so
        // they contribute no item axis.
        size_t itemSteps = 0;
        bool anyItemAccess = false;
        bool anyLength = false;
        for (const IterationInput& input : inputs) {
            if (input.access != PortAccess::Item)
                continue;
            anyItemAccess = true;
            const size_t listCount = input.tree->ListCount ();
            const size_t here = listCount == 0 ? 0 : input.tree->ListAt (ClampIndex (listStep, listCount)).Size ();

            // ⚠️ THE TWO POLICIES DISAGREE ABOUT AN EMPTY INPUT, AND THAT IS
            // THE DISAGREEMENT, NOT AN EDGE CASE.
            //
            // Under Longest an empty input is SKIPPED, so it lengthens nothing
            // and the other inputs decide the count. That has to stay exactly
            // as it was: this is the rule every lifted node runs under, and an
            // unwired optional port is empty on almost every node in a graph -
            // counting it as length zero would silence them all.
            //
            // Under Shortest an empty input IS length zero, and the walk stops.
            // Zipping three items against nothing yields nothing; that is what
            // the word means, and skipping the empty input would quietly make
            // Shortest behave as Longest in the one case they most differ.
            // Reachable only through an explicit `tree.zip`, never through a
            // lift, so this cannot disturb an existing node.
            if (policy.itemMatch == ItemMatch::Shortest) {
                itemSteps = anyLength ? std::min (itemSteps, here) : here;
                anyLength = true;
                continue;
            }
            if (listCount == 0)
                continue;
            itemSteps = std::max (itemSteps, here);
            anyLength = true;
        }
        if (!anyItemAccess)
            itemSteps = 1;

        const DataPath outputPath =
            guide.ListCount () == 0 ? DataPath::Zero () : guide.Paths ()[ClampIndex (listStep, guide.ListCount ())];

        // Nothing to do on this branch, but the branch is still a fact.
        if (itemSteps == 0) {
            plan.emptyPaths.push_back (outputPath);
            continue;
        }

        for (size_t itemStep = 0; itemStep < itemSteps; ++itemStep) {
            Iteration iteration;
            iteration.outputPath = outputPath;
            iteration.cursors.reserve (inputs.size ());

            for (size_t inputIndex = 0; inputIndex < inputs.size (); ++inputIndex) {
                const IterationInput& input = inputs[inputIndex];
                InputCursor cursor;

                if (input.access == PortAccess::Tree) {
                    iteration.cursors.push_back (cursor);
                    continue;
                }

                const size_t listCount = input.tree->ListCount ();
                cursor.listIndex = ClampIndex (listStep, listCount);
                cursor.present = listCount > 0;

                if (input.access == PortAccess::List) {
                    if (!cursor.present && input.requirement != InputRequirement::MayBeMissing && iteration.satisfied) {
                        iteration.satisfied = false;
                        iteration.unsatisfiedInput = inputIndex;
                    }
                    iteration.cursors.push_back (cursor);
                    continue;
                }

                const IDataList* list = cursor.present ? &input.tree->ListAt (cursor.listIndex) : nullptr;
                const size_t itemCount = list == nullptr ? 0 : list->Size ();
                cursor.itemIndex = ClampIndex (itemStep, itemCount);
                cursor.present = itemCount > 0;
                cursor.isNull = cursor.present && list->IsNullAt (cursor.itemIndex);

                const bool missing = !cursor.present;
                const bool nullRefused = cursor.isNull && input.requirement == InputRequirement::MustExist;
                if ((missing && input.requirement != InputRequirement::MayBeMissing) || nullRefused) {
                    if (iteration.satisfied) {
                        iteration.satisfied = false;
                        iteration.unsatisfiedInput = inputIndex;
                    }
                }
                iteration.cursors.push_back (cursor);
            }

            plan.iterations.push_back (std::move (iteration));
        }
    }

    return true;
}

const char* ItemMatchName (ItemMatch match)
{
    switch (match) {
        case ItemMatch::Longest:
            return "longest";
        case ItemMatch::Shortest:
            return "shortest";
    }
    return "longest";
}

bool BuildIterationPlan (const std::vector<IterationInput>& inputs, IterationPlan& plan, std::string& error)
{
    return BuildIterationPlan (inputs, IterationPolicy {}, plan, error);
}

} // namespace evp::nodegraph::data
