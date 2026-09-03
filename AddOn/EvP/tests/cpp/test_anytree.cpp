// Offline gate for the runtime-typed tree surface (HANDOFF §7.4, §8.1).
//
// This is the layer the EVALUATOR uses, where the item type is a value read out
// of the registry rather than a template argument, and it is therefore the one
// place a type mismatch can slip into a tree: a node body that returns a string
// from a port declared Double, a fan-in that quietly mixes two item types, an
// erased builder that stores whatever it was handed. A tree with the wrong item
// in it does not fail here - it fails much later, in a node that trusted the
// declared type, or not at all.

#include "NodeGraph/Data/AnyTree.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace evp::nodegraph;
using namespace evp::nodegraph::data;

namespace {

DataPath P (std::initializer_list<DataPath::Segment> segments)
{
    return DataPath (segments);
}

} // namespace

TEST (AnyTreeBuilder, BuildsATypedTreeWithoutKnowingTheTypeAtCompileTime)
{
    AnyTreeBuilder builder (ItemType::Double);
    std::string error;
    ASSERT_TRUE (builder.Add (P ({ 0 }), Value (1.5), error)) << error;
    ASSERT_TRUE (builder.Add (P ({ 0 }), Value (2.5), error)) << error;
    builder.AddNull (P ({ 1 }));

    const TreeValue tree = std::move (builder).Finish ();
    ASSERT_TRUE (tree.IsPresent ());
    EXPECT_EQ (tree.itemType, ItemType::Double);
    EXPECT_EQ (tree.tree->Type (), ItemType::Double);
    EXPECT_EQ (tree.tree->ListCount (), 2u);
    EXPECT_EQ (tree.tree->ItemCount (), 3u);

    // It really is a DataTree<double>, not a bag of Values.
    const auto typed = std::dynamic_pointer_cast<const DataTree<double>> (tree.tree);
    ASSERT_NE (typed, nullptr);
    EXPECT_EQ (typed->TypedListAt (0).At (1), 2.5);
    EXPECT_TRUE (typed->TypedListAt (1).IsNullAt (0));
}

TEST (AnyTreeBuilder, RefusesAnItemOfTheWrongTypeAndSaysWhichSite)
{
    AnyTreeBuilder builder (ItemType::Double);
    std::string error;
    EXPECT_FALSE (builder.Add (P ({ 0, 3 }), Value (std::string ("nope")), error));
    EXPECT_NE (error.find ("{0;3}"), std::string::npos);
    EXPECT_NE (error.find ("double"), std::string::npos);
    EXPECT_NE (error.find ("string"), std::string::npos);

    // An integer is not a double either: the declared type is the contract, and
    // widening here would make the tree's declared type a suggestion.
    EXPECT_FALSE (builder.Add (P ({ 0 }), Value (int64_t { 2 }), error));

    // Absent is never an item; a body with nothing to say uses AddNull.
    EXPECT_FALSE (builder.Add (P ({ 0 }), Value (), error));
    EXPECT_NE (error.find ("none"), std::string::npos);
}

TEST (AnyTreeBuilder, AnAnyTreeTakesAnyAtomicItemButStillNotAList)
{
    AnyTreeBuilder builder (ItemType::Any);
    std::string error;
    EXPECT_TRUE (builder.Add (P ({ 0 }), Value (1.5), error));
    EXPECT_TRUE (builder.Add (P ({ 0 }), Value (std::string ("text")), error));
    EXPECT_FALSE (builder.Add (P ({ 0 }), Value (Value::List { Value (int64_t { 1 }) }), error));

    const TreeValue tree = std::move (builder).Finish ();
    EXPECT_EQ (tree.itemType, ItemType::Any);
    EXPECT_EQ (tree.tree->ItemCount (), 2u);
}

TEST (AnyTreeBuilder, AddListCopiesNullnessAndMetadata)
{
    DataTreeBuilder<int64_t> source;
    source.Add (P ({ 5 }), 1);
    source.AddNull (P ({ 5 }));
    const auto original = std::move (source).Finish ();

    AnyTreeBuilder builder (ItemType::Integer);
    std::string error;
    ASSERT_TRUE (builder.AddList (P ({ 9 }), original->ListAt (0), error)) << error;
    const TreeValue copy = std::move (builder).Finish ();

    ASSERT_EQ (copy.tree->ListCount (), 1u);
    EXPECT_EQ (copy.tree->Paths ()[0], P ({ 9 }));
    EXPECT_EQ (copy.tree->ListAt (0).Size (), 2u);
    EXPECT_TRUE (copy.tree->ListAt (0).IsNullAt (1));

    AnyTreeBuilder wrong (ItemType::Double);
    EXPECT_FALSE (wrong.AddList (P ({ 0 }), original->ListAt (0), error));
    EXPECT_NE (error.find ("integer"), std::string::npos);
}

TEST (AnyTreeBuilder, TheEmptyTreeOfATypeIsStillThatType)
{
    const TreeValue empty = EmptyTreeValue (ItemType::Polyline);
    ASSERT_TRUE (empty.IsPresent ());
    EXPECT_EQ (empty.itemType, ItemType::Polyline);
    EXPECT_TRUE (empty.tree->IsEmpty ());
}

// ---- Fan-in ----------------------------------------------------------------

TEST (TreeFanIn, CombinesInTheOrderTheCallerDeclares)
{
    AnyTreeBuilder first (ItemType::Integer);
    AnyTreeBuilder second (ItemType::Integer);
    std::string error;
    ASSERT_TRUE (first.Add (P ({ 0 }), Value (int64_t { 1 }), error));
    ASSERT_TRUE (second.Add (P ({ 0 }), Value (int64_t { 2 }), error));
    ASSERT_TRUE (second.Add (P ({ 1 }), Value (int64_t { 3 }), error));

    const TreeValue left = std::move (first).Finish ();
    const TreeValue right = std::move (second).Finish ();

    TreeValue merged;
    ASSERT_TRUE (MergeTreeValues ({ left, right }, FanInContract {}, merged, error)) << error;
    ASSERT_EQ (merged.tree->ListCount (), 2u);
    EXPECT_EQ (merged.tree->ListAt (0).Size (), 2u); // Appended, in declared order.
    EXPECT_EQ (std::get<int64_t> (merged.tree->ListAt (0).ValueAt (0)->DataValue ()), 1);
    EXPECT_EQ (std::get<int64_t> (merged.tree->ListAt (0).ValueAt (1)->DataValue ()), 2);

    // Reversing the declared order reverses the result: that is why the order
    // is the port's to state (§8.1), not the edge list's to imply.
    TreeValue reversed;
    ASSERT_TRUE (MergeTreeValues ({ right, left }, FanInContract {}, reversed, error));
    EXPECT_EQ (std::get<int64_t> (reversed.tree->ListAt (0).ValueAt (0)->DataValue ()), 2);
}

TEST (TreeFanIn, HonoursTheDeclaredCollisionPolicy)
{
    AnyTreeBuilder first (ItemType::Integer);
    AnyTreeBuilder second (ItemType::Integer);
    std::string error;
    ASSERT_TRUE (first.Add (P ({ 0 }), Value (int64_t { 1 }), error));
    ASSERT_TRUE (second.Add (P ({ 0 }), Value (int64_t { 2 }), error));
    const TreeValue left = std::move (first).Finish ();
    const TreeValue right = std::move (second).Finish ();

    TreeValue merged;
    FanInContract strict;
    strict.collision = MergeCollision::Error;
    EXPECT_FALSE (MergeTreeValues ({ left, right }, strict, merged, error));
    EXPECT_NE (error.find ("{0}"), std::string::npos);

    FanInContract replace;
    replace.collision = MergeCollision::Replace;
    ASSERT_TRUE (MergeTreeValues ({ left, right }, replace, merged, error));
    ASSERT_EQ (merged.tree->ListAt (0).Size (), 1u);
    EXPECT_EQ (std::get<int64_t> (merged.tree->ListAt (0).ValueAt (0)->DataValue ()), 2);
}

TEST (TreeFanIn, RefusesToMixItemTypes)
{
    const TreeValue integers = EmptyTreeValue (ItemType::Integer);
    const TreeValue doubles = EmptyTreeValue (ItemType::Double);

    TreeValue merged;
    std::string error;
    EXPECT_FALSE (MergeTreeValues ({ integers, doubles }, FanInContract {}, merged, error));
    EXPECT_NE (error.find ("integer"), std::string::npos);
    EXPECT_NE (error.find ("double"), std::string::npos);

    EXPECT_FALSE (MergeTreeValues ({}, FanInContract {}, merged, error));

    // One tree fans in to itself, shared rather than rebuilt.
    ASSERT_TRUE (MergeTreeValues ({ integers }, FanInContract {}, merged, error));
    EXPECT_EQ (merged.tree, integers.tree);
}

// ---- Slicing ---------------------------------------------------------------

TEST (TreeSlice, HandsTheBodyExactlyWhatItsAccessDeclared)
{
    DataTreeBuilder<int64_t> source;
    source.Add (P ({ 4 }), 10);
    source.Add (P ({ 4 }), 20);
    source.Add (P ({ 7 }), 30);
    const TreeValue input = MakeTreeValue<int64_t> (std::move (source).Finish ());

    InputCursor cursor;
    cursor.listIndex = 0;
    cursor.itemIndex = 1;

    std::string error;
    TreeValue slice;

    ASSERT_TRUE (SliceForAccess (input, PortAccess::Item, cursor, slice, error)) << error;
    EXPECT_EQ (slice.tree->ItemCount (), 1u);
    EXPECT_EQ (slice.tree->Paths ()[0], P ({ 4 })); // The item keeps its own path.
    EXPECT_EQ (std::get<int64_t> (slice.tree->ListAt (0).ValueAt (0)->DataValue ()), 20);

    ASSERT_TRUE (SliceForAccess (input, PortAccess::List, cursor, slice, error)) << error;
    EXPECT_EQ (slice.tree->ListCount (), 1u);
    EXPECT_EQ (slice.tree->ItemCount (), 2u);

    ASSERT_TRUE (SliceForAccess (input, PortAccess::Tree, cursor, slice, error)) << error;
    EXPECT_EQ (slice.tree, input.tree); // Shared whole, not copied.

    // The item itself, for a body that just wants the value.
    const std::optional<Value> item = ItemForCursor (input, cursor);
    ASSERT_TRUE (item.has_value ());
    EXPECT_EQ (std::get<int64_t> (item->DataValue ()), 20);
}

TEST (TreeSlice, AnAbsentCursorSlicesToAnEmptyTreeOfTheRightType)
{
    const TreeValue input = EmptyTreeValue (ItemType::Integer);
    InputCursor absent;
    absent.present = false;

    std::string error;
    TreeValue slice;
    ASSERT_TRUE (SliceForAccess (input, PortAccess::List, absent, slice, error));
    EXPECT_EQ (slice.itemType, ItemType::Integer);
    EXPECT_TRUE (slice.tree->IsEmpty ());
    EXPECT_FALSE (ItemForCursor (input, absent).has_value ());
}

TEST (TreeSlice, ANullItemSlicesToANullItemNotAnEmptyList)
{
    DataTreeBuilder<double> source;
    source.AddNull (P ({ 0 }));
    const TreeValue input = MakeTreeValue<double> (std::move (source).Finish ());

    InputCursor cursor;
    std::string error;
    TreeValue slice;
    ASSERT_TRUE (SliceForAccess (input, PortAccess::Item, cursor, slice, error));
    ASSERT_EQ (slice.tree->ItemCount (), 1u);
    EXPECT_TRUE (slice.tree->ListAt (0).IsNullAt (0));
    EXPECT_FALSE (ItemForCursor (input, cursor).has_value ());
}
