// Offline gate for the ordinal iteration policy (HANDOFF §8.2, §8.3).
//
// This is the file where "why did my component produce 47 results" is decided,
// and every wrong answer here is plausible rather than broken: an off-by-one in
// the clamp silently drops the last item of the longest input, a guide chosen
// by the wrong rule renames every output branch, an empty branch that produces
// no iteration silently disappears from the output, and a null argument that
// reaches a body expecting a value becomes a zero in the result. All of those
// produce a graph that runs.

#include "NodeGraph/Data/TreeIteration.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace evp::nodegraph;
using namespace evp::nodegraph::data;

namespace {

DataPath P (std::initializer_list<DataPath::Segment> segments)
{
    return DataPath (segments);
}

// One list per initializer entry, at consecutive paths {0}, {1}, ...
std::shared_ptr<const DataTree<int64_t>> Tree (std::initializer_list<std::vector<int64_t>> lists)
{
    DataTreeBuilder<int64_t> builder;
    DataPath::Segment path = 0;
    for (const std::vector<int64_t>& list : lists) {
        builder.EnsureList (DataPath ({ path }));
        for (int64_t value : list)
            builder.Add (DataPath ({ path }), value);
        ++path;
    }
    return std::move (builder).Finish ();
}

IterationInput In (const std::shared_ptr<const DataTree<int64_t>>& tree, PortAccess access = PortAccess::Item,
                   InputRequirement requirement = InputRequirement::MustExist)
{
    return IterationInput { tree.get (), access, requirement };
}

// The item an item-access cursor addresses, for readability in expectations.
int64_t ItemAt (const IterationInput& input, const InputCursor& cursor)
{
    const IDataList* list = CursorList (input, cursor);
    EXPECT_NE (list, nullptr);
    const std::optional<Value> value = list->ValueAt (cursor.itemIndex);
    EXPECT_TRUE (value.has_value ());
    return std::get<int64_t> (value->DataValue ());
}

IterationPlan Plan (const std::vector<IterationInput>& inputs)
{
    IterationPlan plan;
    std::string error;
    EXPECT_TRUE (BuildIterationPlan (inputs, plan, error)) << error;
    return plan;
}

} // namespace

TEST (TreeIteration, OneItemPerIterationOverOneList)
{
    const auto values = Tree ({ { 1, 2, 3 } });
    const std::vector<IterationInput> inputs { In (values) };
    const IterationPlan plan = Plan (inputs);

    ASSERT_EQ (plan.iterations.size (), 3u);
    EXPECT_TRUE (plan.emptyPaths.empty ());
    for (size_t index = 0; index < 3u; ++index) {
        EXPECT_EQ (plan.iterations[index].outputPath, P ({ 0 }));
        EXPECT_TRUE (plan.iterations[index].satisfied);
        EXPECT_EQ (ItemAt (inputs[0], plan.iterations[index].cursors[0]), static_cast<int64_t> (index) + 1);
    }
}

TEST (TreeIteration, ShorterInputsClampToTheirLastItem)
{
    // Longest-list matching, stated as a clamp: 3 items against 1 runs three
    // times, and the short input hands back its only item every time.
    const auto many = Tree ({ { 1, 2, 3 } });
    const auto one = Tree ({ { 10 } });
    const std::vector<IterationInput> inputs { In (many), In (one) };
    const IterationPlan plan = Plan (inputs);

    ASSERT_EQ (plan.iterations.size (), 3u);
    EXPECT_EQ (ItemAt (inputs[1], plan.iterations[0].cursors[1]), 10);
    EXPECT_EQ (ItemAt (inputs[1], plan.iterations[2].cursors[1]), 10);
    EXPECT_EQ (ItemAt (inputs[0], plan.iterations[2].cursors[0]), 3);
}

TEST (TreeIteration, ListsAdvanceOnlyWhenItemsAreExhausted)
{
    const auto left = Tree ({ { 1, 2 }, { 3 } });
    const std::vector<IterationInput> inputs { In (left) };
    const IterationPlan plan = Plan (inputs);

    ASSERT_EQ (plan.iterations.size (), 3u);
    EXPECT_EQ (plan.iterations[0].outputPath, P ({ 0 }));
    EXPECT_EQ (plan.iterations[1].outputPath, P ({ 0 }));
    EXPECT_EQ (plan.iterations[2].outputPath, P ({ 1 }));
    EXPECT_EQ (ItemAt (inputs[0], plan.iterations[2].cursors[0]), 3);
}

TEST (TreeIteration, PathsAreNeverComparedOnlyPositions)
{
    // The two trees share no path at all. Ordinal matching pairs them by
    // position regardless, which is the whole point of the policy - and the
    // reason a node that wants path matching must not ask for this one.
    DataTreeBuilder<int64_t> exotic;
    exotic.Add (P ({ 7, 4 }), 100);
    exotic.Add (P ({ 9, 9, 9 }), 200);

    const auto exoticTree = std::move (exotic).Finish ();
    const auto plain = Tree ({ { 1 }, { 2 } });
    const std::vector<IterationInput> inputs { In (plain), In (exoticTree) };
    const IterationPlan plan = Plan (inputs);

    ASSERT_EQ (plan.iterations.size (), 2u);
    EXPECT_EQ (ItemAt (inputs[1], plan.iterations[0].cursors[1]), 100);
    EXPECT_EQ (ItemAt (inputs[1], plan.iterations[1].cursors[1]), 200);
}

TEST (TreeIteration, OutputPathsComeFromTheWidestInput)
{
    const auto narrow = Tree ({ { 1 } });
    DataTreeBuilder<int64_t> wideBuilder;
    wideBuilder.Add (P ({ 4 }), 10);
    wideBuilder.Add (P ({ 5, 1 }), 20);
    const auto wide = std::move (wideBuilder).Finish ();

    const IterationPlan plan = Plan ({ In (narrow), In (wide) });
    EXPECT_EQ (plan.guideInput, 1u);
    ASSERT_EQ (plan.iterations.size (), 2u);
    EXPECT_EQ (plan.iterations[0].outputPath, P ({ 4 }));
    EXPECT_EQ (plan.iterations[1].outputPath, P ({ 5, 1 }));
}

TEST (TreeIteration, TheLongestListBreaksAGuideTie)
{
    const auto shortLists = Tree ({ { 1 }, { 2 } });
    DataTreeBuilder<int64_t> longerBuilder;
    longerBuilder.Add (P ({ 8 }), 1);
    longerBuilder.Add (P ({ 8 }), 2);
    longerBuilder.Add (P ({ 9 }), 3);
    const auto longer = std::move (longerBuilder).Finish ();

    const IterationPlan plan = Plan ({ In (shortLists), In (longer) });
    EXPECT_EQ (plan.guideInput, 1u);
    EXPECT_EQ (plan.iterations.front ().outputPath, P ({ 8 }));
}

TEST (TreeIteration, ListAccessConsumesAWholeListPerIteration)
{
    const auto lists = Tree ({ { 1, 2, 3 }, { 4 } });
    const std::vector<IterationInput> inputs { In (lists, PortAccess::List) };
    const IterationPlan plan = Plan (inputs);

    // One iteration per LIST, not per item.
    ASSERT_EQ (plan.iterations.size (), 2u);
    EXPECT_EQ (plan.iterations[0].outputPath, P ({ 0 }));
    const IDataList* first = CursorList (inputs[0], plan.iterations[0].cursors[0]);
    ASSERT_NE (first, nullptr);
    EXPECT_EQ (first->Size (), 3u);
}

TEST (TreeIteration, TreeAccessNeitherGuidesNorAdvances)
{
    const auto whole = Tree ({ { 1 }, { 2 }, { 3 } });
    const auto items = Tree ({ { 7, 8 } });

    const IterationPlan plan = Plan ({ In (whole, PortAccess::Tree), In (items) });
    EXPECT_EQ (plan.guideInput, 1u);         // The tree-access input does not guide...
    ASSERT_EQ (plan.iterations.size (), 2u); // ...and does not add iterations.
    EXPECT_EQ (plan.iterations[1].outputPath, P ({ 0 }));

    // Every input tree-access: the body runs exactly once, at {0}.
    const IterationPlan single = Plan ({ In (whole, PortAccess::Tree) });
    ASSERT_EQ (single.iterations.size (), 1u);
    EXPECT_EQ (single.iterations[0].outputPath, DataPath::Zero ());
}

TEST (TreeIteration, AnEmptyBranchStillProducesAPath)
{
    DataTreeBuilder<int64_t> builder;
    builder.Add (P ({ 0 }), 1);
    builder.EnsureList (P ({ 1 }));
    builder.Add (P ({ 2 }), 3);

    const auto tree = std::move (builder).Finish ();
    const IterationPlan plan = Plan ({ In (tree) });
    ASSERT_EQ (plan.iterations.size (), 2u);
    ASSERT_EQ (plan.emptyPaths.size (), 1u);
    EXPECT_EQ (plan.emptyPaths[0], P ({ 1 }));
}

TEST (TreeIteration, TheEmptyTreePlansNothing)
{
    const auto empty = DataTree<int64_t>::EmptyTree ();
    const IterationPlan plan = Plan ({ IterationInput { empty.get (), PortAccess::Item } });
    EXPECT_TRUE (plan.iterations.empty ());
    EXPECT_TRUE (plan.emptyPaths.empty ());
}

TEST (TreeIteration, ARequiredNullIsAnUnsatisfiedIterationNotADefault)
{
    DataTreeBuilder<int64_t> builder;
    builder.Add (P ({ 0 }), 1);
    builder.AddNull (P ({ 0 }));
    const auto withNull = std::move (builder).Finish ();

    const IterationPlan strict =
        Plan ({ IterationInput { withNull.get (), PortAccess::Item, InputRequirement::MustExist } });
    ASSERT_EQ (strict.iterations.size (), 2u);
    EXPECT_TRUE (strict.iterations[0].satisfied);
    EXPECT_FALSE (strict.iterations[1].satisfied);
    EXPECT_EQ (strict.iterations[1].unsatisfiedInput, 0u);
    EXPECT_TRUE (strict.iterations[1].cursors[0].isNull);

    // A port that accepts null gets to see it.
    const IterationPlan tolerant =
        Plan ({ IterationInput { withNull.get (), PortAccess::Item, InputRequirement::MayBeNull } });
    EXPECT_TRUE (tolerant.iterations[1].satisfied);
    EXPECT_TRUE (tolerant.iterations[1].cursors[0].isNull);
}

TEST (TreeIteration, AMissingSiteFollowsTheDeclaredRequirement)
{
    // {1} is empty on the second input, so the walk lands on nothing there.
    const auto driver = Tree ({ { 1 }, { 2 } });
    DataTreeBuilder<int64_t> holed;
    holed.Add (P ({ 0 }), 10);
    holed.EnsureList (P ({ 1 }));
    const auto sparse = std::move (holed).Finish ();

    const IterationPlan strict =
        Plan ({ In (driver), IterationInput { sparse.get (), PortAccess::Item, InputRequirement::MustExist } });
    ASSERT_EQ (strict.iterations.size (), 2u);
    EXPECT_TRUE (strict.iterations[0].satisfied);
    EXPECT_FALSE (strict.iterations[1].satisfied);
    EXPECT_EQ (strict.iterations[1].unsatisfiedInput, 1u);

    const IterationPlan optional =
        Plan ({ In (driver), IterationInput { sparse.get (), PortAccess::Item, InputRequirement::MayBeMissing } });
    EXPECT_TRUE (optional.iterations[1].satisfied);
    EXPECT_FALSE (optional.iterations[1].cursors[1].present);
    const IterationInput absentInput { sparse.get (), PortAccess::Item, InputRequirement::MayBeMissing };
    EXPECT_EQ (CursorList (absentInput, optional.iterations[1].cursors[1]), nullptr);
}

TEST (TreeIteration, CallerErrorsAreRefused)
{
    IterationPlan plan;
    std::string error;
    EXPECT_FALSE (BuildIterationPlan ({}, plan, error));
    EXPECT_FALSE (BuildIterationPlan ({ IterationInput {} }, plan, error));
    EXPECT_NE (error.find ("no tree"), std::string::npos);
}
