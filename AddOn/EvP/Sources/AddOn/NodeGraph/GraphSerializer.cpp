#include "NodeGraph/GraphSerializer.hpp"

#include "NodeGraph/GraphEdit.hpp"
#include "NodeGraph/Json.hpp"
#include "NodeGraph/NodeRegistry.hpp"

#include <utility>

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

// The parse companion. Derived from the switch above rather than written as a
// second table, so a name added to one cannot be forgotten in the other - which
// would be a port that serialises and then refuses to load.
bool ParseValueTypeName (const std::string& name, ValueType& type)
{
    for (const ValueType candidate : { ValueType::Absent, ValueType::Bool, ValueType::Integer, ValueType::Double,
                                       ValueType::String, ValueType::Point3, ValueType::Polyline, ValueType::Polygon,
                                       ValueType::Mesh, ValueType::ArchicadElementRef, ValueType::List }) {
        if (name == ValueTypeName (candidate)) {
            type = candidate;
            return true;
        }
    }
    return false;
}

JsonValue PointToJson (const Point3& point)
{
    return JsonValue::Array (
        JsonArray { JsonValue::Double (point.x), JsonValue::Double (point.y), JsonValue::Double (point.z) });
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
            object.emplace ("value", JsonValue::String (std::get<ArchicadElementRef> (value.DataValue ()).guid));
            break;
        case ValueType::List:
            // Unreachable: a Value can no longer carry a List - see
            // ArgumentToJson, which handles the branch before an item is ever
            // passed here.
            error = "a scalar value cannot carry a list";
            return false;
        case ValueType::Mesh:
            // See the header: a mesh is a RESULT, and a result inside the
            // program that computes it is a cache masquerading as a parameter.
            error = "a mesh cannot be stored as a graph parameter";
            return false;
    }

    out = JsonValue::Object (std::move (object));
    return true;
}

// The Argument wrapper around a stored parameter: a scalar delegates to
// ValueToJson unchanged, a List-typed one (a selection set) writes its branch
// as an array of scalar-encoded items - never nested, since an item is
// guaranteed a plain Value.
bool ArgumentToJson (const Argument& value, JsonValue& out, std::string& error)
{
    if (value.Type () != ValueType::List)
        return ValueToJson (value.AsValue (), out, error);

    JsonObject object;
    object.emplace ("valueType", JsonValue::String (ValueTypeName (ValueType::List)));
    JsonArray items;
    for (const Value& item : value.Items ()) {
        JsonValue encoded;
        if (!ValueToJson (item, encoded, error))
            return false;
        items.push_back (std::move (encoded));
    }
    object.emplace ("value", JsonValue::Array (std::move (items)));
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
        // A scalar Value can no longer carry a list - see ArgumentFromJson,
        // which is the entry point that handles it.
        error = "a scalar value cannot carry a list";
        return false;
    }
    if (typeName == "mesh") {
        error = "a mesh cannot be stored as a graph parameter";
        return false;
    }
    error = "unknown value type '" + typeName + "'";
    return false;
}

// The Argument wrapper a stored parameter round-trips through: a scalar
// delegates to ValueFromJson unchanged, a List-typed one (a selection set)
// reads its branch as an array of scalar-decoded items. An item is decoded
// through ValueFromJson too, so a nested "list" inside it is refused rather
// than silently building a tree a tree cannot hold (§7.3).
bool ArgumentFromJson (const JsonValue& source, Argument& out, std::string& error)
{
    std::string typeName;
    const JsonValue* typeMember = source.Find ("valueType");
    if (typeMember == nullptr || !typeMember->AsString (typeName)) {
        error = "a value is missing its valueType";
        return false;
    }
    if (typeName != "list") {
        Value scalar;
        if (!ValueFromJson (source, scalar, error))
            return false;
        out = Argument (std::move (scalar));
        return true;
    }

    const JsonValue* member = source.Find ("value");
    const JsonArray* array = member == nullptr ? nullptr : member->AsArray ();
    if (array == nullptr) {
        error = "expected a list";
        return false;
    }
    std::vector<Value> items;
    items.reserve (array->size ());
    for (const JsonValue& element : *array) {
        Value item;
        if (!ValueFromJson (element, item, error))
            return false;
        items.push_back (std::move (item));
    }
    out = Argument::FromItems (std::move (items));
    return true;
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

        // Stage F: the MODE persists and the retained values do not. A saved
        // graph remembers that a node is disabled or is a dam; it does not
        // remember what was in the dam, which is session cache by contract.
        // Written only when it is not the default, so an ordinary graph's file
        // is unchanged by this field existing.
        if (node.executionMode != ExecutionMode::Enabled)
            encoded.emplace ("executionMode", JsonValue::String (ExecutionModeName (node.executionMode)));

        JsonObject parameters;
        for (const auto& [parameterId, value] : node.parameters) {
            JsonValue encodedValue;
            if (!ArgumentToJson (value, encodedValue, result.error)) {
                result.error = "node " + nodeId + ", parameter " + parameterId + ": " + result.error;
                return result;
            }
            parameters.emplace (parameterId, std::move (encodedValue));
        }
        encoded.emplace ("parameters", JsonValue::Object (std::move (parameters)));

        // Omitted entirely when nothing is modified, so a graph that uses no
        // modifiers reads exactly as it did before they existed.
        std::map<std::string, JsonValue> modifiers;
        for (const auto& [portId, modifier] : node.inputModifiers) {
            if (modifier != PortModifier::None)
                modifiers.emplace (portId, JsonValue::String (PortModifierName (modifier)));
        }
        if (!modifiers.empty ())
            encoded.emplace ("inputModifiers", JsonValue::Object (std::move (modifiers)));

        // Instance ports, for the one family that has them - see
        // Node::dynamicInputs.
        //
        // â ï¸ THEY MUST PERSIST, AND NOT BECAUSE THEY ARE EXPENSIVE TO
        // RECOVER. The EDGES in this file are validated against them on load. A
        // graph that re-derived a script node's ports by reading the script would
        // silently drop every wire whenever the file was missing, unreadable or
        // had been edited since the save - which is to say, exactly when the user
        // most needs the graph to open intact and tell them what is wrong. The
        // ports are the document's, and the file is the thing that may have moved.
        for (const auto& [key, ports] :
             { std::pair { "inputs", &node.dynamicInputs }, std::pair { "outputs", &node.dynamicOutputs } }) {
            if (ports->empty ())
                continue;
            JsonArray encodedPorts;
            for (const PortSchema& port : *ports) {
                JsonObject encodedPort;
                encodedPort.emplace ("portId", JsonValue::String (port.id));
                encodedPort.emplace ("label", JsonValue::String (port.label));
                encodedPort.emplace ("valueType", JsonValue::String (ValueTypeName (port.valueType)));
                encodedPort.emplace ("required", JsonValue::Bool (port.required));
                encodedPort.emplace ("acceptsMultiple", JsonValue::Bool (port.acceptsMultiple));
                encodedPorts.push_back (JsonValue::Object (std::move (encodedPort)));
            }
            encoded.emplace (key, JsonValue::Array (std::move (encodedPorts)));
        }
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
        result.error =
            "this graph was written by a newer version of Tapioca (format " + std::to_string (formatVersion) + ")";
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

        // Absent means enabled, which is what every graph written before Stage F
        // says. An unknown spelling is REFUSED rather than defaulted: a file
        // that says "bypassed" in a build that does not know the word must not
        // load as a normally-executing node and silently change what the graph
        // computes.
        if (const JsonValue* mode = encoded.Find ("executionMode"); mode != nullptr) {
            std::string modeName;
            if (!mode->AsString (modeName) || !ParseExecutionMode (modeName, node.executionMode)) {
                result.error = "node " + node.id + " has an unknown executionMode";
                return result;
            }
        }

        if (const JsonValue* modifiers = encoded.Find ("inputModifiers"); modifiers != nullptr) {
            const std::map<std::string, JsonValue>* members = modifiers->AsObject ();
            if (members == nullptr) {
                result.error = "node " + node.id + " has a malformed inputModifiers";
                return result;
            }
            for (const auto& [portId, encodedModifier] : *members) {
                std::string modifierName;
                PortModifier modifier = PortModifier::None;
                // Refused, never defaulted, for the reason executionMode is: a
                // modifier this build does not know changes what the graph
                // COMPUTES, and opening it as "none" would silently produce a
                // different answer from the one that was saved.
                if (!encodedModifier.AsString (modifierName) || !ParsePortModifier (modifierName, modifier)) {
                    result.error = "node " + node.id + " has an unknown modifier on input '" + portId + "'";
                    return result;
                }
                if (modifier != PortModifier::None)
                    node.inputModifiers.emplace (portId, modifier);
            }
        }
        if (const JsonValue* parameters = encoded.Find ("parameters"); parameters != nullptr) {
            const JsonObject* object = parameters->AsObject ();
            if (object == nullptr) {
                result.error = "node " + node.id + " has a malformed parameter set";
                return result;
            }
            for (const auto& [parameterId, value] : *object) {
                Argument decoded;
                std::string error;
                if (!ArgumentFromJson (value, decoded, error)) {
                    result.error = "node " + node.id + ", parameter " + parameterId + ": " + error;
                    return result;
                }
                node.parameters.emplace (parameterId, std::move (decoded));
            }
        }

        for (const auto& [key, ports] :
             { std::pair { "inputs", &node.dynamicInputs }, std::pair { "outputs", &node.dynamicOutputs } }) {
            const JsonValue* member = encoded.Find (key);
            if (member == nullptr)
                continue;
            const JsonArray* array = member->AsArray ();
            if (array == nullptr) {
                result.error = "node " + node.id + " has a malformed " + key + " port list";
                return result;
            }
            for (const JsonValue& encodedPort : *array) {
                PortSchema port;
                std::string valueTypeName;
                const JsonValue* portId = encodedPort.Find ("portId");
                const JsonValue* valueType = encodedPort.Find ("valueType");
                if (portId == nullptr || !portId->AsString (port.id) || port.id.empty ()) {
                    result.error = "node " + node.id + " has a port with no portId";
                    return result;
                }
                // Refused, never defaulted, for the reason executionMode is: a
                // type this build does not know must not load as Absent, which
                // means "any" and would let every edge in the file revalidate
                // against a port that accepts anything.
                if (valueType == nullptr || !valueType->AsString (valueTypeName) ||
                    !ParseValueTypeName (valueTypeName, port.valueType)) {
                    result.error = "node " + node.id + ", port " + port.id + " has an unknown valueType";
                    return result;
                }
                if (const JsonValue* label = encodedPort.Find ("label"); label != nullptr)
                    label->AsString (port.label);
                if (port.label.empty ())
                    port.label = port.id;
                if (const JsonValue* required = encodedPort.Find ("required"); required != nullptr)
                    required->AsBool (port.required);
                if (const JsonValue* multiple = encodedPort.Find ("acceptsMultiple"); multiple != nullptr)
                    multiple->AsBool (port.acceptsMultiple);
                ports->push_back (std::move (port));
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
            const EditResult edit = ApplyEdit (result.graph.document, registry, GraphEdit { ConnectEdit { edge } });
            if (!edit.accepted) {
                result.error = "the edge " + edge.sourceNode + "." + edge.sourcePort + " -> " + edge.targetNode + "." +
                               edge.targetPort + " cannot be loaded: " + edit.error;
                return result;
            }
        }
    }

    result.ok = true;
    return result;
}

} // namespace evp::nodegraph
