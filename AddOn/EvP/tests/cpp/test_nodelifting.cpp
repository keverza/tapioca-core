// Offline gate for what a client is SENT about one published output
// (HANDOFF §7.1.2 decision 4, §7.3).
//
// A flat projection can say a node produced twelve walls. Only the branch view
// can say it produced four walls on each of three storeys - and those are the
// same `value` and different trees, so a client with only the flat view cannot
// tell a grafted result from a flattened one. That distinction is the entire
// reason the tree layer exists, which makes these assertions the ones that stop
// it being quietly invisible to every UI.
//
// The bounds are asserted as hard as the contents. An inspector that renders a
// million items is a hang rather than an inspector, and a per-branch budget
// would let ten thousand one-item branches slip the cap entirely.

#include "NodeGraph/NodeLifting.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace evp::nodegraph;
using namespace evp::nodegraph::data;

namespace {

DataPath P (std::initializer_list<DataPath::Segment> segments)
{
    return DataPath (segments);
}

// A tree of `branches` branches with `perBranch` integers in each.
TreeValue Grid (size_t branches, size_t perBranch)
{
    DataTreeBuilder<int64_t> builder;
    for (size_t branch = 0; branch < branches; ++branch) {
        for (size_t index = 0; index < perBranch; ++index)
            builder.Add (P ({ static_cast<DataPath::Segment> (branch) }), static_cast<int64_t> (index));
    }
    return MakeTreeValue<int64_t> (std::move (builder).Finish ());
}

const std::vector<Value>& Items (const Argument& value)
{
    return value.Items ();
}

} // namespace

TEST (ProjectOutput, ReportsTheBranchesAndNotJustTheFlattenedItems)
{
    DataTreeBuilder<int64_t> builder;
    builder.Add (P ({ 0, 0 }), 1);
    builder.Add (P ({ 0, 0 }), 2);
    builder.Add (P ({ 0, 1 }), 3);
    const TreeValue tree = MakeTreeValue<int64_t> (std::move (builder).Finish ());

    const ProjectedOutput projected = ProjectOutput (tree, 100, 100);
    EXPECT_EQ (projected.itemCount, 3u);
    EXPECT_EQ (projected.branchCount, 2u);
    EXPECT_FALSE (projected.truncated);
    EXPECT_FALSE (projected.branchesTruncated);
    ASSERT_EQ (projected.branches.size (), 2u);

    EXPECT_EQ (projected.branches[0].path, P ({ 0, 0 }));
    EXPECT_EQ (projected.branches[0].itemCount, 2u);
    EXPECT_EQ (projected.branches[1].path, P ({ 0, 1 }));
    EXPECT_EQ (projected.branches[1].itemCount, 1u);

    // The flat view is the same three items with the shape spent: a client
    // reading only `value` is told nothing false, only less.
    ASSERT_EQ (Items (projected.value).size (), 3u);
}

TEST (ProjectOutput, ABranchOfOneIsStillAListWhileTheWholeTreeOfOneIsNot)
{
    const TreeValue single = Grid (1, 1);
    const ProjectedOutput projected = ProjectOutput (single, 100, 100);

    // The flat view collapses: a consumer that asked for a value gets the value.
    EXPECT_EQ (projected.value.Type (), ValueType::Integer);

    // The branch view does NOT, because a branch IS a list and collapsing it
    // here would erase the one thing this view exists to report.
    ASSERT_EQ (projected.branches.size (), 1u);
    EXPECT_EQ (projected.branches[0].value.Type (), ValueType::List);
    EXPECT_EQ (Items (projected.branches[0].value).size (), 1u);
}

TEST (ProjectOutput, TheItemBudgetIsSpentAcrossBranchesNotPerBranch)
{
    // Ten branches of ten. A per-branch cap of 25 would let all 100 through.
    const ProjectedOutput projected = ProjectOutput (Grid (10, 10), 25, 100);

    EXPECT_EQ (projected.itemCount, 100u);  // Counted before the cap.
    EXPECT_EQ (projected.branchCount, 10u); // Every branch is still reported...
    ASSERT_EQ (projected.branches.size (), 10u);

    size_t sent = 0;
    for (const ProjectedBranch& branch : projected.branches) {
        EXPECT_EQ (branch.itemCount, 10u); // ...at its true length,
        sent += Items (branch.value).size ();
    }
    EXPECT_EQ (sent, 25u); // ...and the items stop at the budget.

    // The branch where the budget ran out says so, and the ones after it are
    // empty rather than absent: a branch that exists is not hidden by a cap.
    EXPECT_TRUE (projected.branches[2].truncated);
    EXPECT_TRUE (projected.branches[9].truncated);
    EXPECT_TRUE (Items (projected.branches[9].value).empty ());
}

TEST (ProjectOutput, TooManyBranchesAreCutAndSaidToBeCut)
{
    const ProjectedOutput projected = ProjectOutput (Grid (12, 1), 100, 5);
    EXPECT_EQ (projected.branchCount, 12u);
    EXPECT_TRUE (projected.branchesTruncated);
    EXPECT_EQ (projected.branches.size (), 5u);
}

TEST (ProjectOutput, AnEmptyTreeKeepsItsItemTypeAndHasNoBranches)
{
    // An empty tree of meshes and an empty tree of numbers project to the same
    // empty value, so the declared type is the only thing that tells them apart.
    const ProjectedOutput meshes = ProjectOutput (EmptyTreeValue (ItemType::Mesh), 100, 100);
    EXPECT_EQ (meshes.itemType, ItemType::Mesh);
    EXPECT_EQ (meshes.branchCount, 0u);
    EXPECT_TRUE (meshes.branches.empty ());
    EXPECT_EQ (meshes.itemCount, 0u);

    // An EMPTY BRANCH is not the same as no branch, and survives the projection
    // as a branch with no items (§7.5).
    DataTreeBuilder<double> builder;
    builder.EnsureList (P ({ 7 }));
    const ProjectedOutput empty = ProjectOutput (MakeTreeValue<double> (std::move (builder).Finish ()), 100, 100);
    EXPECT_EQ (empty.branchCount, 1u);
    ASSERT_EQ (empty.branches.size (), 1u);
    EXPECT_EQ (empty.branches[0].path, P ({ 7 }));
    EXPECT_EQ (empty.branches[0].itemCount, 0u);
    EXPECT_FALSE (empty.branches[0].truncated);
}

TEST (ProjectOutput, ANullItemCrossesAsAbsentAndNotAsAMissingRow)
{
    DataTreeBuilder<double> builder;
    builder.Add (P ({ 0 }), 1.5);
    builder.AddNull (P ({ 0 }));
    builder.Add (P ({ 0 }), 2.5);
    const ProjectedOutput projected = ProjectOutput (MakeTreeValue<double> (std::move (builder).Finish ()), 100, 100);

    ASSERT_EQ (projected.branches.size (), 1u);
    const std::vector<Value>& items = Items (projected.branches[0].value);
    ASSERT_EQ (items.size (), 3u); // Three sites, one of which holds nothing.
    EXPECT_EQ (items[1].Type (), ValueType::Absent);
    EXPECT_EQ (projected.branches[0].itemCount, 3u);
}
