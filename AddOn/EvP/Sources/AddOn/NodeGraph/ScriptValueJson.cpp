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

/**
 * What a value IS, when the header did not say.
 *
 * ⚠️ THIS READS THE SHAPE, AND THE VERSION IT REPLACED READ NOTHING. An untyped
 * output used to travel as TEXT - honest about having lost the type, and useless:
 * `out = 2.5` arrived downstream as the string "2.5", so the first thing anyone
 * did after omitting a type was put it back. Since omitting it is now the
 * ordinary way to write a script node, inference has to produce the value the
 * script actually computed.
 *
 * ⚠️ AND IT NEVER GUESSES BETWEEN TWO ANSWERS. An array of points is a LIST of
 * points, not a polyline and not a polygon: those differ by whether the last
 * point joins the first, which is a fact about intent that no shape carries. A
 * script that means a polyline says `@out edge : polyline`, and that is exactly
 * the case declaring a type is for.
 *
 * A mesh and an element are refused here for the same reasons they are refused
 * when declared - a script may read them and must not be able to invent one.
 */
static bool InferScriptValue (const JsonValue& source, Value& out, std::string& error)
{
    bool flag = false;
    if (source.AsBool (flag)) {
        out = Value (flag);
        return true;
    }

    std::string text;
    if (source.AsString (text)) {
        out = Value (std::move (text));
        return true;
    }

    // Integral first, and the distinction is KEPT rather than widened: an index,
    // a count and a storey number are integers, and handing them downstream as
    // doubles is how `2` becomes `2.0` in a label.
    int64_t whole = 0;
    if (source.IsIntegral () && source.AsInteger (whole)) {
        out = Value (whole);
        return true;
    }
    double number = 0.0;
    if (source.AsDouble (number)) {
        out = Value (number);
        return true;
    }

    if (source.AsArray () != nullptr) {
        // A scalar Value cannot carry a list; the Argument overload handles the
        // collection case before ever reaching here. Saying so beats producing
        // something that looks like a value and is not.
        error = "a list output needs the Argument form";
        return false;
    }

    if (source.Find ("elementGuid") != nullptr)
        return ScriptValueFromJson (source, ValueType::ArchicadElementRef, out, error);
    if (source.Find ("isMesh") != nullptr) {
        error = "a script cannot produce a mesh; use the geometry nodes and wire the result";
        return false;
    }
    if (source.Find ("x") != nullptr && source.Find ("y") != nullptr)
        return ScriptValueFromJson (source, ValueType::Point3, out, error);

    error = "the script produced something with no graph type; return a number, text, a "
            "point or a list, or declare the port's type in the header";
    return false;
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
        case ValueType::Absent:
            // An INFERRED output: `@out result`, with no type written down. See
            // InferScriptValue for why the shape is read rather than the value
            // being flattened to text, which is what this used to do.
            return InferScriptValue (source, out, error);
    }
    error = "unsupported type";
    return false;
}

bool ScriptValueFromJson (const JsonValue& source, ValueType expected, Argument& out, std::string& error)
{
    // ⚠️ AN INFERRED PORT DECIDES BETWEEN SCALAR AND LIST FROM THE VALUE, which a
    // DECLARED port never has to: `@out result` is one port whether the script
    // sets it to a number or to a hundred of them, and the header has said
    // nothing either way. A typed port keeps the old behaviour exactly - `list`
    // demands an array, `number` refuses one - because that is what declaring the
    // type is for.
    const bool inferredList = expected == ValueType::Absent && source.AsArray () != nullptr;

    if (expected != ValueType::List && !inferredList) {
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
        // A DECLARED list carries numbers, as every list in the catalog does
        // today: a heterogeneous list would need a per-item type the header has
        // no way to state, so it is refused rather than guessed. An INFERRED one
        // reads each item's own shape, because there was no header to state it
        // with - which is what lets `@out points` carry the points the script
        // built without the author first learning the type vocabulary.
        Value item;
        if (!ScriptValueFromJson (element, inferredList ? ValueType::Absent : ValueType::Double, item, error))
            return false;
        items.push_back (std::move (item));
    }
    out = Argument::FromItems (std::move (items));
    return true;
}

} // namespace evp::nodegraph
