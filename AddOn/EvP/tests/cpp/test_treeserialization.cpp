// Offline gate for the tree JSON round trip and the type assistants
// (HANDOFF §7.8, §34).
//
// Serialization is where a data structure fails WITHOUT FAILING. An empty list
// that is not written comes back as a missing path; a null item written as 0.0
// comes back as a number somebody will build on; metadata dropped on the way
// out makes a saved graph compute the same answer with no provenance; a NaN
// written as JSON `null` comes back as a null item. Each of those reads back as
// a valid tree - just not the one that was saved - so the only place to catch
// them is a round-trip test.

#include "NodeGraph/Data/TreeSerialization.hpp"
#include "NodeGraph/Data/ValueTypeAssistant.hpp"

#include <gtest/gtest.h>

#include <cmath>
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

DataPath P (std::initializer_list<DataPath::Segment> segments)
{
    return DataPath (segments);
}

SharedMetadata Meta (const char* dotted, MetadataValue value, bool transformable = false)
{
    MetadataBuilder builder;
    std::string error;
    EXPECT_TRUE (builder.Set (Key (dotted), std::move (value), transformable, error)) << error;
    return std::move (builder).Finish ();
}

// Writes, re-reads, and hands back the typed tree that came out.
template <class T> std::shared_ptr<const DataTree<T>> RoundTrip (const DataTree<T>& tree)
{
    json::JsonValue encoded;
    std::string error;
    EXPECT_TRUE (SerializeTree (tree, encoded, error)) << error;

    // Through TEXT, not just through the value: a document is a file, and a
    // writer/parser disagreement is exactly what an in-memory hop would hide.
    const json::ParseResult parsed = json::Parse (json::Write (encoded));
    EXPECT_TRUE (parsed.ok) << parsed.error;

    TreeValue result;
    EXPECT_TRUE (DeserializeTree (parsed.value, result, error)) << error;
    EXPECT_EQ (result.itemType, tree.Type ());
    return std::dynamic_pointer_cast<const DataTree<T>> (result.tree);
}

} // namespace

// ---- Assistants ------------------------------------------------------------

TEST (ValueTypeAssistants, EveryDeclaredItemTypeHasOne)
{
    const std::vector<const ValueTypeAssistant*> all = AllValueTypeAssistants ();
    EXPECT_EQ (all.size (), 10u);

    for (const ValueTypeAssistant* assistant : all) {
        ASSERT_NE (assistant, nullptr);
        EXPECT_STREQ (assistant->name, ItemTypeName (assistant->itemType));
        EXPECT_NE (assistant->Hash, nullptr);
        EXPECT_NE (assistant->Equals, nullptr);
        EXPECT_NE (assistant->Format, nullptr);
        EXPECT_EQ (FindValueTypeAssistant (assistant->itemType), assistant);
        EXPECT_EQ (FindValueTypeAssistant (assistant->name), assistant);
    }
}

TEST (ValueTypeAssistants, MeshIsRegisteredButNotPersistable)
{
    const ValueTypeAssistant* mesh = FindValueTypeAssistant (ItemType::Mesh);
    ASSERT_NE (mesh, nullptr);
    EXPECT_FALSE (mesh->CanSerialize ());
    EXPECT_NE (mesh->Hash, nullptr); // Still hashable and printable.

    const auto tree = DataTree<Value::ImmutableMesh>::FromItem (std::make_shared<const geomsrv::Mesh> ());
    json::JsonValue encoded;
    std::string error;
    EXPECT_FALSE (SerializeTree (*tree, encoded, error));
    EXPECT_NE (error.find ("mesh"), std::string::npos);
}

TEST (ValueTypeAssistants, HashAndEqualsAgreeWithTheTypedTraits)
{
    const ValueTypeAssistant* assistant = FindValueTypeAssistant (ItemType::Double);
    ASSERT_NE (assistant, nullptr);

    const Value left (2.5);
    const Value right (2.5);
    EXPECT_TRUE (assistant->Equals (left, right));
    EXPECT_EQ (assistant->Hash (left), assistant->Hash (right));
    EXPECT_FALSE (assistant->Equals (left, Value (2.6)));
    EXPECT_FALSE (assistant->Format (left).empty ());
}

// ---- Round trips -----------------------------------------------------------

TEST (TreeSerialization, TopologyNullnessAndMetadataAllSurvive)
{
    DataTreeBuilder<int64_t> builder;
    builder.Add (P ({ 0 }), 1, Meta ("tapioca.source.run", int64_t { 9 }));
    builder.AddNull (P ({ 0 }), Meta ("a.b", std::string ("why")));
    builder.Add (P ({ 0 }), 3);
    builder.EnsureList (P ({ 1, 2 })); // Empty list: a fact, not an absence.
    const auto original = std::move (builder).Finish ();

    const auto restored = RoundTrip (*original);
    ASSERT_NE (restored, nullptr);
    EXPECT_TRUE (restored->Equals (*original));
    EXPECT_EQ (restored->Hash (), original->Hash ());

    ASSERT_EQ (restored->ListCount (), 2u);
    EXPECT_EQ (restored->PathAt (1), P ({ 1, 2 }));
    EXPECT_TRUE (restored->TypedListAt (1).IsEmpty ());
    EXPECT_TRUE (restored->TypedListAt (0).IsNullAt (1));
    EXPECT_EQ (std::get<std::string> (*restored->TypedListAt (0).MetadataAt (1)->FindValue (Key ("a.b"))), "why");
}

TEST (TreeSerialization, TheEmptyTreeIsNotTheTreeOfOneEmptyList)
{
    const auto empty = RoundTrip (*DataTree<double>::EmptyTree ());
    ASSERT_NE (empty, nullptr);
    EXPECT_EQ (empty->ListCount (), 0u);

    DataTreeBuilder<double> builder;
    builder.EnsureList (DataPath::Zero ());
    const auto oneEmptyList = RoundTrip (*std::move (builder).Finish ());
    ASSERT_NE (oneEmptyList, nullptr);
    EXPECT_EQ (oneEmptyList->ListCount (), 1u);
    EXPECT_EQ (oneEmptyList->ItemCount (), 0u);
}

TEST (TreeSerialization, NonFiniteDoublesComeBackAsThemselves)
{
    DataTreeBuilder<double> builder;
    builder.Add (P ({ 0 }), std::numeric_limits<double>::quiet_NaN ());
    builder.Add (P ({ 0 }), std::numeric_limits<double>::infinity ());
    builder.Add (P ({ 0 }), -std::numeric_limits<double>::infinity ());
    builder.Add (P ({ 0 }), -0.0);

    const auto restored = RoundTrip (*std::move (builder).Finish ());
    ASSERT_NE (restored, nullptr);
    const DataList<double>& list = restored->TypedListAt (0);
    ASSERT_EQ (list.Size (), 4u);
    EXPECT_TRUE (std::isnan (list.At (0)));
    EXPECT_TRUE (std::isinf (list.At (1)) && list.At (1) > 0.0);
    EXPECT_TRUE (std::isinf (list.At (2)) && list.At (2) < 0.0);
    EXPECT_TRUE (std::signbit (list.At (3))); // -0.0, not 0.0.

    // And none of them became a null item.
    EXPECT_FALSE (list.HasNulls ());
}

TEST (TreeSerialization, GeometryAndReferenceItemsRoundTrip)
{
    DataTreeBuilder<Polyline> lines;
    lines.Add (P ({ 0 }), Polyline { { Point3 { 0.0, 1.0, 2.0 }, Point3 { 3.0, 4.0, 5.0 } } });
    const auto restoredLines = RoundTrip (*std::move (lines).Finish ());
    ASSERT_NE (restoredLines, nullptr);
    ASSERT_EQ (restoredLines->TypedListAt (0).At (0).points.size (), 2u);
    EXPECT_EQ (restoredLines->TypedListAt (0).At (0).points[1].y, 4.0);

    DataTreeBuilder<ArchicadElementRef> refs;
    refs.Add (P ({ 0 }), ArchicadElementRef { "AABBCCDD-0000-0000-0000-000000000001" });
    const auto restoredRefs = RoundTrip (*std::move (refs).Finish ());
    ASSERT_NE (restoredRefs, nullptr);
    EXPECT_EQ (restoredRefs->TypedListAt (0).At (0).guid, "AABBCCDD-0000-0000-0000-000000000001");
}

TEST (TreeSerialization, AnItemInAnAnyTreeCarriesItsOwnType)
{
    DataTreeBuilder<Value> builder;
    builder.Add (P ({ 0 }), Value (int64_t { 7 }));
    builder.Add (P ({ 0 }), Value (std::string ("text")));
    builder.AddNull (P ({ 0 }));

    const auto restored = RoundTrip (*std::move (builder).Finish ());
    ASSERT_NE (restored, nullptr);
    ASSERT_EQ (restored->TypedListAt (0).Size (), 3u);
    EXPECT_EQ (restored->TypedListAt (0).At (0).Type (), ValueType::Integer);
    EXPECT_EQ (std::get<std::string> (restored->TypedListAt (0).At (1).DataValue ()), "text");
    EXPECT_TRUE (restored->TypedListAt (0).IsNullAt (2));
}

TEST (TreeSerialization, MetadataKeepsItsValueTypeAndTransformFlag)
{
    MetadataBuilder builder;
    std::string error;
    ASSERT_TRUE (builder.Set (Key ("tapioca.transform.origin"), Point3 { 1.0, 2.0, 3.0 }, true, error));
    ASSERT_TRUE (builder.Set (Key ("tapioca.preview.colour"), ColourRgba { 10, 20, 30, 40 }, error));
    ASSERT_TRUE (builder.Set (Key ("tapioca.unit.count"), int64_t { 5 }, error));
    ASSERT_TRUE (builder.Set (Key ("tapioca.unit.scale"), 0.5, error));
    ASSERT_TRUE (builder.Set (Key ("tapioca.display.on"), true, error));
    const SharedMetadata original = std::move (builder).Finish ();

    json::JsonValue encoded;
    ASSERT_TRUE (SerializeMetadata (*original, encoded, error)) << error;
    SharedMetadata restored;
    ASSERT_TRUE (DeserializeMetadata (encoded, restored, error)) << error;

    EXPECT_TRUE (MetadataEquals (original, restored));
    const MetadataEntry* origin = restored->Find (Key ("tapioca.transform.origin"));
    ASSERT_NE (origin, nullptr);
    EXPECT_TRUE (origin->transformable);
    EXPECT_EQ (std::get<Point3> (origin->value).z, 3.0);
    EXPECT_FALSE (restored->Find (Key ("tapioca.preview.colour"))->transformable);
}

// ---- Rejections ------------------------------------------------------------

TEST (TreeSerialization, MalformedDocumentsAreRefusedAndSayWhere)
{
    TreeValue result;
    std::string error;

    const auto read = [&] (const char* text) {
        const json::ParseResult parsed = json::Parse (text);
        EXPECT_TRUE (parsed.ok) << parsed.error;
        error.clear ();
        return DeserializeTree (parsed.value, result, error);
    };

    EXPECT_FALSE (read (R"({"lists": []})"));                                     // No item type.
    EXPECT_FALSE (read (R"({"itemType": "widget", "lists": []})"));               // Unknown type.
    EXPECT_FALSE (read (R"({"itemType": "mesh", "lists": []})"));                 // Not persistable.
    EXPECT_FALSE (read (R"({"itemType": "integer", "lists": {}})"));              // Lists is not an array.
    EXPECT_FALSE (read (R"({"itemType": "integer", "lists": [{"items": []}]})")); // No path.

    EXPECT_FALSE (read (R"({"itemType": "integer", "lists": [{"path": [], "items": []}]})"));
    EXPECT_NE (error.find ("segment"), std::string::npos);

    EXPECT_FALSE (read (R"({"itemType": "integer", "lists": [{"path": [-1], "items": []}]})"));
    EXPECT_FALSE (read (R"({"itemType": "integer", "lists": [{"path": [0], "items": ["nope"]}]})"));
    EXPECT_NE (error.find ("{0}[0]"), std::string::npos);
}

TEST (TreeSerialization, BadMetadataIsRefusedNamingItsKey)
{
    TreeValue result;
    std::string error;
    const json::ParseResult parsed = json::Parse (R"({
        "itemType": "integer",
        "lists": [{ "path": [0], "items": [1],
                    "meta": [{ "index": 0,
                               "entries": [{ "key": "a.b", "type": "point3", "value": [1, 2] }] }] }]
    })");
    ASSERT_TRUE (parsed.ok) << parsed.error;

    EXPECT_FALSE (DeserializeTree (parsed.value, result, error));
    EXPECT_NE (error.find ("a.b"), std::string::npos);
}
