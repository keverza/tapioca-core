// Offline gate for the data-tree VALUE and PATH operations (HANDOFF §9.1-§9.3).
//
// test_datatree.cpp covers the containers and the four topology operations;
// this file covers what nodes are written in terms of on top of them. The
// failure modes are the same kind of silent: a map that turns a null item into
// a default, a filter that quietly deletes the path it emptied, a path shift
// that clamps instead of refusing, a metadata write that lands on the valued
// items but not the null ones. Every one of those produces a graph that runs.

#include "NodeGraph/Data/DataTreeOps.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace evp::nodegraph;
using namespace evp::nodegraph::data;

namespace {

MetadataKey Key (const char* dotted)
{
    const std::optional<MetadataKey> key = MetadataKey::Parse (dotted);
    EXPECT_TRUE (key.has_value ()) << dotted;
    return *key;
}

SharedMetadata Meta (const char* dotted, MetadataValue value)
{
    MetadataBuilder builder;
    std::string error;
    EXPECT_TRUE (builder.Set (Key (dotted), std::move (value), error)) << error;
    return std::move (builder).Finish ();
}

DataPath P (std::initializer_list<DataPath::Segment> segments)
{
    return DataPath (segments);
}

// {0}: 1, null(a.b=7), 3   {1;2}: 10
std::shared_ptr<const DataTree<int64_t>> Sample ()
{
    DataTreeBuilder<int64_t> builder;
    builder.Add (P ({ 0 }), 1);
    builder.AddNull (P ({ 0 }), Meta ("a.b", int64_t { 7 }));
    builder.Add (P ({ 0 }), 3);
    builder.Add (P ({ 1, 2 }), 10);
    return std::move (builder).Finish ();
}

} // namespace

// ---- Map -------------------------------------------------------------------

TEST (DataTreeMap, PreservesTopologyNullnessAndMetadata)
{
    const auto doubled = MapTree (*Sample (), [] (int64_t value) { return value * 2; });

    ASSERT_EQ (doubled->ListCount (), 2u);
    EXPECT_EQ (doubled->PathAt (0), P ({ 0 }));
    EXPECT_EQ (doubled->PathAt (1), P ({ 1, 2 }));

    const DataList<int64_t>& list = doubled->TypedListAt (0);
    ASSERT_EQ (list.Size (), 3u);
    EXPECT_EQ (list.At (0), 2);
    EXPECT_EQ (list.At (2), 6);

    // The null item is still null - not a doubled zero - and kept its metadata.
    EXPECT_TRUE (list.IsNullAt (1));
    EXPECT_EQ (std::get<int64_t> (*list.MetadataAt (1)->FindValue (Key ("a.b"))), 7);
}

TEST (DataTreeMap, ChangesTheItemType)
{
    const auto text = MapTree (*Sample (), [] (int64_t value) { return std::to_string (value); });

    EXPECT_EQ (text->Type (), ItemType::String);
    EXPECT_EQ (text->TypedListAt (0).At (0), "1");
    EXPECT_TRUE (text->TypedListAt (0).IsNullAt (1));
    EXPECT_EQ (text->ItemCount (), 4u);
}

TEST (DataTreeMap, EmptyListsSurviveTheMap)
{
    DataTreeBuilder<int64_t> builder;
    builder.EnsureList (P ({ 5 }));
    const auto mapped = MapTree (*std::move (builder).Finish (), [] (int64_t value) { return value; });

    ASSERT_EQ (mapped->ListCount (), 1u);
    EXPECT_EQ (mapped->PathAt (0), P ({ 5 }));
    EXPECT_TRUE (mapped->TypedListAt (0).IsEmpty ());
}

// ---- Filter ----------------------------------------------------------------

TEST (DataTreeFilter, KeepsEmptiedPathsUnlessAskedOtherwise)
{
    const auto sample = Sample ();
    const auto odd =
        FilterTree (*sample, [] (const DataItem<int64_t>& item) { return !item.IsNull () && item.Value () % 2 == 1; });

    ASSERT_EQ (odd->ListCount (), 2u);
    EXPECT_EQ (odd->TypedListAt (0).Size (), 2u);
    EXPECT_TRUE (odd->TypedListAt (1).IsEmpty ()); // {1;2} kept 10 out, path stays.

    const auto pruned = FilterTree (
        *sample, [] (const DataItem<int64_t>& item) { return !item.IsNull () && item.Value () % 2 == 1; },
        /*removeEmptyLists=*/true);
    ASSERT_EQ (pruned->ListCount (), 1u);
    EXPECT_EQ (pruned->PathAt (0), P ({ 0 }));
}

TEST (DataTreeFilter, SeesNullnessAndMetadata)
{
    const auto nullsOnly = FilterTree (*Sample (), [] (const DataItem<int64_t>& item) { return item.IsNull (); }, true);

    ASSERT_EQ (nullsOnly->ListCount (), 1u);
    ASSERT_EQ (nullsOnly->ItemCount (), 1u);
    EXPECT_TRUE (nullsOnly->TypedListAt (0).IsNullAt (0));
    EXPECT_EQ (std::get<int64_t> (*nullsOnly->TypedListAt (0).MetadataAt (0)->FindValue (Key ("a.b"))), 7);
}

TEST (DataTreeCount, ReportsOneIntegerPerPath)
{
    const auto counts = CountTreeItems (*Sample ());

    ASSERT_EQ (counts->ListCount (), 2u);
    EXPECT_EQ (counts->Find (P ({ 0 }))->At (0), 3); // The null item counts.
    EXPECT_EQ (counts->Find (P ({ 1, 2 }))->At (0), 1);
    EXPECT_EQ (counts->Type (), ItemType::Integer);
}

// ---- Path remapping --------------------------------------------------------

TEST (DataTreePaths, RemapRefusesToFoldTwoPathsIntoOne)
{
    const auto sample = Sample ();
    std::shared_ptr<const DataTree<int64_t>> result;
    std::string error;

    const auto toZero = [] (const DataPath&) { return DataPath::Zero (); };
    EXPECT_FALSE (MapTreePaths (*sample, toZero, PathCollision::Error, result, error));
    EXPECT_NE (error.find ("{0}"), std::string::npos);

    ASSERT_TRUE (MapTreePaths (*sample, toZero, PathCollision::Append, result, error));
    ASSERT_EQ (result->ListCount (), 1u);
    ASSERT_EQ (result->ItemCount (), 4u);
    // Canonical source order: {0}'s three items, then {1;2}'s one.
    EXPECT_EQ (result->TypedListAt (0).At (0), 1);
    EXPECT_EQ (result->TypedListAt (0).At (3), 10);
}

TEST (DataTreePaths, RemapKeepsItemsAndReordersPathsCanonically)
{
    std::shared_ptr<const DataTree<int64_t>> result;
    std::string error;
    ASSERT_TRUE (MapTreePaths (
        *Sample (), [] (const DataPath& path) { return path.Prepend (9); }, PathCollision::Error, result, error));

    ASSERT_EQ (result->ListCount (), 2u);
    EXPECT_EQ (result->PathAt (0), P ({ 9, 0 }));
    EXPECT_EQ (result->PathAt (1), P ({ 9, 1, 2 }));
    EXPECT_EQ (result->ItemCount (), 4u);
}

TEST (DataTreePaths, ShiftDropsOrPrependsAndRefusesToEmptyAPath)
{
    std::shared_ptr<const DataTree<int64_t>> result;
    std::string error;

    // {0} has one segment, so a positive shift cannot apply to this tree.
    EXPECT_FALSE (ShiftTreePaths (*Sample (), 1, PathCollision::Error, result, error));
    EXPECT_NE (error.find ("{0}"), std::string::npos);

    ASSERT_TRUE (ShiftTreePaths (*Sample (), -2, PathCollision::Error, result, error));
    EXPECT_EQ (result->PathAt (0), P ({ 0, 0, 0 }));
    EXPECT_EQ (result->PathAt (1), P ({ 0, 0, 1, 2 }));

    DataTreeBuilder<int64_t> deep;
    deep.Add (P ({ 4, 0 }), 1);
    deep.Add (P ({ 4, 1 }), 2);
    ASSERT_TRUE (ShiftTreePaths (*std::move (deep).Finish (), 1, PathCollision::Error, result, error));
    EXPECT_EQ (result->PathAt (0), P ({ 0 }));
    EXPECT_EQ (result->PathAt (1), P ({ 1 }));

    ASSERT_TRUE (ShiftTreePaths (*Sample (), 0, PathCollision::Error, result, error));
    EXPECT_TRUE (result->Equals (*Sample ()));
}

// ---- Metadata --------------------------------------------------------------

TEST (DataTreeMetadata, SetReachesNullItemsAndReplacesExistingKeys)
{
    std::shared_ptr<const DataTree<int64_t>> result;
    std::string error;
    const MetadataEntry entry { Key ("tapioca.source.run"), int64_t { 42 }, false };
    ASSERT_TRUE (SetTreeMetadata (*Sample (), entry, result, error)) << error;

    for (size_t listIndex = 0; listIndex < result->ListCount (); ++listIndex) {
        const DataList<int64_t>& list = result->TypedListAt (listIndex);
        for (size_t index = 0; index < list.Size (); ++index) {
            const MetadataValue* value = list.MetadataAt (index)->FindValue (Key ("tapioca.source.run"));
            ASSERT_NE (value, nullptr);
            EXPECT_EQ (std::get<int64_t> (*value), 42);
        }
    }

    // The item that already had a.b keeps it alongside the new key.
    EXPECT_EQ (result->TypedListAt (0).MetadataAt (1)->Size (), 2u);

    // Replacing is not an error; a second write wins on that key alone.
    const MetadataEntry again { Key ("tapioca.source.run"), int64_t { 43 }, false };
    ASSERT_TRUE (SetTreeMetadata (*result, again, result, error));
    EXPECT_EQ (std::get<int64_t> (*result->TypedListAt (0).MetadataAt (0)->FindValue (Key ("tapioca.source.run"))), 43);
}

TEST (DataTreeMetadata, SetRefusesAValueTheMapCannotHold)
{
    std::shared_ptr<const DataTree<int64_t>> result;
    std::string error;
    const MetadataEntry bad { Key ("tapioca.display.label"), std::string ("text"), /*transformable=*/true };
    EXPECT_FALSE (SetTreeMetadata (*Sample (), bad, result, error));
    EXPECT_NE (error.find ("tapioca.display.label"), std::string::npos);
}

TEST (DataTreeMetadata, RemoveLeavesUntaggedItemsAlone)
{
    const auto stripped = RemoveTreeMetadata (*Sample (), Key ("a.b"));

    EXPECT_TRUE (stripped->TypedListAt (0).MetadataAt (1)->IsEmpty ());
    EXPECT_TRUE (stripped->TypedListAt (0).IsNullAt (1)); // Still null.
    EXPECT_EQ (stripped->ItemCount (), 4u);

    // Metadata is part of content, so removing some really is a new tree (7.10).
    EXPECT_FALSE (stripped->Equals (*Sample ()));
    EXPECT_NE (stripped->Hash (), Sample ()->Hash ());
}

TEST (DataTreeOperations, NoOperationTouchesItsInput)
{
    const auto sample = Sample ();
    const size_t hash = sample->Hash ();

    MapTree (*sample, [] (int64_t value) { return value; });
    FilterTree (*sample, [] (const DataItem<int64_t>&) { return false; });
    CountTreeItems (*sample);
    RemoveTreeMetadata (*sample, Key ("a.b"));

    std::shared_ptr<const DataTree<int64_t>> result;
    std::string error;
    ShiftTreePaths (*sample, -1, PathCollision::Error, result, error);
    SetTreeMetadata (*sample, MetadataEntry { Key ("a.c"), int64_t { 1 }, false }, result, error);

    EXPECT_EQ (sample->Hash (), hash);
    EXPECT_EQ (sample->ItemCount (), 4u);
    EXPECT_EQ (std::get<int64_t> (*sample->TypedListAt (0).MetadataAt (1)->FindValue (Key ("a.b"))), 7);
}
