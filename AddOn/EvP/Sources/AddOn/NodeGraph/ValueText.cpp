#include "NodeGraph/ValueText.hpp"

#include "Geometry/Mesh.hpp"

#include <cstdio>

namespace evp::nodegraph {
namespace {

// %.10g rather than std::to_string: to_string always prints six decimals, so
// every coordinate in a panel would read "1.000000" and a reader would have to
// squint past the noise to see the number.
std::string Number (double value)
{
    char text[64];
    std::snprintf (text, sizeof text, "%.10g", value);
    return text;
}

std::string Point (const Point3& point)
{
    return "(" + Number (point.x) + ", " + Number (point.y) + ", " + Number (point.z) + ")";
}

std::string Format (const Value& value, size_t depth, size_t maxDepth, size_t maxItems)
{
    switch (value.Type ()) {
        case ValueType::Absent:
            return "(none)";
        case ValueType::Bool:
            return std::get<bool> (value.DataValue ()) ? "true" : "false";
        case ValueType::Integer:
            return std::to_string (std::get<int64_t> (value.DataValue ()));
        case ValueType::Double:
            return Number (std::get<double> (value.DataValue ()));
        case ValueType::String:
            return std::get<std::string> (value.DataValue ());
        case ValueType::Point3:
            return Point (std::get<Point3> (value.DataValue ()));
        case ValueType::Polyline: {
            const Polyline& line = std::get<Polyline> (value.DataValue ());
            return "Polyline (" + std::to_string (line.points.size ()) + " points)";
        }
        case ValueType::Polygon: {
            const Polygon& polygon = std::get<Polygon> (value.DataValue ());
            return "Polygon (" + std::to_string (polygon.points.size ()) + " points)";
        }
        case ValueType::Mesh: {
            const auto& mesh = std::get<Value::ImmutableMesh> (value.DataValue ());
            if (!mesh)
                return "Mesh (empty)";
            return "Mesh (" + std::to_string (mesh->vertices.size () / 3) + " vertices, " +
                   std::to_string (mesh->triangles.size () / 3) + " triangles)";
        }
        case ValueType::ArchicadElementRef:
            return std::get<ArchicadElementRef> (value.DataValue ()).guid;
        case ValueType::List:
            // Unreachable: a Value can no longer carry a List - see
            // FormatArgument, which renders the branch before an item is ever
            // passed here.
            return "(unknown)";
    }
    return "(unknown)";
}

// The branch a List-typed port hands the body, or a scalar argument - the only
// place depth still means anything, since an item inside a branch is
// guaranteed scalar.
std::string FormatArgument (const Argument& value, size_t depth, size_t maxDepth, size_t maxItems)
{
    if (value.Type () != ValueType::List)
        return Format (value.AsValue (), depth, maxDepth, maxItems);

    const std::vector<Value>& list = value.Items ();
    if (depth >= maxDepth)
        return "[" + std::to_string (list.size ()) + " items]";
    std::string text = "[";
    for (size_t i = 0; i < list.size (); ++i) {
        if (i >= maxItems) {
            text += ", +" + std::to_string (list.size () - i) + " more";
            break;
        }
        if (i != 0)
            text += ", ";
        text += Format (list[i], depth + 1, maxDepth, maxItems);
    }
    return text + "]";
}

const char* TypeName (ValueType valueType)
{
    switch (valueType) {
        case ValueType::Absent:
            return "Nothing";
        case ValueType::Bool:
            return "Boolean";
        case ValueType::Integer:
            return "Integer";
        case ValueType::Double:
            return "Number";
        case ValueType::String:
            return "Text";
        case ValueType::Point3:
            return "Point";
        case ValueType::Polyline:
            return "Polyline";
        case ValueType::Polygon:
            return "Polygon";
        case ValueType::Mesh:
            return "Mesh";
        case ValueType::ArchicadElementRef:
            return "Element";
        case ValueType::List:
            return "List";
    }
    return "Value";
}

} // namespace

std::string FormatValue (const Argument& value, size_t maxDepth, size_t maxItems)
{
    return FormatArgument (value, 0, maxDepth, maxItems);
}

std::vector<std::string> FormatValueLines (const Argument& value, size_t maxLines)
{
    std::vector<std::string> lines;

    if (value.Type () != ValueType::List) {
        lines.push_back (FormatValue (value));
        return lines;
    }

    const std::vector<Value>& list = value.Items ();
    if (list.empty ()) {
        lines.push_back ("(empty list)");
        return lines;
    }

    for (size_t i = 0; i < list.size (); ++i) {
        if (maxLines != 0 && lines.size () + 1 >= maxLines) {
            // Say so. A quietly shortened list reads as a wrong answer.
            lines.push_back ("... " + std::to_string (list.size () - i) + " more of " + std::to_string (list.size ()));
            break;
        }
        lines.push_back (FormatValue (list[i]));
    }
    return lines;
}

std::string DescribeValue (const Argument& value)
{
    if (value.Type () != ValueType::List)
        return TypeName (value.Type ());
    return "List of " + std::to_string (value.Items ().size ());
}

} // namespace evp::nodegraph
