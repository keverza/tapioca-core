// Offline gate for the node-graph data-tree layer (HANDOFF §7, §9).
//
// This layer is worth testing here rather than in Archicad because every way of
// getting it wrong is SILENT. A path order that is not canonical produces a tree
// that serialises differently on two machines; a null item that loses its
// nullness reads as a zero; metadata dropped by graft removes provenance from
// geometry that still draws correctly; a hash that ignores metadata makes the
// evaluator serve a stale cached result. None of those report anything - they
// produce a graph that looks like it worked.

#include "NodeGraph/Data/DataTree.hpp"
#include "NodeGraph/Data/DataTreeOps.hpp"

#include <gtest/gtest.h>

#include <limits>
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

SharedMetadata Meta (const char* dotted, MetadataValue value, bool transformable = false)
{
    MetadataBuilder builder;
    std::string error;
    EXPECT_TRUE (builder.Set (Key (dotted), std::move (value), transformable, error)) << error;
    return std::move (builder).Finish ();
}

DataPath P (std::initializer_list<DataPath::Segment> segments)
{
    return DataPath (segments);
}

} // namespace

// ---- DataPath --------------------------------------------------------------

TEST (DataPath, DefaultIsZeroAndNeverEmpty)
{
    EXPECT_EQ (DataPath ().Length (), 1u);
    EXPECT_EQ (DataPath ().ToString (), "{0}");
    EXPECT_TRUE (DataPath ().IsZero ());
    EXPECT_THROW (DataPath (std::vector<DataPath::Segment> {}), std::invalid_argument);
    EXPECT_FALSE (DataPath::TryCreate ({}).has_value ());
}

TEST (DataPath, OrderIsLengthAwareLexicographic)
{
    // A prefix sorts before what extends it. This is the canonical traversal
    // order every operation and serialisation depends on.
    EXPECT_LT (P ({ 0 }), P ({ 0, 1 }));
    EXPECT_LT (P ({ 0, 1 }), P ({ 1 }));
    EXPECT_LT (P ({ 0, 2 }), P ({ 0, 10 }));
    EXPECT_EQ (P ({ 3, 4 }), P ({ 3, 4 }));
    EXPECT_NE (P ({ 3, 4 }), P ({ 3, 4, 0 }));
}

TEST (DataPath, ParseAcceptsBracedAndBareFormsAndRejectsRubbish)
{
    EXPECT_EQ (DataPath::Parse ("{0;1;2}").value (), P ({ 0, 1, 2 }));
    EXPECT_EQ (DataPath::Parse ("0;1;2").value (), P ({ 0, 1, 2 }));
    EXPECT_EQ (DataPath::Parse (" { 4 } ").value (), P ({ 4 }));

    EXPECT_FALSE (DataPath::Parse ("").has_value ());
    EXPECT_FALSE (DataPath::Parse ("{}").has_value ());
    EXPECT_FALSE (DataPath::Parse ("{0;}").has_value ());
    EXPECT_FALSE (DataPath::Parse ("{-1}").has_value ()); // Must not wrap to a huge segment.
    EXPECT_FALSE (DataPath::Parse ("{1;a}").has_value ());
    EXPECT_FALSE (DataPath::Parse ("{0").has_value ());
}

TEST (DataPath, EditsReturnNewPathsAndRefuseToEmptyOne)
{
    const DataPath path = P ({ 1, 2 });
    EXPECT_EQ (path.Append (3), P ({ 1, 2, 3 }));
    EXPECT_EQ (path.Prepend (0), P ({ 0, 1, 2 }));
    EXPECT_EQ (path.WithLast (9), P ({ 1, 9 }));
    EXPECT_EQ (path.Increment (), P ({ 1, 3 }));
    EXPECT_EQ (path.DropLast ().value (), P ({ 1 }));
    EXPECT_EQ (path, P ({ 1, 2 })); // Unchanged: every edit is a new path.

    EXPECT_FALSE (P ({ 7 }).DropLast ().has_value ());
    EXPECT_FALSE (P ({ 7 }).DropFirst (1).has_value ());
    EXPECT_FALSE (path.SubPath (0, 0).has_value ());
}

TEST (DataPath, CommonPrefixLengthIsTheBasisOfSimplify)
{
    EXPECT_EQ (CommonPrefixLength (P ({ 5, 0, 1 }), P ({ 5, 0, 2 })), 2u);
    EXPECT_EQ (CommonPrefixLength (P ({ 5 }), P ({ 6 })), 0u);
    EXPECT_EQ (CommonPrefixLength (P ({ 5, 1 }), P ({ 5, 1 })), 2u);
}

// ---- Metadata --------------------------------------------------------------

TEST (Metadata, KeysAreSegmentedAndValidated)
{
    EXPECT_EQ (Key ("tapioca.source.guid").Segments ().size (), 3u);
    EXPECT_EQ (Key ("tapioca.source.guid").ToString (), "tapioca.source.guid");
    EXPECT_TRUE (Key ("tapioca.unit.length").IsReserved ());
    EXPECT_FALSE (Key ("com.example.thing").IsReserved ());

    EXPECT_FALSE (MetadataKey::Parse ("").has_value ());
    EXPECT_FALSE (MetadataKey::Parse ("a..b").has_value ()); // Empty segment.
    EXPECT_FALSE (MetadataKey::Parse (".lead").has_value ());
    EXPECT_FALSE (MetadataKey::Create ({ std::string ("bad\nkey") }).has_value ());
}

TEST (Metadata, BuilderRejectsDuplicatesNonFiniteAndFalseTransformables)
{
    MetadataBuilder builder;
    std::string error;
    EXPECT_TRUE (builder.Set (Key ("a.b"), int64_t { 3 }, error));
    EXPECT_FALSE (builder.Set (Key ("a.b"), int64_t { 4 }, error));
    EXPECT_NE (error.find ("a.b"), std::string::npos);

    EXPECT_FALSE (builder.Set (Key ("a.c"), std::numeric_limits<double>::quiet_NaN (), error));
    EXPECT_FALSE (builder.Set (Key ("a.d"), std::string ("text"), true, error));
    EXPECT_TRUE (builder.Set (Key ("a.e"), Point3 { 1.0, 2.0, 3.0 }, true, error));
}

TEST (Metadata, EmptyIsOneSharedSingletonAndEqualityIsByContent)
{
    EXPECT_EQ (MetadataMap::Empty (), MetadataMap::Empty ());
    EXPECT_TRUE (MetadataMap::Empty ()->IsEmpty ());
    EXPECT_TRUE (MetadataEquals (nullptr, MetadataMap::Empty ()));

    const SharedMetadata left = Meta ("tapioca.unit.length", std::string ("m"));
    const SharedMetadata right = Meta ("tapioca.unit.length", std::string ("m"));
    EXPECT_NE (left, right);                    // Different objects...
    EXPECT_TRUE (MetadataEquals (left, right)); // ...same metadata.
    EXPECT_EQ (left->Hash (), right->Hash ());
    EXPECT_FALSE (MetadataEquals (left, Meta ("tapioca.unit.length", std::string ("mm"))));
}

TEST (Metadata, MergeRefusesConflictsButNotAgreement)
{
    const SharedMetadata left = Meta ("a.b", int64_t { 1 });
    const SharedMetadata same = Meta ("a.b", int64_t { 1 });
    const SharedMetadata other = Meta ("a.b", int64_t { 2 });

    SharedMetadata result;
    std::string error;
    EXPECT_TRUE (MergeMetadata (left, same, MetadataMerge::Error, result, error));
    EXPECT_TRUE (MetadataEquals (result, left));

    EXPECT_FALSE (MergeMetadata (left, other, MetadataMerge::Error, result, error));
    EXPECT_NE (error.find ("a.b"), std::string::npos);

    ASSERT_TRUE (MergeMetadata (left, other, MetadataMerge::KeepLeft, result, error));
    EXPECT_EQ (std::get<int64_t> (*result->FindValue (Key ("a.b"))), 1);

    ASSERT_TRUE (MergeMetadata (left, other, MetadataMerge::PreferRight, result, error));
    EXPECT_EQ (std::get<int64_t> (*result->FindValue (Key ("a.b"))), 2);
}

// ---- DataList --------------------------------------------------------------

TEST (DataList, NullEmptyAndValuedItemsStayDistinct)
{
    DataListBuilder<double> builder;
    builder.Add (1.5);
    builder.AddNull (Meta ("tapioca.source.note", std::string ("missing")));
    builder.Add (2.5);
    const DataList<double> list = std::move (builder).Finish ();

    EXPECT_EQ (list.Size (), 3u);
    EXPECT_FALSE (list.IsEmpty ());
    EXPECT_FALSE (list.IsNullAt (0));
    EXPECT_TRUE (list.IsNullAt (1));
    EXPECT_EQ (list.At (2), 2.5);
    EXPECT_THROW (list.At (1), std::logic_error);

    // A null item still carries metadata, and a projection can tell it from a
    // value (7.5): ValueAt gives nothing rather than a defaulted zero.
    EXPECT_FALSE (list.ValueAt (1).has_value ());
    EXPECT_FALSE (list.MetadataAt (1)->IsEmpty ());
    EXPECT_TRUE (list.MetadataAt (0)->IsEmpty ());

    EXPECT_TRUE (DataList<double>::EmptyList ().IsEmpty ());
}

TEST (DataList, SideArraysCollapseWhenNothingUsesThem)
{
    DataListBuilder<int64_t> builder;
    builder.Add (1);
    builder.Add (2);
    const DataList<int64_t> plain = std::move (builder).Finish ();
    EXPECT_FALSE (plain.HasNulls ());
    EXPECT_FALSE (plain.HasMetadata ());
    EXPECT_TRUE (plain.MetadataAt (1)->IsEmpty ());

    DataListBuilder<int64_t> other;
    other.Add (1, Meta ("a.b", int64_t { 7 }));
    const DataList<int64_t> tagged = std::move (other).Finish ();
    EXPECT_TRUE (tagged.HasMetadata ());
}

TEST (DataList, ConcatAndSlicePreserveOrderNullnessAndMetadata)
{
    DataListBuilder<int64_t> first;
    first.Add (1, Meta ("a.b", int64_t { 10 }));
    first.AddNull ();
    DataListBuilder<int64_t> second;
    second.Add (3);

    const DataList<int64_t> joined = std::move (first).Finish ().Concat (std::move (second).Finish ());
    ASSERT_EQ (joined.Size (), 3u);
    EXPECT_EQ (joined.At (0), 1);
    EXPECT_TRUE (joined.IsNullAt (1));
    EXPECT_EQ (joined.At (2), 3);
    EXPECT_EQ (std::get<int64_t> (*joined.MetadataAt (0)->FindValue (Key ("a.b"))), 10);

    const DataList<int64_t> slice = joined.Slice (1, 5); // Clamped, not an error.
    ASSERT_EQ (slice.Size (), 2u);
    EXPECT_TRUE (slice.IsNullAt (0));
    EXPECT_EQ (slice.At (1), 3);
    EXPECT_TRUE (joined.Slice (9, 1).IsEmpty ());
}

TEST (DataList, HashAndEqualityAccountForMetadataAndNullness)
{
    DataListBuilder<int64_t> plain;
    plain.Add (1);
    DataListBuilder<int64_t> tagged;
    tagged.Add (1, Meta ("a.b", int64_t { 1 }));
    DataListBuilder<int64_t> nulled;
    nulled.AddNull ();

    const DataList<int64_t> a = std::move (plain).Finish ();
    const DataList<int64_t> b = std::move (tagged).Finish ();
    const DataList<int64_t> c = std::move (nulled).Finish ();

    EXPECT_FALSE (a.Equals (b));
    EXPECT_NE (a.Hash (), b.Hash ());
    EXPECT_FALSE (a.Equals (c));
    EXPECT_NE (a.Hash (), c.Hash ());

    DataListBuilder<int64_t> again;
    again.Add (1);
    EXPECT_TRUE (a.Equals (std::move (again).Finish ()));
}

// ---- DataTree --------------------------------------------------------------

TEST (DataTree, ItemListAndTreeAreTheSameStorage)
{
    const auto item = DataTree<double>::FromItem (4.0);
    EXPECT_EQ (item->ListCount (), 1u);
    EXPECT_EQ (item->ItemCount (), 1u);
    EXPECT_EQ (item->Paths ().front (), DataPath::Zero ());

    const auto list = DataTree<double>::FromList ({ 1.0, 2.0, 3.0 });
    EXPECT_EQ (list->ListCount (), 1u);
    EXPECT_EQ (list->ItemCount (), 3u);

    EXPECT_TRUE (DataTree<double>::EmptyTree ()->IsEmpty ());
    EXPECT_EQ (DataTree<double>::EmptyTree ()->ItemCount (), 0u);
}

TEST (DataTree, BuilderSortsPathsAndKeepsItemOrderWithinAPath)
{
    DataTreeBuilder<int64_t> builder;
    builder.Add (P ({ 1 }), 10);
    builder.Add (P ({ 0, 1 }), 20);
    builder.Add (P ({ 1 }), 11);
    builder.Add (P ({ 0 }), 30);
    const auto tree = std::move (builder).Finish ();

    ASSERT_EQ (tree->ListCount (), 3u);
    EXPECT_EQ (tree->PathAt (0), P ({ 0 }));
    EXPECT_EQ (tree->PathAt (1), P ({ 0, 1 }));
    EXPECT_EQ (tree->PathAt (2), P ({ 1 }));

    const DataList<int64_t>* repeated = tree->Find (P ({ 1 }));
    ASSERT_NE (repeated, nullptr);
    ASSERT_EQ (repeated->Size (), 2u);
    EXPECT_EQ (repeated->At (0), 10); // Append order, not sorted.
    EXPECT_EQ (repeated->At (1), 11);
    EXPECT_EQ (tree->ItemCount (), 4u);
}

TEST (DataTree, EmptyListIsNotAMissingList)
{
    DataTreeBuilder<int64_t> builder;
    builder.EnsureList (P ({ 2 }));
    const auto tree = std::move (builder).Finish ();

    const DataList<int64_t>* present = tree->Find (P ({ 2 }));
    ASSERT_NE (present, nullptr);
    EXPECT_TRUE (present->IsEmpty ());
    EXPECT_EQ (tree->Find (P ({ 3 })), nullptr);
    EXPECT_EQ (tree->ListCount (), 1u);
}

TEST (DataTree, SiteAddressesAnItemAndReportsNullOrMissing)
{
    DataTreeBuilder<int64_t> builder;
    builder.Add (P ({ 0 }), 5);
    builder.AddNull (P ({ 0 }));
    const auto tree = std::move (builder).Finish ();

    EXPECT_EQ (tree->ItemAt (DataSite { P ({ 0 }), 0 }).value (), 5);
    EXPECT_FALSE (tree->ItemAt (DataSite { P ({ 0 }), 1 }).has_value ()); // Null item.
    EXPECT_FALSE (tree->ItemAt (DataSite { P ({ 0 }), 9 }).has_value ()); // Past the end.
    EXPECT_FALSE (tree->ItemAt (DataSite { P ({ 4 }), 0 }).has_value ()); // No such path.
    const DataSite site { P ({ 0, 2 }), 4 };
    EXPECT_EQ (site.ToString (), "{0;2}[4]");
}

TEST (DataTree, HashAndEqualityCoverTopologyOrderAndMetadata)
{
    const auto build = [] (bool withMetadata, bool swapOrder) {
        DataTreeBuilder<int64_t> builder;
        const SharedMetadata metadata = withMetadata ? Meta ("a.b", int64_t { 1 }) : nullptr;
        if (swapOrder) {
            builder.Add (P ({ 0 }), 2, metadata);
            builder.Add (P ({ 0 }), 1);
        }
        else {
            builder.Add (P ({ 0 }), 1, metadata);
            builder.Add (P ({ 0 }), 2);
        }
        return std::move (builder).Finish ();
    };

    const auto plain = build (false, false);
    EXPECT_TRUE (plain->Equals (*build (false, false)));

    // A metadata-only change is a real output change (7.10).
    EXPECT_FALSE (plain->Equals (*build (true, false)));
    EXPECT_NE (plain->Hash (), build (true, false)->Hash ());

    // So is a reordering.
    EXPECT_FALSE (plain->Equals (*build (false, true)));
    EXPECT_NE (plain->Hash (), build (false, true)->Hash ());
}

TEST (DataTree, TypeErasedSurfaceSeesStructureWithoutTheItemType)
{
    DataTreeBuilder<std::string> builder;
    builder.Add (P ({ 0 }), std::string ("one"));
    builder.AddNull (P ({ 1 }));
    const TreeValue value = MakeTreeValue<std::string> (std::move (builder).Finish ());

    ASSERT_TRUE (value.IsPresent ());
    EXPECT_EQ (value.itemType, ItemType::String);
    EXPECT_EQ (value.tree->Type (), ItemType::String);
    EXPECT_EQ (value.tree->ListCount (), 2u);
    EXPECT_EQ (value.tree->ItemCount (), 2u);

    const IDataList& first = value.tree->ListAt (0);
    ASSERT_TRUE (first.ValueAt (0).has_value ());
    EXPECT_EQ (std::get<std::string> (first.ValueAt (0)->DataValue ()), "one");
    EXPECT_FALSE (value.tree->ListAt (1).ValueAt (0).has_value ());
}

TEST (DataTree, ItemTypeMappingRefusesListAndAbsent)
{
    EXPECT_EQ (ItemTypeFromValueType (ValueType::Double), ItemType::Double);
    EXPECT_FALSE (ItemTypeFromValueType (ValueType::List).has_value ());
    EXPECT_FALSE (ItemTypeFromValueType (ValueType::Absent).has_value ());
    EXPECT_FALSE (ValueTypeFromItemType (ItemType::Any).has_value ());
    EXPECT_EQ (ItemTypeFromName ("mesh"), ItemType::Mesh);
    EXPECT_FALSE (ItemTypeFromName ("list").has_value ());

    EXPECT_TRUE (IsAtomicValue (Value (int64_t { 1 })));
    EXPECT_FALSE (IsAtomicValue (Value ()));
    // A list can no longer be a Value at all - Value has no List alternative
    // any more, so IsAtomicValue's exclusion of it is enforced by the type
    // system rather than by this check.
}

// ---- Operations ------------------------------------------------------------

TEST (DataTreeOps, FlattenConcatenatesInCanonicalOrderAndKeepsMetadata)
{
    DataTreeBuilder<int64_t> builder;
    builder.Add (P ({ 1 }), 30);
    builder.Add (P ({ 0 }), 10, Meta ("a.b", int64_t { 1 }));
    builder.Add (P ({ 0 }), 20);
    builder.EnsureList (P ({ 5 }));
    const auto flat = FlattenTree (*std::move (builder).Finish ());

    ASSERT_EQ (flat->ListCount (), 1u);
    EXPECT_EQ (flat->PathAt (0), DataPath::Zero ());
    const DataList<int64_t>& list = flat->TypedListAt (0);
    ASSERT_EQ (list.Size (), 3u);
    EXPECT_EQ (list.At (0), 10);
    EXPECT_EQ (list.At (1), 20);
    EXPECT_EQ (list.At (2), 30);
    EXPECT_FALSE (list.MetadataAt (0)->IsEmpty ());

    EXPECT_TRUE (FlattenTree (*DataTree<int64_t>::EmptyTree ())->IsEmpty ());
}

TEST (DataTreeOps, GraftMovesEachItemToItsOwnList)
{
    DataTreeBuilder<int64_t> builder;
    builder.Add (P ({ 0 }), 10, Meta ("a.b", int64_t { 1 }));
    builder.Add (P ({ 0 }), 20);
    builder.EnsureList (P ({ 7 }));
    const auto grafted = GraftTree (*std::move (builder).Finish ());

    ASSERT_EQ (grafted->ListCount (), 3u);
    EXPECT_EQ (grafted->PathAt (0), P ({ 0, 0 }));
    EXPECT_EQ (grafted->PathAt (1), P ({ 0, 1 }));
    EXPECT_EQ (grafted->TypedListAt (0).At (0), 10);
    EXPECT_FALSE (grafted->TypedListAt (0).MetadataAt (0)->IsEmpty ());

    // The empty list keeps its own path rather than disappearing.
    EXPECT_EQ (grafted->PathAt (2), P ({ 7 }));
    EXPECT_TRUE (grafted->TypedListAt (2).IsEmpty ());
}

TEST (DataTreeOps, SimplifyDropsTheSharedPrefixAndKeepsOneSegment)
{
    DataTreeBuilder<int64_t> builder;
    builder.Add (P ({ 4, 9, 0 }), 1);
    builder.Add (P ({ 4, 9, 1 }), 2);
    const auto simplified = SimplifyTree (std::move (builder).Finish ());

    ASSERT_EQ (simplified->ListCount (), 2u);
    EXPECT_EQ (simplified->PathAt (0), P ({ 0 }));
    EXPECT_EQ (simplified->PathAt (1), P ({ 1 }));

    DataTreeBuilder<int64_t> single;
    single.Add (P ({ 3, 3 }), 1);
    const auto one = SimplifyTree (std::move (single).Finish ());
    EXPECT_EQ (one->PathAt (0), P ({ 3 })); // A path never becomes empty.

    // Already simple: the same tree comes back, not a copy of it.
    const auto plain = DataTree<int64_t>::FromList ({ 1, 2 });
    EXPECT_EQ (SimplifyTree (plain), plain);
}

TEST (DataTreeOps, MergeRefusesCollisionsByDefault)
{
    DataTreeBuilder<int64_t> leftBuilder;
    leftBuilder.Add (P ({ 0 }), 1);
    leftBuilder.Add (P ({ 1 }), 2);
    const auto left = std::move (leftBuilder).Finish ();

    DataTreeBuilder<int64_t> rightBuilder;
    rightBuilder.Add (P ({ 1 }), 3);
    rightBuilder.Add (P ({ 2 }), 4);
    const auto right = std::move (rightBuilder).Finish ();

    std::shared_ptr<const DataTree<int64_t>> merged;
    std::string error;
    EXPECT_FALSE (MergeTrees (*left, *right, MergeCollision::Error, merged, error));
    EXPECT_NE (error.find ("{1}"), std::string::npos);

    ASSERT_TRUE (MergeTrees (*left, *right, MergeCollision::Append, merged, error));
    ASSERT_EQ (merged->ListCount (), 3u);
    ASSERT_EQ (merged->Find (P ({ 1 }))->Size (), 2u);
    EXPECT_EQ (merged->Find (P ({ 1 }))->At (0), 2);
    EXPECT_EQ (merged->Find (P ({ 1 }))->At (1), 3);

    ASSERT_TRUE (MergeTrees (*left, *right, MergeCollision::Replace, merged, error));
    ASSERT_EQ (merged->Find (P ({ 1 }))->Size (), 1u);
    EXPECT_EQ (merged->Find (P ({ 1 }))->At (0), 3);
    EXPECT_EQ (merged->ItemCount (), 3u);
}

TEST (DataTreeOps, InputsAreUntouchedByEveryOperation)
{
    const auto original = DataTree<int64_t>::FromList ({ 1, 2, 3 });
    const size_t hash = original->Hash ();

    FlattenTree (*original);
    GraftTree (*original);
    SimplifyTree (original);

    EXPECT_EQ (original->Hash (), hash);
    EXPECT_EQ (original->ListCount (), 1u);
    EXPECT_EQ (original->ItemCount (), 3u);
}
