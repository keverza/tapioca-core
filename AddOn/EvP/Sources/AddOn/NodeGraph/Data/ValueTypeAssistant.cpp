#include "NodeGraph/Data/ValueTypeAssistant.hpp"

#include "NodeGraph/ValueText.hpp"

#include <cmath>
#include <iterator>
#include <limits>

namespace evp::nodegraph::data {
namespace {

using json::JsonArray;
using json::JsonObject;
using json::JsonValue;

JsonValue EncodePoint (const Point3& point)
{
    return JsonValue::Array (
        JsonArray { EncodeJsonDouble (point.x), EncodeJsonDouble (point.y), EncodeJsonDouble (point.z) });
}

bool DecodePoint (const JsonValue& encoded, Point3& result, std::string& error)
{
    const JsonArray* array = encoded.AsArray ();
    if (array == nullptr || array->size () != 3) {
        error = "Expected a point as [x, y, z]";
        return false;
    }
    return DecodeJsonDouble ((*array)[0], result.x, error) && DecodeJsonDouble ((*array)[1], result.y, error) &&
           DecodeJsonDouble ((*array)[2], result.z, error);
}

JsonValue EncodePoints (const std::vector<Point3>& points)
{
    JsonArray array;
    array.reserve (points.size ());
    for (const Point3& point : points)
        array.push_back (EncodePoint (point));
    return JsonValue::Array (std::move (array));
}

bool DecodePoints (const JsonValue& encoded, std::vector<Point3>& result, std::string& error)
{
    const JsonArray* array = encoded.AsArray ();
    if (array == nullptr) {
        error = "Expected an array of points";
        return false;
    }
    result.clear ();
    result.reserve (array->size ());
    for (const JsonValue& entry : *array) {
        Point3 point;
        if (!DecodePoint (entry, point, error))
            return false;
        result.push_back (point);
    }
    return true;
}

// Every assistant shares these two: item equality and item hashing are already
// defined once, over the erased Value, in AtomicValue.cpp. Duplicating them per
// type is how two items start comparing equal in one place and not the other.
size_t HashItem (const Value& item)
{
    return ItemTraits<Value>::Hash (item);
}

bool ItemsEqual (const Value& left, const Value& right)
{
    return ItemTraits<Value>::Equals (left, right);
}

std::string FormatItem (const Value& item)
{
    return FormatValue (item);
}

bool SerializeBool (const Value& item, JsonValue& result, std::string& error)
{
    const bool* value = std::get_if<bool> (&item.DataValue ());
    if (value == nullptr) {
        error = "Expected a bool item";
        return false;
    }
    result = JsonValue::Bool (*value);
    return true;
}

bool DeserializeBool (const JsonValue& encoded, Value& result, std::string& error)
{
    bool value = false;
    if (!encoded.AsBool (value)) {
        error = "Expected a bool";
        return false;
    }
    result = Value (value);
    return true;
}

bool SerializeInteger (const Value& item, JsonValue& result, std::string& error)
{
    const int64_t* value = std::get_if<int64_t> (&item.DataValue ());
    if (value == nullptr) {
        error = "Expected an integer item";
        return false;
    }
    result = JsonValue::Integer (*value);
    return true;
}

bool DeserializeInteger (const JsonValue& encoded, Value& result, std::string& error)
{
    int64_t value = 0;
    if (!encoded.AsInteger (value)) {
        error = "Expected an integer";
        return false;
    }
    result = Value (value);
    return true;
}

bool SerializeDouble (const Value& item, JsonValue& result, std::string& error)
{
    const double* value = std::get_if<double> (&item.DataValue ());
    if (value == nullptr) {
        error = "Expected a double item";
        return false;
    }
    result = EncodeJsonDouble (*value);
    return true;
}

bool DeserializeDouble (const JsonValue& encoded, Value& result, std::string& error)
{
    double value = 0.0;
    if (!DecodeJsonDouble (encoded, value, error))
        return false;
    result = Value (value);
    return true;
}

bool SerializeString (const Value& item, JsonValue& result, std::string& error)
{
    const std::string* value = std::get_if<std::string> (&item.DataValue ());
    if (value == nullptr) {
        error = "Expected a string item";
        return false;
    }
    result = JsonValue::String (*value);
    return true;
}

bool DeserializeString (const JsonValue& encoded, Value& result, std::string& error)
{
    std::string value;
    if (!encoded.AsString (value)) {
        error = "Expected a string";
        return false;
    }
    result = Value (std::move (value));
    return true;
}

bool SerializePoint (const Value& item, JsonValue& result, std::string& error)
{
    const Point3* value = std::get_if<Point3> (&item.DataValue ());
    if (value == nullptr) {
        error = "Expected a point item";
        return false;
    }
    result = EncodePoint (*value);
    return true;
}

bool DeserializePoint (const JsonValue& encoded, Value& result, std::string& error)
{
    Point3 value;
    if (!DecodePoint (encoded, value, error))
        return false;
    result = Value (value);
    return true;
}

bool SerializePolyline (const Value& item, JsonValue& result, std::string& error)
{
    const Polyline* value = std::get_if<Polyline> (&item.DataValue ());
    if (value == nullptr) {
        error = "Expected a polyline item";
        return false;
    }
    result = EncodePoints (value->points);
    return true;
}

bool DeserializePolyline (const JsonValue& encoded, Value& result, std::string& error)
{
    Polyline value;
    if (!DecodePoints (encoded, value.points, error))
        return false;
    result = Value (std::move (value));
    return true;
}

bool SerializePolygon (const Value& item, JsonValue& result, std::string& error)
{
    const Polygon* value = std::get_if<Polygon> (&item.DataValue ());
    if (value == nullptr) {
        error = "Expected a polygon item";
        return false;
    }
    result = EncodePoints (value->points);
    return true;
}

bool DeserializePolygon (const JsonValue& encoded, Value& result, std::string& error)
{
    Polygon value;
    if (!DecodePoints (encoded, value.points, error))
        return false;
    result = Value (std::move (value));
    return true;
}

bool SerializeElementRef (const Value& item, JsonValue& result, std::string& error)
{
    const ArchicadElementRef* value = std::get_if<ArchicadElementRef> (&item.DataValue ());
    if (value == nullptr) {
        error = "Expected an element reference item";
        return false;
    }
    result = JsonValue::Object (JsonObject { { "guid", JsonValue::String (value->guid) } });
    return true;
}

bool DeserializeElementRef (const JsonValue& encoded, Value& result, std::string& error)
{
    const JsonValue* guid = encoded.Find ("guid");
    std::string text;
    if (guid == nullptr || !guid->AsString (text)) {
        error = "Expected an element reference as { \"guid\": ... }";
        return false;
    }
    result = Value (ArchicadElementRef { std::move (text) });
    return true;
}

// An Any item carries its own type, because the tree it lives in declares none.
bool SerializeAny (const Value& item, JsonValue& result, std::string& error)
{
    const std::optional<ItemType> type = ItemTypeFromValueType (item.Type ());
    if (!type.has_value ()) {
        error = "An 'any' tree cannot hold a list or an absent value";
        return false;
    }
    const ValueTypeAssistant* assistant = FindValueTypeAssistant (*type);
    if (assistant == nullptr || !assistant->CanSerialize ()) {
        error = std::string ("Items of type '") + ItemTypeName (*type) + "' cannot be serialised";
        return false;
    }

    JsonValue encoded;
    if (!assistant->Serialize (item, encoded, error))
        return false;
    result = JsonValue::Object (JsonObject { { "type", JsonValue::String (assistant->name) }, { "value", encoded } });
    return true;
}

bool DeserializeAny (const JsonValue& encoded, Value& result, std::string& error)
{
    const JsonValue* type = encoded.Find ("type");
    const JsonValue* value = encoded.Find ("value");
    std::string name;
    if (type == nullptr || value == nullptr || !type->AsString (name)) {
        error = "Expected an any item as { \"type\": ..., \"value\": ... }";
        return false;
    }

    const ValueTypeAssistant* assistant = FindValueTypeAssistant (name);
    if (assistant == nullptr || assistant->itemType == ItemType::Any || !assistant->CanSerialize ()) {
        error = "Unknown or non-serialisable item type: " + name;
        return false;
    }
    return assistant->Deserialize (*value, result, error);
}

const ValueTypeAssistant kAssistants[] = {
    { ItemType::Bool, "bool", HashItem, ItemsEqual, FormatItem, SerializeBool, DeserializeBool },
    { ItemType::Integer, "integer", HashItem, ItemsEqual, FormatItem, SerializeInteger, DeserializeInteger },
    { ItemType::Double, "double", HashItem, ItemsEqual, FormatItem, SerializeDouble, DeserializeDouble },
    { ItemType::String, "string", HashItem, ItemsEqual, FormatItem, SerializeString, DeserializeString },
    { ItemType::Point3, "point3", HashItem, ItemsEqual, FormatItem, SerializePoint, DeserializePoint },
    { ItemType::Polyline, "polyline", HashItem, ItemsEqual, FormatItem, SerializePolyline, DeserializePolyline },
    { ItemType::Polygon, "polygon", HashItem, ItemsEqual, FormatItem, SerializePolygon, DeserializePolygon },
    // Mesh: see the header. Deliberately not persistable.
    { ItemType::Mesh, "mesh", HashItem, ItemsEqual, FormatItem, nullptr, nullptr },
    { ItemType::ElementRef, "elementRef", HashItem, ItemsEqual, FormatItem, SerializeElementRef,
      DeserializeElementRef },
    { ItemType::Any, "any", HashItem, ItemsEqual, FormatItem, SerializeAny, DeserializeAny },
};

} // namespace

json::JsonValue EncodeJsonDouble (double value)
{
    if (std::isnan (value))
        return JsonValue::String ("NaN");
    if (std::isinf (value))
        return JsonValue::String (value > 0.0 ? "Infinity" : "-Infinity");
    if (value == 0.0 && std::signbit (value))
        return JsonValue::String ("-0");
    return JsonValue::Double (value);
}

bool DecodeJsonDouble (const json::JsonValue& encoded, double& result, std::string& error)
{
    std::string name;
    if (encoded.AsString (name)) {
        if (name == "NaN") {
            result = std::numeric_limits<double>::quiet_NaN ();
            return true;
        }
        if (name == "Infinity") {
            result = std::numeric_limits<double>::infinity ();
            return true;
        }
        if (name == "-Infinity") {
            result = -std::numeric_limits<double>::infinity ();
            return true;
        }
        if (name == "-0") {
            result = -0.0;
            return true;
        }
        error = "Expected a number, got the string \"" + name + "\"";
        return false;
    }
    if (encoded.AsDouble (result))
        return true;

    error = "Expected a number";
    return false;
}

const ValueTypeAssistant* FindValueTypeAssistant (ItemType type)
{
    for (const ValueTypeAssistant& assistant : kAssistants) {
        if (assistant.itemType == type)
            return &assistant;
    }
    return nullptr;
}

const ValueTypeAssistant* FindValueTypeAssistant (std::string_view name)
{
    for (const ValueTypeAssistant& assistant : kAssistants) {
        if (name == assistant.name)
            return &assistant;
    }
    return nullptr;
}

std::vector<const ValueTypeAssistant*> AllValueTypeAssistants ()
{
    std::vector<const ValueTypeAssistant*> all;
    all.reserve (std::size (kAssistants));
    for (const ValueTypeAssistant& assistant : kAssistants)
        all.push_back (&assistant);
    return all;
}

} // namespace evp::nodegraph::data
