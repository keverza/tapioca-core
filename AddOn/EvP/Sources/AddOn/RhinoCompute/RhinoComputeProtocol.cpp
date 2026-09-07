#include "RhinoCompute/RhinoComputeProtocol.hpp"

#include "NodeGraph/Json.hpp"

#include <cstdio>
// std::locale::classic, for the number parses below. MSVC happens to pull this
// in through <sstream>; relying on that is how a locale-correct parse silently
// stops compiling on a toolchain that does not.
#include <locale>
#include <sstream>

namespace evp {
namespace rhinocompute {

namespace json = evp::nodegraph::json;

namespace {

// Compute's own key spellings. They are PascalCase on the way in and lowercase
// on the way out, which is not a mistake in this file: Resthopper's request
// schema and its response schema genuinely disagree, and pretending otherwise
// produced a request compute accepted and ignored.
constexpr const char* kParamName = "ParamName";
constexpr const char* kInnerTree = "InnerTree";
constexpr const char* kBranchZero = "{0}";

void AddError (std::vector<Error>& errors, ErrorSource source, const char* code, const std::string& message)
{
    Error e;
    e.source = source;
    e.code = code;
    e.message = message;
    errors.push_back (e);
}

// A number the way JSON wants it, with enough digits to round-trip a double and
// no locale in sight. std::to_string honours the C locale and would write a
// comma on a machine set to one, which compute rejects.
std::string NumberLiteral (double value)
{
    char buffer[40];
    std::snprintf (buffer, sizeof (buffer), "%.17g", value);
    return std::string (buffer);
}

// One InnerTree branch holding one item, which is every input Tapioca sends.
// AtMost is 1 on every input the panel builds, so a list would be a value the
// panel could not have produced.
json::JsonValue LeafTree (const std::string& typeName, const std::string& data)
{
    json::JsonObject item;
    item["type"] = json::JsonValue::String (typeName);
    item["data"] = json::JsonValue::String (data);

    json::JsonArray branch;
    branch.push_back (json::JsonValue::Object (item));

    json::JsonObject tree;
    tree[kBranchZero] = json::JsonValue::Array (branch);
    return json::JsonValue::Object (tree);
}

// The CLR type name compute expects for a Tapioca schema type, and the encoding
// of the value alongside it.
//
// ⚠️ THE VALUE IS ALWAYS A STRING IN `data`, EVEN FOR A NUMBER. That is
// Resthopper's shape, not ours: `data` is a JSON-encoded value carried inside a
// JSON string. A bare number there is accepted and silently ignored.
bool EncodeValue (const InputValue& input, std::string& typeName, std::string& data)
{
    if (input.type == "number") {
        double parsed = 0.0;
        std::istringstream stream (input.value);
        stream.imbue (std::locale::classic ());
        if (!(stream >> parsed))
            return false;
        typeName = "System.Double";
        data = NumberLiteral (parsed);
        return true;
    }

    if (input.type == "integer") {
        long long parsed = 0;
        std::istringstream stream (input.value);
        stream.imbue (std::locale::classic ());
        if (!(stream >> parsed))
            return false;
        typeName = "System.Int32";
        data = std::to_string (parsed);
        return true;
    }

    if (input.type == "boolean") {
        const bool truthy = input.value == "true" || input.value == "True" || input.value == "1";
        const bool falsy = input.value == "false" || input.value == "False" || input.value == "0";
        if (!truthy && !falsy)
            return false;
        typeName = "System.Boolean";
        data = truthy ? "true" : "false";
        return true;
    }

    if (input.type == "string" || input.type == "enum") {
        typeName = "System.String";
        // A string value is itself JSON-encoded inside `data`, so it carries its
        // own quotes and escapes.
        std::string escaped;
        escaped.reserve (input.value.size () + 2);
        escaped.push_back ('"');
        for (char c : input.value) {
            switch (c) {
                case '"':
                    escaped += "\\\"";
                    break;
                case '\\':
                    escaped += "\\\\";
                    break;
                case '\n':
                    escaped += "\\n";
                    break;
                case '\r':
                    escaped += "\\r";
                    break;
                case '\t':
                    escaped += "\\t";
                    break;
                default:
                    if (static_cast<unsigned char> (c) < 0x20) {
                        char esc[8];
                        std::snprintf (esc, sizeof (esc), "\\u%04x", static_cast<unsigned char> (c));
                        escaped += esc;
                    }
                    else {
                        escaped.push_back (c);
                    }
                    break;
            }
        }
        escaped.push_back ('"');
        data = escaped;
        return true;
    }

    return false;
}

bool ReadString (const json::JsonValue& object, const char* key, std::string& out)
{
    const json::JsonValue* found = object.Find (key);
    return found != nullptr && found->AsString (out);
}

bool ReadDouble (const json::JsonValue& object, const char* key, double& out)
{
    const json::JsonValue* found = object.Find (key);
    if (found == nullptr)
        return false;

    if (found->AsDouble (out))
        return true;

    int64_t integral = 0;
    if (found->AsInteger (integral)) {
        out = static_cast<double> (integral);
        return true;
    }

    return false;
}

void ReadErrorArray (const json::JsonValue& parent, const char* key, ErrorSource source, const char* code,
                     std::vector<Error>& out)
{
    const json::JsonValue* array = parent.Find (key);
    if (array == nullptr)
        return;

    const json::JsonArray* items = array->AsArray ();
    if (items == nullptr)
        return;

    for (const json::JsonValue& item : *items) {
        std::string message;
        if (item.AsString (message) && !message.empty ())
            AddError (out, source, code, message);
    }
}

// Reads a float triple stream out of a JSON number array.
bool ReadFloatArray (const json::JsonValue& parent, const char* key, std::vector<float>& out)
{
    const json::JsonValue* array = parent.Find (key);
    if (array == nullptr)
        return true; // Absent is legal; empty is the result.

    const json::JsonArray* items = array->AsArray ();
    if (items == nullptr)
        return false;

    out.reserve (items->size ());
    for (const json::JsonValue& item : *items) {
        double value = 0.0;
        int64_t integral = 0;
        if (item.AsDouble (value))
            out.push_back (static_cast<float> (value));
        else if (item.AsInteger (integral))
            out.push_back (static_cast<float> (integral));
        else
            return false;
    }

    return true;
}

bool ReadIndexArray (const json::JsonValue& parent, const char* key, std::vector<uint32_t>& out)
{
    const json::JsonValue* array = parent.Find (key);
    if (array == nullptr)
        return true;

    const json::JsonArray* items = array->AsArray ();
    if (items == nullptr)
        return false;

    out.reserve (items->size ());
    for (const json::JsonValue& item : *items) {
        int64_t value = 0;
        if (!item.AsInteger (value) || value < 0)
            return false;
        out.push_back (static_cast<uint32_t> (value));
    }

    return true;
}

} // namespace

std::string SchemaTypeForParamType (const std::string& paramType)
{
    if (paramType == "Number")
        return "number";
    if (paramType == "Integer")
        return "integer";
    if (paramType == "Boolean" || paramType == "Bool")
        return "boolean";
    if (paramType == "Text" || paramType == "String")
        return "string";

    // "Generic Data", "Point", "Curve", "Geometry" and everything else are
    // deliberately NOT mapped. An unsupported input silently treated as text is
    // a control the user can type into that can never produce a valid solve.
    return std::string ();
}

bool IsNoServerResponse (const std::string& body)
{
    return body.find ("No compute server found") != std::string::npos;
}

std::string BuildIoRequest (const std::string& definitionBase64)
{
    json::JsonObject root;
    root["algo"] = json::JsonValue::String (definitionBase64);
    root["pointer"] = json::JsonValue ();
    root["values"] = json::JsonValue::Array (json::JsonArray ());
    return json::Write (json::JsonValue::Object (root), 0);
}

std::string BuildSolveRequest (const SolveRequest& request)
{
    json::JsonObject root;

    // Pointer wins. Sending both is not an error compute reports; it just uses
    // one, and which one is not worth depending on.
    if (!request.pointer.empty ()) {
        root["pointer"] = json::JsonValue::String (request.pointer);
        root["algo"] = json::JsonValue::String (std::string ());
    }
    else {
        root["algo"] = json::JsonValue::String (request.definitionBase64);
        root["pointer"] = json::JsonValue ();
    }

    json::JsonArray values;
    for (const InputValue& input : request.inputs) {
        std::string typeName;
        std::string data;
        if (!EncodeValue (input, typeName, data)) {
            // An input the panel could not encode is dropped rather than sent as
            // a guess. ParseSolveResponse will report the definition's own
            // complaint about the missing value, which names the input.
            continue;
        }

        json::JsonObject value;
        value[kParamName] = json::JsonValue::String (input.id);
        value[kInnerTree] = LeafTree (typeName, data);
        values.push_back (json::JsonValue::Object (value));
    }

    root["values"] = json::JsonValue::Array (values);
    return json::Write (json::JsonValue::Object (root), 0);
}

bool ParseIoResponse (const std::string& body, WorkflowSchema& schema)
{
    json::ParseResult parsed = json::Parse (body);
    if (!parsed.ok) {
        AddError (schema.errors, ErrorSource::Tapioca, "COMPUTE_BAD_JSON",
                  "compute's /io answer was not JSON: " + parsed.error);
        return false;
    }

    const json::JsonValue& root = parsed.value;
    if (root.AsObject () == nullptr) {
        AddError (schema.errors, ErrorSource::Tapioca, "COMPUTE_BAD_JSON",
                  "compute's /io answer was not a JSON object.");
        return false;
    }

    ReadString (root, "CacheKey", schema.pointer);
    ReadString (root, "Description", schema.name);

    // Compute reports these when the DEFINITION is wrong — a duplicate input
    // name being the one that actually happens. They are carried, not swallowed:
    // the measured failure is that compute then solves with empty trees and
    // nothing else says why.
    ReadErrorArray (root, "Errors", ErrorSource::Grasshopper, "GH_DEFINITION_INVALID", schema.errors);

    const json::JsonValue* inputs = root.Find ("Inputs");
    if (inputs == nullptr)
        return true; // A definition with no public inputs is legal.

    const json::JsonArray* items = inputs->AsArray ();
    if (items == nullptr) {
        AddError (schema.errors, ErrorSource::Tapioca, "COMPUTE_BAD_JSON",
                  "compute reported Inputs that were not an array.");
        return false;
    }

    int fallbackOrder = 0;
    for (const json::JsonValue& item : *items) {
        if (item.AsObject () == nullptr)
            continue;

        InputDescriptor descriptor;
        if (!ReadString (item, "Name", descriptor.id) || descriptor.id.empty ()) {
            AddError (schema.errors, ErrorSource::Grasshopper, "GH_INPUT_UNNAMED",
                      "An input has no name, so Archicad cannot bind a control to it.");
            continue;
        }

        // Nickname is the author's label when they set one; the name is the
        // identity and a poor caption.
        if (!ReadString (item, "Nickname", descriptor.label) || descriptor.label.empty ())
            descriptor.label = descriptor.id;

        std::string paramType;
        ReadString (item, "ParamType", paramType);
        descriptor.type = SchemaTypeForParamType (paramType);
        if (descriptor.type.empty ()) {
            AddError (schema.errors, ErrorSource::Grasshopper, "GH_INPUT_UNSUPPORTED",
                      "The input '" + descriptor.id + "' has type '" + paramType +
                          "', which Tapioca cannot build a control for yet.");
            continue;
        }

        double bound = 0.0;
        if (ReadDouble (item, "Minimum", bound)) {
            descriptor.hasMinimum = true;
            descriptor.minimum = bound;
        }

        if (ReadDouble (item, "Maximum", bound)) {
            descriptor.hasMaximum = true;
            descriptor.maximum = bound;
        }

        // AtLeast comes straight off IGH_ContextualParameter: 0 means the
        // definition solves without a value.
        double atLeast = 1.0;
        if (ReadDouble (item, "AtLeast", atLeast))
            descriptor.required = atLeast >= 1.0;

        descriptor.order = fallbackOrder++;
        schema.inputs.push_back (descriptor);
    }

    // Two inputs sharing an id is compute's own hard error, but it reports it as
    // prose in Errors and still answers 200. Restating it as a refusal here is
    // what keeps a caller from solving anyway.
    for (size_t i = 0; i < schema.inputs.size (); ++i) {
        for (size_t j = i + 1; j < schema.inputs.size (); ++j) {
            if (schema.inputs[i].id == schema.inputs[j].id) {
                AddError (schema.errors, ErrorSource::Grasshopper, "GH_INPUT_DUPLICATE",
                          "Two inputs share the id '" + schema.inputs[i].id +
                              "'. compute keys injection by name and will solve with no data.");
            }
        }
    }

    return true;
}

bool ParseSolveResponse (const std::string& body, SolveResult& result)
{
    json::ParseResult parsed = json::Parse (body);
    if (!parsed.ok) {
        AddError (result.errors, ErrorSource::Tapioca, "COMPUTE_BAD_JSON",
                  "compute's solve answer was not JSON: " + parsed.error);
        return false;
    }

    const json::JsonValue& root = parsed.value;
    if (root.AsObject () == nullptr) {
        AddError (result.errors, ErrorSource::Tapioca, "COMPUTE_BAD_JSON",
                  "compute's solve answer was not a JSON object.");
        return false;
    }

    ReadString (root, "pointer", result.pointer);
    ReadString (root, "modelunits", result.modelUnits);
    ReadErrorArray (root, "errors", ErrorSource::Grasshopper, "GH_SOLVE_FAILED", result.errors);

    // ⚠️ WARNINGS ARE NOT ERRORS AND MUST NOT BE DROPPED. A Tapioca input that
    // something else drives reports itself this way: the solve SUCCEEDS, the
    // result is computed from the wired value rather than the injected one, and
    // the only trace is here. Read into `warnings` so the panel can show it
    // without failing the run.
    const json::JsonValue* warnings = root.Find ("warnings");
    if (warnings != nullptr) {
        const json::JsonArray* items = warnings->AsArray ();
        if (items != nullptr) {
            for (const json::JsonValue& item : *items) {
                std::string message;
                if (item.AsString (message) && !message.empty ())
                    result.warnings.push_back (message);
            }
        }
    }

    const json::JsonValue* values = root.Find ("values");
    if (values != nullptr) {
        const json::JsonArray* items = values->AsArray ();
        if (items == nullptr) {
            AddError (result.errors, ErrorSource::Tapioca, "COMPUTE_BAD_JSON",
                      "compute reported values that were not an array.");
            return false;
        }

        for (const json::JsonValue& item : *items) {
            const json::JsonValue* tree = item.Find (kInnerTree);
            if (tree == nullptr)
                continue;

            const json::JsonObject* branches = tree->AsObject ();
            if (branches == nullptr)
                continue;

            for (const auto& branch : *branches) {
                const json::JsonArray* leaves = branch.second.AsArray ();
                if (leaves == nullptr)
                    continue;

                for (const json::JsonValue& leaf : *leaves) {
                    // The preview component states its mesh as explicit arrays
                    // carried in `data` as JSON text — never as a serialized
                    // RhinoCommon object. See PreviewMesh in the header.
                    std::string data;
                    if (!ReadString (leaf, "data", data) || data.empty ())
                        continue;

                    json::ParseResult inner = json::Parse (data);
                    if (!inner.ok || inner.value.AsObject () == nullptr)
                        continue;

                    const json::JsonValue* marker = inner.value.Find ("tapiocaPreview");
                    if (marker == nullptr)
                        continue;

                    PreviewMesh mesh;
                    double meshId = 0.0;
                    if (ReadDouble (inner.value, "id", meshId) && meshId >= 0.0)
                        mesh.id = static_cast<uint64_t> (meshId);

                    if (!ReadFloatArray (inner.value, "vertices", mesh.vertices) ||
                        !ReadFloatArray (inner.value, "normals", mesh.normals) ||
                        !ReadIndexArray (inner.value, "indices", mesh.indices)) {
                        AddError (result.errors, ErrorSource::Tapioca, "PREVIEW_MALFORMED",
                                  "A preview mesh carried a value that was not a number.");
                        continue;
                    }

                    // ⚠️ VALIDATE BEFORE ANYTHING INDEXES THIS. These bytes came
                    // from another process running third-party components and
                    // are about to become GPU buffers inside Archicad.exe.
                    if (mesh.vertices.size () % 3 != 0) {
                        AddError (result.errors, ErrorSource::Tapioca, "PREVIEW_MALFORMED",
                                  "A preview mesh had a vertex count that is not a multiple of three.");
                        continue;
                    }

                    if (!mesh.normals.empty () && mesh.normals.size () != mesh.vertices.size ()) {
                        AddError (result.errors, ErrorSource::Tapioca, "PREVIEW_MALFORMED",
                                  "A preview mesh had normals that do not match its vertices.");
                        continue;
                    }

                    if (mesh.indices.size () % 3 != 0) {
                        AddError (result.errors, ErrorSource::Tapioca, "PREVIEW_MALFORMED",
                                  "A preview mesh had an index count that is not a multiple of three.");
                        continue;
                    }

                    const uint32_t vertexCount = static_cast<uint32_t> (mesh.vertices.size () / 3);
                    bool inRange = true;
                    for (uint32_t index : mesh.indices) {
                        if (index >= vertexCount) {
                            inRange = false;
                            break;
                        }
                    }

                    if (!inRange) {
                        AddError (result.errors, ErrorSource::Tapioca, "PREVIEW_OUT_OF_RANGE",
                                  "A preview mesh indexed past its own vertices.");
                        continue;
                    }

                    result.meshes.push_back (mesh);
                }
            }
        }
    }

    result.success = result.errors.empty ();
    return true;
}

std::string ToBase64 (const std::vector<uint8_t>& bytes)
{
    static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve (((bytes.size () + 2) / 3) * 4);

    size_t i = 0;
    while (i + 2 < bytes.size ()) {
        const uint32_t triple = (static_cast<uint32_t> (bytes[i]) << 16) | (static_cast<uint32_t> (bytes[i + 1]) << 8) |
                                static_cast<uint32_t> (bytes[i + 2]);
        out.push_back (kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back (kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back (kAlphabet[(triple >> 6) & 0x3F]);
        out.push_back (kAlphabet[triple & 0x3F]);
        i += 3;
    }

    const size_t remaining = bytes.size () - i;
    if (remaining == 1) {
        const uint32_t triple = static_cast<uint32_t> (bytes[i]) << 16;
        out.push_back (kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back (kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back ('=');
        out.push_back ('=');
    }
    else if (remaining == 2) {
        const uint32_t triple = (static_cast<uint32_t> (bytes[i]) << 16) | (static_cast<uint32_t> (bytes[i + 1]) << 8);
        out.push_back (kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back (kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back (kAlphabet[(triple >> 6) & 0x3F]);
        out.push_back ('=');
    }

    return out;
}

} // namespace rhinocompute
} // namespace evp
