#include "NodeGraph/Data/TreeSerialization.hpp"

#include "NodeGraph/Data/ValueTypeAssistant.hpp"

#include <cmath>
#include <limits>

namespace evp::nodegraph::data {
namespace {

using json::JsonArray;
using json::JsonObject;
using json::JsonValue;

// ---- metadata values -------------------------------------------------------
//
// A separate encoding from the item one: MetadataValue is its own variant (7.7)
// and shares only two or three members with the item vocabulary. Tagging each
// entry with its type name rather than its variant index is what lets the
// variant grow without invalidating every saved document.

bool DecodeTriple (const JsonValue& encoded, double& x, double& y, double& z, std::string& error)
{
    const JsonArray* array = encoded.AsArray ();
    if (array == nullptr || array->size () != 3) {
        error = "Expected three numbers";
        return false;
    }
    return DecodeJsonDouble ((*array)[0], x, error) && DecodeJsonDouble ((*array)[1], y, error) &&
           DecodeJsonDouble ((*array)[2], z, error);
}

const char* MetadataValueTypeName (const MetadataValue& value)
{
    if (std::holds_alternative<bool> (value))
        return "bool";
    if (std::holds_alternative<int64_t> (value))
        return "integer";
    if (std::holds_alternative<double> (value))
        return "double";
    if (std::holds_alternative<std::string> (value))
        return "string";
    if (std::holds_alternative<Point3> (value))
        return "point3";
    if (std::holds_alternative<Vector3> (value))
        return "vector3";
    if (std::holds_alternative<Transform> (value))
        return "transform";
    return "colour";
}

JsonValue EncodeMetadataValue (const MetadataValue& value)
{
    if (const auto* flag = std::get_if<bool> (&value))
        return JsonValue::Bool (*flag);
    if (const auto* integer = std::get_if<int64_t> (&value))
        return JsonValue::Integer (*integer);
    if (const auto* number = std::get_if<double> (&value))
        return EncodeJsonDouble (*number);
    if (const auto* text = std::get_if<std::string> (&value))
        return JsonValue::String (*text);
    if (const auto* point = std::get_if<Point3> (&value))
        return JsonValue::Array (
            JsonArray { EncodeJsonDouble (point->x), EncodeJsonDouble (point->y), EncodeJsonDouble (point->z) });
    if (const auto* vector = std::get_if<Vector3> (&value))
        return JsonValue::Array (
            JsonArray { EncodeJsonDouble (vector->x), EncodeJsonDouble (vector->y), EncodeJsonDouble (vector->z) });
    if (const auto* transform = std::get_if<Transform> (&value)) {
        JsonArray cells;
        cells.reserve (12);
        for (const auto& row : transform->m) {
            for (double cell : row)
                cells.push_back (EncodeJsonDouble (cell));
        }
        return JsonValue::Array (std::move (cells));
    }

    const ColourRgba& colour = std::get<ColourRgba> (value);
    return JsonValue::Array (JsonArray { JsonValue::Integer (colour.red), JsonValue::Integer (colour.green),
                                         JsonValue::Integer (colour.blue), JsonValue::Integer (colour.alpha) });
}

bool DecodeMetadataValue (const std::string& type, const JsonValue& encoded, MetadataValue& result, std::string& error)
{
    if (type == "bool") {
        bool flag = false;
        if (!encoded.AsBool (flag)) {
            error = "Expected a bool";
            return false;
        }
        result = flag;
        return true;
    }
    if (type == "integer") {
        int64_t integer = 0;
        if (!encoded.AsInteger (integer)) {
            error = "Expected an integer";
            return false;
        }
        result = integer;
        return true;
    }
    if (type == "double") {
        double number = 0.0;
        if (!DecodeJsonDouble (encoded, number, error))
            return false;
        result = number;
        return true;
    }
    if (type == "string") {
        std::string text;
        if (!encoded.AsString (text)) {
            error = "Expected a string";
            return false;
        }
        result = std::move (text);
        return true;
    }
    if (type == "point3") {
        Point3 point;
        if (!DecodeTriple (encoded, point.x, point.y, point.z, error))
            return false;
        result = point;
        return true;
    }
    if (type == "vector3") {
        Vector3 vector;
        if (!DecodeTriple (encoded, vector.x, vector.y, vector.z, error))
            return false;
        result = vector;
        return true;
    }
    if (type == "transform") {
        const JsonArray* cells = encoded.AsArray ();
        if (cells == nullptr || cells->size () != 12) {
            error = "Expected a transform as twelve numbers";
            return false;
        }
        Transform transform;
        for (size_t index = 0; index < 12; ++index) {
            if (!DecodeJsonDouble ((*cells)[index], transform.m[index / 4][index % 4], error))
                return false;
        }
        result = transform;
        return true;
    }
    if (type == "colour") {
        const JsonArray* channels = encoded.AsArray ();
        if (channels == nullptr || channels->size () != 4) {
            error = "Expected a colour as four channels";
            return false;
        }
        ColourRgba colour;
        uint8_t* targets[] = { &colour.red, &colour.green, &colour.blue, &colour.alpha };
        for (size_t index = 0; index < 4; ++index) {
            int64_t channel = 0;
            if (!(*channels)[index].AsInteger (channel) || channel < 0 || channel > 255) {
                error = "Colour channels must be integers in 0..255";
                return false;
            }
            *targets[index] = static_cast<uint8_t> (channel);
        }
        result = colour;
        return true;
    }

    error = "Unknown metadata value type: " + type;
    return false;
}

// ---- items -----------------------------------------------------------------

bool SerializeItem (const IDataList& list, size_t index, const ValueTypeAssistant& assistant, JsonValue& result,
                    std::string& error)
{
    const std::optional<Value> value = list.ValueAt (index);
    if (!value.has_value ()) {
        // JSON null IS the null item: no assistant encodes to null, so the two
        // can never be confused on the way back in.
        result = JsonValue ();
        return true;
    }
    return assistant.Serialize (*value, result, error);
}

// One typed builder pass, chosen by the item type the document declares.
template <class T>
bool BuildTree (const JsonArray& lists, const ValueTypeAssistant& assistant, TreeValue& result, std::string& error);

template <class T> bool ItemFromValue (const Value& value, T& result)
{
    const T* held = std::get_if<T> (&value.DataValue ());
    if (held == nullptr)
        return false;
    result = *held;
    return true;
}

template <> bool ItemFromValue<Value> (const Value& value, Value& result)
{
    result = value;
    return true;
}

} // namespace

bool SerializeMetadata (const MetadataMap& metadata, json::JsonValue& result, std::string& error)
{
    JsonArray entries;
    entries.reserve (metadata.Size ());
    for (const MetadataEntry& entry : metadata.Entries ()) {
        entries.push_back (
            JsonValue::Object (JsonObject { { "key", JsonValue::String (entry.key.ToString ()) },
                                            { "type", JsonValue::String (MetadataValueTypeName (entry.value)) },
                                            { "value", EncodeMetadataValue (entry.value) },
                                            { "transformable", JsonValue::Bool (entry.transformable) } }));
    }
    (void) error;
    result = JsonValue::Array (std::move (entries));
    return true;
}

bool DeserializeMetadata (const json::JsonValue& encoded, SharedMetadata& result, std::string& error)
{
    const JsonArray* entries = encoded.AsArray ();
    if (entries == nullptr) {
        error = "Expected metadata as an array of entries";
        return false;
    }

    MetadataBuilder builder;
    for (const JsonValue& entry : *entries) {
        const JsonValue* key = entry.Find ("key");
        const JsonValue* type = entry.Find ("type");
        const JsonValue* value = entry.Find ("value");
        std::string keyText;
        std::string typeText;
        if (key == nullptr || type == nullptr || value == nullptr || !key->AsString (keyText) ||
            !type->AsString (typeText)) {
            error = "A metadata entry needs a key, a type and a value";
            return false;
        }

        const std::optional<MetadataKey> parsed = MetadataKey::Parse (keyText);
        if (!parsed.has_value ()) {
            error = "Invalid metadata key: " + keyText;
            return false;
        }

        MetadataValue decoded;
        if (!DecodeMetadataValue (typeText, *value, decoded, error)) {
            error = "Metadata key " + keyText + ": " + error;
            return false;
        }

        bool transformable = false;
        if (const JsonValue* flag = entry.Find ("transformable"))
            flag->AsBool (transformable);

        if (!builder.Set (*parsed, std::move (decoded), transformable, error))
            return false;
    }

    result = std::move (builder).Finish ();
    return true;
}

bool SerializeTree (const IDataTree& tree, json::JsonValue& result, std::string& error)
{
    const ValueTypeAssistant* assistant = FindValueTypeAssistant (tree.Type ());
    if (assistant == nullptr) {
        error = "No assistant is registered for this item type";
        return false;
    }
    if (!assistant->CanSerialize ()) {
        error = std::string ("Trees of '") + assistant->name + "' items cannot be serialised";
        return false;
    }

    JsonArray lists;
    lists.reserve (tree.ListCount ());
    for (size_t listIndex = 0; listIndex < tree.ListCount (); ++listIndex) {
        const DataPath& path = tree.Paths ()[listIndex];
        const IDataList& list = tree.ListAt (listIndex);

        JsonArray segments;
        segments.reserve (path.Length ());
        for (DataPath::Segment segment : path.Segments ())
            segments.push_back (JsonValue::Integer (segment));

        JsonArray items;
        JsonArray metadata;
        items.reserve (list.Size ());
        for (size_t index = 0; index < list.Size (); ++index) {
            JsonValue item;
            if (!SerializeItem (list, index, *assistant, item, error)) {
                error = path.ToString () + "[" + std::to_string (index) + "]: " + error;
                return false;
            }
            items.push_back (std::move (item));

            const SharedMetadata& itemMetadata = list.MetadataAt (index);
            if (itemMetadata->IsEmpty ())
                continue;

            JsonValue encodedMetadata;
            if (!SerializeMetadata (*itemMetadata, encodedMetadata, error)) {
                error = path.ToString () + "[" + std::to_string (index) + "]: " + error;
                return false;
            }
            metadata.push_back (
                JsonValue::Object (JsonObject { { "index", JsonValue::Integer (static_cast<int64_t> (index)) },
                                                { "entries", std::move (encodedMetadata) } }));
        }

        JsonObject listObject { { "path", JsonValue::Array (std::move (segments)) },
                                { "items", JsonValue::Array (std::move (items)) } };
        if (!metadata.empty ())
            listObject.emplace ("meta", JsonValue::Array (std::move (metadata)));
        lists.push_back (JsonValue::Object (std::move (listObject)));
    }

    result = JsonValue::Object (JsonObject { { "itemType", JsonValue::String (assistant->name) },
                                             { "lists", JsonValue::Array (std::move (lists)) } });
    return true;
}

namespace {

bool ReadPath (const JsonValue& encoded, DataPath& result, std::string& error)
{
    const JsonArray* segments = encoded.AsArray ();
    if (segments == nullptr || segments->empty ()) {
        error = "A path needs at least one segment";
        return false;
    }

    std::vector<DataPath::Segment> parsed;
    parsed.reserve (segments->size ());
    for (const JsonValue& segment : *segments) {
        int64_t value = 0;
        if (!segment.AsInteger (value) || value < 0 ||
            value > static_cast<int64_t> (std::numeric_limits<DataPath::Segment>::max ())) {
            error = "Path segments must be non-negative integers";
            return false;
        }
        parsed.push_back (static_cast<DataPath::Segment> (value));
    }

    const std::optional<DataPath> path = DataPath::TryCreate (std::move (parsed));
    if (!path.has_value ()) {
        error = "A path needs at least one segment";
        return false;
    }
    result = *path;
    return true;
}

// index -> metadata, for one list.
using MetadataBySite = std::map<size_t, SharedMetadata>;

bool ReadListMetadata (const JsonValue& list, MetadataBySite& result, std::string& error)
{
    const JsonValue* meta = list.Find ("meta");
    if (meta == nullptr)
        return true;

    const JsonArray* entries = meta->AsArray ();
    if (entries == nullptr) {
        error = "Expected 'meta' to be an array";
        return false;
    }
    for (const JsonValue& entry : *entries) {
        const JsonValue* index = entry.Find ("index");
        const JsonValue* content = entry.Find ("entries");
        int64_t site = 0;
        if (index == nullptr || content == nullptr || !index->AsInteger (site) || site < 0) {
            error = "A metadata group needs an item index and its entries";
            return false;
        }

        SharedMetadata metadata;
        if (!DeserializeMetadata (*content, metadata, error))
            return false;
        result.emplace (static_cast<size_t> (site), std::move (metadata));
    }
    return true;
}

template <class T>
bool BuildTree (const JsonArray& lists, const ValueTypeAssistant& assistant, TreeValue& result, std::string& error)
{
    DataTreeBuilder<T> builder;
    for (const JsonValue& list : lists) {
        const JsonValue* pathValue = list.Find ("path");
        const JsonValue* itemsValue = list.Find ("items");
        if (pathValue == nullptr || itemsValue == nullptr) {
            error = "A list needs a path and an items array";
            return false;
        }

        DataPath path;
        if (!ReadPath (*pathValue, path, error))
            return false;

        const JsonArray* items = itemsValue->AsArray ();
        if (items == nullptr) {
            error = path.ToString () + ": expected 'items' to be an array";
            return false;
        }

        MetadataBySite metadata;
        if (!ReadListMetadata (list, metadata, error)) {
            error = path.ToString () + ": " + error;
            return false;
        }

        // An empty list is a list, not an absence, so the path is claimed
        // before the items are read (7.5).
        builder.EnsureList (path);

        for (size_t index = 0; index < items->size (); ++index) {
            const auto found = metadata.find (index);
            SharedMetadata itemMetadata = found == metadata.end () ? MetadataMap::Empty () : found->second;

            if ((*items)[index].IsNull ()) {
                builder.AddNull (path, std::move (itemMetadata));
                continue;
            }

            Value value;
            if (!assistant.Deserialize ((*items)[index], value, error)) {
                error = path.ToString () + "[" + std::to_string (index) + "]: " + error;
                return false;
            }

            T item;
            if (!ItemFromValue<T> (value, item)) {
                error = path.ToString () + "[" + std::to_string (index) + "]: item is not a " + assistant.name;
                return false;
            }
            builder.Add (path, std::move (item), std::move (itemMetadata));
        }
    }

    result = MakeTreeValue<T> (std::move (builder).Finish ());
    return true;
}

} // namespace

bool DeserializeTree (const json::JsonValue& encoded, TreeValue& result, std::string& error)
{
    const JsonValue* typeValue = encoded.Find ("itemType");
    const JsonValue* listsValue = encoded.Find ("lists");
    std::string typeName;
    if (typeValue == nullptr || listsValue == nullptr || !typeValue->AsString (typeName)) {
        error = "Expected a tree as { \"itemType\": ..., \"lists\": [...] }";
        return false;
    }

    const JsonArray* lists = listsValue->AsArray ();
    if (lists == nullptr) {
        error = "Expected 'lists' to be an array";
        return false;
    }

    const ValueTypeAssistant* assistant = FindValueTypeAssistant (typeName);
    if (assistant == nullptr) {
        error = "Unknown item type: " + typeName;
        return false;
    }
    if (!assistant->CanSerialize ()) {
        error = "Trees of '" + typeName + "' items cannot be deserialised";
        return false;
    }

    switch (assistant->itemType) {
        case ItemType::Bool:
            return BuildTree<bool> (*lists, *assistant, result, error);
        case ItemType::Integer:
            return BuildTree<int64_t> (*lists, *assistant, result, error);
        case ItemType::Double:
            return BuildTree<double> (*lists, *assistant, result, error);
        case ItemType::String:
            return BuildTree<std::string> (*lists, *assistant, result, error);
        case ItemType::Point3:
            return BuildTree<Point3> (*lists, *assistant, result, error);
        case ItemType::Polyline:
            return BuildTree<Polyline> (*lists, *assistant, result, error);
        case ItemType::Polygon:
            return BuildTree<Polygon> (*lists, *assistant, result, error);
        case ItemType::ElementRef:
            return BuildTree<ArchicadElementRef> (*lists, *assistant, result, error);
        case ItemType::Any:
            return BuildTree<Value> (*lists, *assistant, result, error);
        case ItemType::Mesh:
            break; // Refused above by CanSerialize.
    }

    error = "Unknown item type: " + typeName;
    return false;
}

} // namespace evp::nodegraph::data
