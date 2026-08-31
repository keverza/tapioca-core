#include "NodeGraph/GraphSerializer.hpp"

#include "NodeGraph/GraphEdit.hpp"
#include "NodeGraph/Json.hpp"
#include "NodeGraph/NodeRegistry.hpp"

namespace evp::nodegraph {
namespace {

using json::JsonArray;
using json::JsonObject;
using json::JsonValue;

// The value vocabulary as file text. Spelled out rather than derived from the
// enum's order, because an enum reordered during a refactor must not silently
// reinterpret every stored parameter.
const char* ValueTypeName (ValueType type)
{
    switch (type) {
        case ValueType::Absent:
            return "absent";
        case ValueType::Bool:
            return "bool";
        case ValueType::Integer:
            return "integer";
        case ValueType::Double:
            return "double";
        case ValueType::String:
            return "string";
        case ValueType::Point3:
            return "point3";
        case ValueType::Polyline:
            return "polyline";
        case ValueType::Polygon:
            return "polygon";
        case ValueType::Mesh:
            return "mesh";
        case ValueType::ArchicadElementRef:
            return "elementRef";
        case ValueType::List:
            return "list";
    }
    return "absent";
}

JsonValue PointToJson (const Point3& point)
{
    return JsonValue::Array (JsonArray { JsonValue::Double (point.x), JsonValue::Double (point.y),
                                         JsonValue::Double (point.z) });
}

bool PointFromJson (const JsonValue& value, Point3& out, std::string& error)
{
    const JsonArray* array = value.AsArray ();
    if (array == nullptr || array->size () != 3) {
        error = "a point must be an array of three numbers";
        return false;
    }
    double coordinates[3] = { 0.0, 0.0, 0.0 };
    for (size_t i = 0; i < 3; ++i) {
        if (!(*array)[i].AsDouble (coordinates[i])) {
            error = "a point coordinate is not a number";
            return false;
        }
    }
    out = Point3 { coordinates[0], coordinates[1], coordinates[2] };
    return true;
}

JsonValue PointsToJson (const std::vector<Point3>& points)
{
    JsonArray array;
    array.reserve (points.size ());
    for (const Point3& point : points)
        array.push_back (PointToJson (point));
    return JsonValue::Array (std::move (array));
}

bool PointsFromJson (const JsonValue& value, std::vector<Point3>& out, std::string& error)
{
    const JsonArray* array = value.AsArray ();
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

bool ValueToJson (const Value& value, JsonValue& out, std::string& error);

bool ValueToJson (const Value& value, JsonValue& out, std::string& error)
{
    JsonObject object;
    object.emplace ("valueType", JsonValue::String (ValueTypeName (value.Type ())));

    switch (value.Type ()) {
        case ValueType::Absent:
            break;
        case ValueType::Bool:
            object.emplace ("value", JsonValue::Bool (std::get<bool> (value.DataValue ())));
            break;
        case ValueType::Integer:
            object.emplace ("value", JsonValue::Integer (std::get<int64_t> (value.DataValue ())));
            break;
        case ValueType::Double:
            object.emplace ("value", JsonValue::Double (std::get<double> (value.DataValue ())));
            break;
        case ValueType::String:
            object.emplace ("value", JsonValue::String (std::get<std::string> (value.DataValue ())));
            break;
        case ValueType::Point3:
            object.emplace ("value", PointToJson (std::get<Point3> (value.DataValue ())));
            break;
        case ValueType::Polyline:
            object.emplace ("value", PointsToJson (std::get<Polyline> (value.DataValue ()).points));
            break;
        case ValueType::Polygon:
            object.emplace ("value", PointsToJson (std::get<Polygon> (value.DataValue ()).points));
            break;
        case ValueType::ArchicadElementRef:
            object.emplace ("value",
                            JsonValue::String (std::get<ArchicadElementRef> (value.DataValue ()).guid));
            break;
        case ValueType::List: {
            JsonArray items;
            for (const Value& item : std::get<Value::List> (value.DataValue ())) {
                JsonValue encoded;
                if (!ValueToJson (item, encoded, error))
                    return false;
                items.push_back (std::move (encoded));
            }
            object.emplace ("value", JsonValue::Array (std::move (items)));
            break;
        }
        case ValueType::Mesh:
            // See the header: a mesh is a RESULT, and a result inside the
            // program that computes it is a cache masquerading as a parameter.
            error = "a mesh cannot be stored as a graph parameter";
            return false;
    }

    out = JsonValue::Object (std::move (object));
    return true;
}

bool ValueFromJson (const JsonValue& source, Value& out, std::string& error)
{
    std::string typeName;
    const JsonValue* typeMember = source.Find ("valueType");
    if (typeMember == nullptr || !typeMember->AsString (typeName)) {
        error = "a value is missing its valueType";
        return false;
    }
    const JsonValue* member = source.Find ("value");

    if (typeName == "absent") {
        out = Value {};
        return true;
    }
    if (member == nullptr) {
        error = "a value of type '" + typeName + "' carries no value";
        return false;
    }
    if (typeName == "bool") {
        bool flag = false;
        if (!member->AsBool (flag)) {
            error = "expected a boolean";
            return false;
        }
        out = Value (flag);
        return true;
    }
    if (typeName == "integer") {
        int64_t number = 0;
        if (!member->IsIntegral () || !member->AsInteger (number)) {
            error = "expected an integer written without a fraction";
            return false;
        }
        out = Value (number);
        return true;
    }
    if (typeName == "double") {
        double number = 0.0;
        if (!member->AsDouble (number)) {
            error = "expected a number";
            return false;
        }
        out = Value (number);
        return true;
    }
    if (typeName == "string") {
        std::string text;
        if (!member->AsString (text)) {
            error = "expected a string";
            return false;
        }
        out = Value (std::move (text));
        return true;
    }
    if (typeName == "point3") {
        Point3 point {};
        if (!PointFromJson (*member, point, error))
            return false;
        out = Value (point);
        return true;
    }
    if (typeName == "polyline") {
        Polyline polyline;
        if (!PointsFromJson (*member, polyline.points, error))
            return false;
        out = Value (std::move (polyline));
        return true;
    }
    if (typeName == "polygon") {
        Polygon polygon;
        if (!PointsFromJson (*member, polygon.points, error))
            return false;
        out = Value (std::move (polygon));
        return true;
    }
    if (typeName == "elementRef") {
        std::string guid;
        if (!member->AsString (guid)) {
            error = "expected an element guid";
            return false;
        }
        out = Value (ArchicadElementRef { std::move (guid) });
        return true;
    }
    if (typeName == "list") {
        const JsonArray* array = member->AsArray ();
        if (array == nullptr) {
            error = "expected a list";
            return false;
        }
        Value::List items;
        items.reserve (array->size ());
        for (const JsonValue& element : *array) {
            Value item;
            if (!ValueFromJson (element, item, error))
                return false;
            items.push_back (std::move (item));
        }
        out = Value (std::move (items));
        return true;
    }
    if (typeName == "mesh") {
        error = "a mesh cannot be stored as a graph parameter";
        return false;
    }
    error = "unknown value type '" + typeName + "'";
    return false;
}

JsonValue LayoutToJson (const GraphMetadata& metadata)
{
    JsonObject byNode;
    for (const auto& [nodeId, fields] : metadata.nodeLayout) {
        JsonObject encoded;
        for (const auto& [key, value] : fields)
            encoded.emplace (key, JsonValue::String (value));
        byNode.emplace (nodeId, JsonValue::Object (std::move (encoded)));
    }
    return JsonValue::Object (std::move (byNode));
}

void LayoutFromJson (const JsonValue* source, GraphMetadata& metadata)
{
    if (source == nullptr)
        return;
    const JsonObject* byNode = source->AsObject ();
    if (byNode == nullptr)
        return;
    for (const auto& [nodeId, fields] : *byNode) {
        const JsonObject* encoded = fields.AsObject ();
        if (encoded == nullptr)
            continue;
        std::map<std::string, std::string> entry;
        for (const auto& [key, value] : *encoded) {
            std::string text;
            // Layout is round-tripped, not interpreted: a member the runtime
            // cannot read as text is dropped rather than made an error, because
            // an editor's own field must never be able to make a graph
            // unloadable.
            if (value.AsString (text))
                entry.emplace (key, std::move (text));
        }
        metadata.nodeLayout.emplace (nodeId, std::move (entry));
    }
}

} // namespace

SerializeResult SerializeGraph (const GraphDocument& document, const GraphMetadata& metadata, size_t indent)
{
    SerializeResult result;

    JsonArray nodes;
    for (const auto& [nodeId, node] : document.Nodes ()) {
        JsonObject encoded;
        encoded.emplace ("id", JsonValue::String (node.id));
        // "nodeType", never "type" - the repository's converter reserves the
        // latter, and one spelling across the wire and the file is one less
        // thing to get wrong.
        encoded.emplace ("nodeType", JsonValue::String (node.nodeType));

        JsonObject parameters;
        for (const auto& [parameterId, value] : node.parameters) {
            JsonValue encodedValue;
            if (!ValueToJson (value, encodedValue, result.error)) {
                result.error = "node " + nodeId + ", parameter " + parameterId + ": " + result.error;
                return result;
            }
            parameters.emplace (parameterId, std::move (encodedValue));
        }
        encoded.emplace ("parameters", JsonValue::Object (std::move (parameters)));
        nodes.push_back (JsonValue::Object (std::move (encoded)));
    }

    JsonArray edges;
    for (const Edge& edge : document.Edges ()) {
        JsonObject encoded;
        encoded.emplace ("sourceNode", JsonValue::String (edge.sourceNode));
        encoded.emplace ("sourcePort", JsonValue::String (edge.sourcePort));
        encoded.emplace ("targetNode", JsonValue::String (edge.targetNode));
        encoded.emplace ("targetPort", JsonValue::String (edge.targetPort));
        edges.push_back (JsonValue::Object (std::move (encoded)));
    }

    JsonObject metadataObject;
    metadataObject.emplace ("label", JsonValue::String (metadata.label));
    metadataObject.emplace ("description", JsonValue::String (metadata.description));
    metadataObject.emplace ("nodeLayout", LayoutToJson (metadata));

    JsonObject root;
    root.emplace ("format", JsonValue::String (kGraphFormatName));
    root.emplace ("formatVersion", JsonValue::Integer (static_cast<int64_t> (kGraphFormatVersion)));
    root.emplace ("metadata", JsonValue::Object (std::move (metadataObject)));
    root.emplace ("nodes", JsonValue::Array (std::move (nodes)));
    root.emplace ("edges", JsonValue::Array (std::move (edges)));

    result.text = json::Write (JsonValue::Object (std::move (root)), indent);
    result.ok = true;
    return result;
}

DeserializeResult DeserializeGraph (const std::string& text, const NodeRegistry& registry)
{
    DeserializeResult result;

    const json::ParseResult parsed = json::Parse (text);
    if (!parsed.ok) {
        result.error = "the file is not valid JSON: " + parsed.error;
        return result;
    }

    const JsonValue& root = parsed.value;
    std::string format;
    const JsonValue* formatMember = root.Find ("format");
    if (formatMember == nullptr || !formatMember->AsString (format) || format != kGraphFormatName) {
        result.error = "this is not a Tapioca node graph";
        return result;
    }

    int64_t formatVersion = 0;
    const JsonValue* versionMember = root.Find ("formatVersion");
    if (versionMember == nullptr || !versionMember->AsInteger (formatVersion)) {
        result.error = "the file carries no format version";
        return result;
    }
    if (formatVersion > static_cast<int64_t> (kGraphFormatVersion)) {
        // Refused rather than attempted. A newer file may mean something this
        // build cannot know, and §35's rule is that an old graph is never
        // silently reinterpreted under new semantics - which cuts both ways.
        result.error = "this graph was written by a newer version of Tapioca (format " +
                       std::to_string (formatVersion) + ")";
        return result;
    }
    result.graph.formatVersion = static_cast<uint32_t> (formatVersion);

    if (const JsonValue* metadata = root.Find ("metadata"); metadata != nullptr) {
        std::string text_;
        if (const JsonValue* label = metadata->Find ("label"); label != nullptr && label->AsString (text_))
            result.graph.metadata.label = text_;
        if (const JsonValue* description = metadata->Find ("description");
            description != nullptr && description->AsString (text_))
            result.graph.metadata.description = text_;
        LayoutFromJson (metadata->Find ("nodeLayout"), result.graph.metadata);
    }

    const JsonValue* nodesMember = root.Find ("nodes");
    const JsonArray* nodes = nodesMember == nullptr ? nullptr : nodesMember->AsArray ();
    if (nodes == nullptr) {
        result.error = "the graph has no node list";
        return result;
    }

    for (const JsonValue& encoded : *nodes) {
        Node node;
        const JsonValue* idMember = encoded.Find ("id");
        const JsonValue* typeMember = encoded.Find ("nodeType");
        if (idMember == nullptr || !idMember->AsString (node.id) || node.id.empty ()) {
            result.error = "a node has no id";
            return result;
        }
        if (typeMember == nullptr || !typeMember->AsString (node.nodeType)) {
            result.error = "node " + node.id + " has no nodeType";
            return result;
        }

        if (const JsonValue* parameters = encoded.Find ("parameters"); parameters != nullptr) {
            const JsonObject* object = parameters->AsObject ();
            if (object == nullptr) {
                result.error = "node " + node.id + " has a malformed parameter set";
                return result;
            }
            for (const auto& [parameterId, value] : *object) {
                Value decoded;
                std::string error;
                if (!ValueFromJson (value, decoded, error)) {
                    result.error = "node " + node.id + ", parameter " + parameterId + ": " + error;
                    return result;
                }
                node.parameters.emplace (parameterId, std::move (decoded));
            }
        }

        // Through the validated edit, not into the map: this is where an
        // unknown node type or an out-of-schema parameter is caught, on the one
        // code path that catches it for an interactive edit too.
        const NodeId nodeId = node.id;
        const EditResult edit = ApplyEdit (result.graph.document, registry, GraphEdit { AddNodeEdit { node } });
        if (!edit.accepted) {
            result.error = "node " + nodeId + " cannot be loaded: " + edit.error;
            return result;
        }
    }

    const JsonValue* edgesMember = root.Find ("edges");
    if (edgesMember != nullptr) {
        const JsonArray* edges = edgesMember->AsArray ();
        if (edges == nullptr) {
            result.error = "the graph has a malformed edge list";
            return result;
        }
        for (const JsonValue& encoded : *edges) {
            Edge edge;
            const JsonValue* sourceNode = encoded.Find ("sourceNode");
            const JsonValue* sourcePort = encoded.Find ("sourcePort");
            const JsonValue* targetNode = encoded.Find ("targetNode");
            const JsonValue* targetPort = encoded.Find ("targetPort");
            if (sourceNode == nullptr || !sourceNode->AsString (edge.sourceNode) || sourcePort == nullptr ||
                !sourcePort->AsString (edge.sourcePort) || targetNode == nullptr ||
                !targetNode->AsString (edge.targetNode) || targetPort == nullptr ||
                !targetPort->AsString (edge.targetPort)) {
                result.error = "an edge is missing an endpoint";
                return result;
            }
            const EditResult edit =
                ApplyEdit (result.graph.document, registry, GraphEdit { ConnectEdit { edge } });
            if (!edit.accepted) {
                result.error = "the edge " + edge.sourceNode + "." + edge.sourcePort + " -> " + edge.targetNode +
                               "." + edge.targetPort + " cannot be loaded: " + edit.error;
                return result;
            }
        }
    }

    result.ok = true;
    return result;
}

} // namespace evp::nodegraph
