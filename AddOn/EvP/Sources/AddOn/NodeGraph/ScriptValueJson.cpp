#include "NodeGraph/ScriptValueJson.hpp"

#include <vector>

namespace evp::nodegraph {

using json::JsonArray;
using json::JsonObject;
using json::JsonValue;

namespace {

JsonValue PointToJson (const Point3& point)
{
    JsonObject object;
    object.emplace ("x", JsonValue::Double (point.x));
    object.emplace ("y", JsonValue::Double (point.y));
    object.emplace ("z", JsonValue::Double (point.z));
    return JsonValue::Object (std::move (object));
}

JsonValue PointsToJson (const std::vector<Point3>& points)
{
    JsonArray array;
    array.reserve (points.size ());
    for (const Point3& point : points)
        array.push_back (PointToJson (point));
    return JsonValue::Array (std::move (array));
}

bool PointFromJson (const JsonValue& source, Point3& out, std::string& error)
{
    const JsonObject* object = source.AsObject ();
    if (object == nullptr) {
        error = "expected a point like { x: 0, y: 0, z: 0 }";
        return false;
    }
    double coordinates[3] = { 0.0, 0.0, 0.0 };
    const char* names[3] = { "x", "y", "z" };
    for (size_t axis = 0; axis < 3; ++axis) {
        const JsonValue* member = source.Find (names[axis]);
        // A missing axis is zero rather than an error: a script working in plan
        // naturally writes { x, y } and means z = 0, and refusing that would make
        // every 2D script carry a component it does not care about.
        if (member != nullptr && !member->AsDouble (coordinates[axis])) {
            error = std::string ("a point's ") + names[axis] + " is not a number";
            return false;
        }
    }
    out = Point3 { coordinates[0], coordinates[1], coordinates[2] };
    return true;
}

bool PointsFromJson (const JsonValue& source, std::vector<Point3>& out, std::string& error)
{
    const JsonArray* array = source.AsArray ();
    if (array == nullptr) {
        error = "expected an array of points";
        return false;
    }
    out.clear ();
    out.reserve (array->size ());
    for (const JsonValue& element : *array) {
        Point3 point {};
        if (!PointFromJson (element, point, error))
            return false;
        out.push_back (point);
    }
    return true;
}

} // namespace

JsonValue ScriptValueToJson (const Value& value)
{
    switch (value.Type ()) {
        case ValueType::Absent:
            return JsonValue {};
        case ValueType::Bool:
            return JsonValue::Bool (std::get<bool> (value.DataValue ()));
        case ValueType::Integer:
            return JsonValue::Integer (std::get<int64_t> (value.DataValue ()));
        case ValueType::Double:
            return JsonValue::Double (std::get<double> (value.DataValue ()));
        case ValueType::String:
            return JsonValue::String (std::get<std::string> (value.DataValue ()));
        case ValueType::Point3:
            return PointToJson (std::get<Point3> (value.DataValue ()));
        case ValueType::Polyline:
            return PointsToJson (std::get<Polyline> (value.DataValue ()).points);
        case ValueType::Polygon:
            return PointsToJson (std::get<Polygon> (value.DataValue ()).points);
        case ValueType::ArchicadElementRef: {
            JsonObject object;
            object.emplace ("elementGuid", JsonValue::String (std::get<ArchicadElementRef> (value.DataValue ()).guid));
            return JsonValue::Object (std::move (object));
        }
        case ValueType::Mesh: {
            const Value::ImmutableMesh& mesh = std::get<Value::ImmutableMesh> (value.DataValue ());
            JsonObject object;
            object.emplace ("isMesh", JsonValue::Bool (true));
            object.emplace ("vertexCount", JsonValue::Integer (mesh ? static_cast<int64_t> (mesh->VertexCount ()) : 0));
            object.emplace ("triangleCount",
                            JsonValue::Integer (mesh ? static_cast<int64_t> (mesh->TriangleCount ()) : 0));
            return JsonValue::Object (std::move (object));
        }
        case ValueType::List:
            // Unreachable: a Value can no longer carry a List - see the
            // Argument overload, which handles the branch before an item is
            // ever passed here.
            return JsonValue {};
    }
    return JsonValue {};
}

JsonValue ScriptValueToJson (const Argument& value)
{
    if (value.Type () != ValueType::List)
        return ScriptValueToJson (value.AsValue ());
    JsonArray array;
    for (const Value& item : value.Items ())
        array.push_back (ScriptValueToJson (item));
    return JsonValue::Array (std::move (array));
}

bool ScriptValueFromJson (const JsonValue& source, ValueType expected, Value& out, std::string& error)
{
    switch (expected) {
        case ValueType::Bool: {
            bool flag = false;
            if (!source.AsBool (flag)) {
                error = "expected true or false";
                return false;
            }
            out = Value (flag);
            return true;
        }
        case ValueType::Integer: {
            int64_t number = 0;
            // ⚠️ IsIntegral() FIRST. AsInteger succeeds on any number and
            // TRUNCATES, so gating on it alone silently accepted 4.5 as 4 - which
            // is the one behaviour an integer port must not have, because the
            // script author never sees that their value was quietly changed. The
            // graph file's decoder gates on the same flag for the same reason.
            if (source.IsIntegral () && source.AsInteger (number)) {
                out = Value (number);
                return true;
            }
            // A script that computed 4.0 and assigned it to an integer port meant
            // 4. Truncating silently would be worse, so only an exact whole
            // number is accepted and anything else says what it wanted.
            double fractional = 0.0;
            if (!source.AsDouble (fractional) ||
                fractional != static_cast<double> (static_cast<int64_t> (fractional))) {
                error = "expected a whole number";
                return false;
            }
            out = Value (static_cast<int64_t> (fractional));
            return true;
        }
        case ValueType::Double: {
            double number = 0.0;
            if (!source.AsDouble (number)) {
                error = "expected a number";
                return false;
            }
            out = Value (number);
            return true;
        }
        case ValueType::String: {
            std::string text;
            if (!source.AsString (text)) {
                error = "expected text";
                return false;
            }
            out = Value (std::move (text));
            return true;
        }
        case ValueType::Point3: {
            Point3 point {};
            if (!PointFromJson (source, point, error))
                return false;
            out = Value (point);
            return true;
        }
        case ValueType::Polyline: {
            Polyline polyline;
            if (!PointsFromJson (source, polyline.points, error))
                return false;
            out = Value (std::move (polyline));
            return true;
        }
        case ValueType::Polygon: {
            Polygon polygon;
            if (!PointsFromJson (source, polygon.points, error))
                return false;
            out = Value (std::move (polygon));
            return true;
        }
        case ValueType::List:
            // Unreachable: a scalar Value can no longer carry a List - see the
            // Argument overload.
            error = "a scalar value cannot carry a list";
            return false;
        case ValueType::ArchicadElementRef: {
            const JsonValue* guid = source.Find ("elementGuid");
            std::string text;
            if (guid == nullptr || !guid->AsString (text) || text.empty ()) {
                error = "expected an element, as handed to the script - a script cannot make one";
                return false;
            }
            out = Value (ArchicadElementRef { std::move (text) });
            return true;
        }
        case ValueType::Mesh:
            error = "a script cannot produce a mesh; use the geometry nodes and wire the result";
            return false;
        case ValueType::Absent: {
            // An `any` output. Nothing can be checked and nothing should be
            // invented, so it travels as text - honest about having lost the type,
            // and the same thing a Panel would show.
            std::string text;
            if (source.AsString (text)) {
                out = Value (std::move (text));
                return true;
            }
            out = Value (json::Write (source, 0));
            return true;
        }
    }
    error = "unsupported type";
    return false;
}

bool ScriptValueFromJson (const JsonValue& source, ValueType expected, Argument& out, std::string& error)
{
    if (expected != ValueType::List) {
        Value scalar;
        if (!ScriptValueFromJson (source, expected, scalar, error))
            return false;
        out = Argument (std::move (scalar));
        return true;
    }

    const JsonArray* array = source.AsArray ();
    if (array == nullptr) {
        error = "expected a list";
        return false;
    }
    std::vector<Value> items;
    items.reserve (array->size ());
    for (const JsonValue& element : *array) {
        // Numbers, as every list in the catalog carries today. A
        // heterogeneous list would need a per-item type the header has no way
        // to state, so it is refused rather than guessed.
        Value item;
        if (!ScriptValueFromJson (element, ValueType::Double, item, error))
            return false;
        items.push_back (std::move (item));
    }
    out = Argument::FromItems (std::move (items));
    return true;
}

} // namespace evp::nodegraph
