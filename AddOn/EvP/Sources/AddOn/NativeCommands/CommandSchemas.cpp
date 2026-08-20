#include "NativeCommands/CommandSchemas.hpp"

namespace geomsrv {

namespace {

struct CommandSchema {
    const char* name;
    const char* input;
    const char* output;
};

constexpr const char kSchemaDefinitions[] =
    R"json({"ElementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]},"Element":{"type":"object","properties":{"elementId":{"$ref":"#ElementId"}},"additionalProperties":false,"required":["elementId"]},"Elements":{"type":"array","items":{"$ref":"#Element"}},"Point2D":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"}},"additionalProperties":false,"required":["x","y"]},"Point3D":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}},"additionalProperties":false,"required":["x","y","z"]}})json";

constexpr const char kEmptyObject[] = R"json({"type":"object","properties":{},"additionalProperties":false})json";

// Entries are added only when the corresponding handler has been converted to
// the same final wire shape. Keeping schemas and handler changes in one C++ pass
// prevents declarative contracts from getting ahead of runtime behavior.
constexpr CommandSchema schemas[] = {
    { "GetCommands", kEmptyObject,
      R"json({"type":"object","properties":{"commands":{"type":"array","items":{"type":"string"}},"status":{"type":"string"},"broken":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["commands","status","broken"]})json" },

    { "RunCommand",
      R"json({"type":"object","properties":{"folder":{"type":"string","minLength":1},"params":{"type":"object"}},"additionalProperties":false,"required":["folder","params"]})json",
      R"json({"type":"object","properties":{"started":{"type":"boolean"},"generation":{"type":"integer","minimum":0},"title":{"type":"string"}},"additionalProperties":false,"required":["started","generation","title"]})json" },

    { "GetRunState", kEmptyObject,
      R"json({"type":"object","properties":{"active":{"type":"boolean"},"generation":{"type":"integer","minimum":0},"title":{"type":"string"},"status":{"type":"string"},"headers":{"type":"array","items":{"type":"string"}},"rows":{"type":"array","items":{"type":"string"}},"resultText":{"type":"string"}},"additionalProperties":false,"required":["active","generation","title","status","headers","rows","resultText"]})json" },

    { "CancelRun", kEmptyObject,
      R"json({"type":"object","properties":{"requested":{"type":"boolean"}},"additionalProperties":false,"required":["requested"]})json" },

    { "GetServerState", kEmptyObject,
      R"json({"type":"object","properties":{"running":{"type":"boolean"},"port":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["running","port"]})json" },

    { "StartServer", kEmptyObject,
      R"json({"type":"object","properties":{"started":{"type":"boolean"},"port":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["started","port"]})json" },

    { "StopServer", kEmptyObject,
      R"json({"type":"object","properties":{"stopped":{"type":"boolean"}},"additionalProperties":false,"required":["stopped"]})json" },

    { "SetTracing",
      R"json({"type":"object","properties":{"enabled":{"type":"boolean","description":"Whether dispatcher API tracing is enabled; defaults to true."}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"enabled":{"type":"boolean"}},"additionalProperties":false,"required":["enabled"]})json" },

    { "GetErrorTrail",
      R"json({"type":"object","properties":{"limit":{"type":"integer","minimum":0,"description":"Maximum number of recent failure entries to return; defaults to 10. Zero returns no entries."}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"entries":{"type":"array","description":"Recent native failure summaries, oldest first.","items":{"type":"string"}},"total":{"type":"integer","minimum":0,"description":"Failures reported since the add-on loaded."},"logPath":{"type":"string","description":"Path to the full native API error log, or an empty string when unavailable."}},"additionalProperties":false,"required":["entries","total","logPath"]})json" },

    { "SetStatus",
      R"json({"type":"object","properties":{"message":{"type":"string"}},"additionalProperties":false,"required":["message"]})json",
      R"json({"type":"object","properties":{"shown":{"type":"string","description":"Status message accepted for asynchronous display."}},"additionalProperties":false,"required":["shown"]})json" },

    { "ShowAlert",
      R"json({"type":"object","properties":{"message":{"type":"string"}},"additionalProperties":false,"required":["message"]})json",
      R"json({"type":"object","properties":{"shown":{"type":"string","description":"Alert message accepted for asynchronous display."}},"additionalProperties":false,"required":["shown"]})json" },

    { "ShowResults",
      R"json({"type":"object","properties":{"headers":{"type":"array","items":{"type":"string"}},"rows":{"type":"array","description":"Table rows as JSON strings. Each string encodes an object with required cells (an array of strings), optional rgb (three integer channels from 0 to 255), and optional guid (an element GUID string).","items":{"type":"string"}}},"additionalProperties":false,"required":["headers","rows"]})json",
      R"json({"type":"object","properties":{"rows":{"type":"integer","minimum":0,"description":"Number of rows accepted for asynchronous display."}},"additionalProperties":false,"required":["rows"]})json" },

    { "ShowResultText",
      R"json({"type":"object","properties":{"text":{"type":"string"}},"additionalProperties":false,"required":["text"]})json",
      R"json({"type":"object","properties":{"shown":{"type":"string","description":"Result text accepted for asynchronous display."}},"additionalProperties":false,"required":["shown"]})json" },

    { "GetCurrentParams", kEmptyObject,
      R"json({"type":"object","properties":{"paramsJson":{"type":"string","description":"Current palette parameter values serialized as a JSON object; empty when no palette is available."}},"additionalProperties":false,"required":["paramsJson"]})json" },

    { "PollCancel", kEmptyObject,
      R"json({"type":"object","properties":{"cancelled":{"type":"boolean"},"running":{"type":"boolean"},"reason":{"type":"string","description":"Human-readable cancellation reason, or an empty string when not cancelled."}},"additionalProperties":false,"required":["cancelled","running","reason"]})json" },

    { "PollSelectionPrompt", kEmptyObject,
      R"json({"type":"object","properties":{"continued":{"type":"boolean"},"cancelled":{"type":"boolean"},"active":{"type":"boolean"}},"additionalProperties":false,"required":["continued","cancelled","active"]})json" },

    { "ShowSelectionPrompt",
      R"json({"type":"object","properties":{"message":{"type":"string"}},"additionalProperties":false,"required":["message"]})json",
      R"json({"type":"object","properties":{"shown":{"type":"boolean","const":true,"description":"True when showing the prompt was accepted for asynchronous dispatch."}},"additionalProperties":false,"required":["shown"]})json" },

    { "HideSelectionPrompt", kEmptyObject,
      R"json({"type":"object","properties":{"shown":{"type":"boolean","const":false,"description":"False when hiding the prompt was accepted for asynchronous dispatch."}},"additionalProperties":false,"required":["shown"]})json" },

    { "CommitTransaction",
      R"json({"type":"object","properties":{"name":{"type":"string","description":"Archicad undo-step name; defaults to 'EvP command'."},"steps":{"type":"array","description":"Transaction steps as JSON strings, replayed in array order inside one undo scope. Each string encodes command, params, bindings, and bindingCount. Each binding string uses step plus dotted source key and dotted target path; values may be booleans, integers, numbers, strings, or complete nested objects.","items":{"type":"string"}}},"additionalProperties":false,"required":["steps"]})json", R"json({"type":"object","properties":{"results":{"type":"array","description":"Per-step successful response objects serialized as JSON strings, positionally aligned with the input steps.","items":{"type":"string"}},"steps":{"type":"integer","minimum":0,"description":"Number of committed steps."}},"additionalProperties":false,"required":["results","steps"]})json" }
};

GS::Optional<GS::UniString> Lookup (const GS::String& commandName, bool input)
{
    for (const CommandSchema& schema : schemas) {
        if (schema.name != nullptr && commandName == schema.name)
            return GS::UniString (input ? schema.input : schema.output);
    }
    return GS::NoValue;
}

} // namespace

GS::Optional<GS::UniString> GetNativeSchemaDefinitions ()
{
    // Archicad add-on command schemas resolve local definitions as "#Name".
    return GS::UniString (kSchemaDefinitions);
}

GS::Optional<GS::UniString> GetNativeInputSchema (const GS::String& commandName)
{
    return Lookup (commandName, true);
}

GS::Optional<GS::UniString> GetNativeResponseSchema (const GS::String& commandName)
{
    return Lookup (commandName, false);
}

} // namespace geomsrv
