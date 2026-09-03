#include "Python/GraphScriptRuntime.hpp"

#include "NodeGraph/Json.hpp"
#include "NodeGraph/ScriptRuntime.hpp"
#include "NodeGraph/ScriptValueJson.hpp"
#include "Python/PythonHost.hpp"

#include <string>

namespace evp {
namespace {

namespace graph = evp::nodegraph;
using graph::json::JsonArray;
using graph::json::JsonObject;
using graph::json::JsonValue;

std::string Utf8 (const GS::UniString& text)
{
    return std::string (text.ToCStr (0, MaxUSize, CC_UTF8).Get ());
}

class PythonScriptRuntime final : public graph::IScriptRuntime {
  public:
    graph::ScriptRunResult Run (const graph::ScriptRunRequest& request) override
    {
        graph::ScriptRunResult result;

        // Checked before the interpreter is asked for anything: the evaluator
        // cancels a superseded run, and starting CPython for a run nobody is
        // waiting on is seconds of work thrown away.
        if (request.cancellation.IsCancelled ()) {
            result.error = "the run was cancelled";
            return result;
        }

        JsonObject inputs;
        for (const auto& [portId, value] : request.inputs)
            inputs.emplace (portId, graph::ScriptValueToJson (value));

        JsonArray outputs;
        for (const graph::PortSchema& port : request.outputs) {
            JsonObject encoded;
            encoded.emplace ("portId", JsonValue::String (port.id));
            outputs.push_back (JsonValue::Object (std::move (encoded)));
        }

        GS::UniString resultJson;
        GS::UniString error;
        const bool called = PythonHost::Get ().RunGraphScript (
            GS::UniString (request.source.c_str (), CC_UTF8), GS::UniString (request.path.c_str (), CC_UTF8),
            GS::UniString (graph::json::Write (JsonValue::Object (std::move (inputs)), 0).c_str (), CC_UTF8),
            GS::UniString (graph::json::Write (JsonValue::Array (std::move (outputs)), 0).c_str (), CC_UTF8),
            static_cast<int> (request.timeBudgetMs), resultJson, error);

        if (!called) {
            // The run could not be ATTEMPTED - no runtime, a broken bridge. A
            // script that merely threw comes back below as an ordinary result
            // carrying ok=false, which is a different thing and reads differently
            // to the person who wrote the script.
            result.error = error.IsEmpty () ? "the python runtime could not run this script" : Utf8 (error);
            return result;
        }

        const graph::json::ParseResult parsed = graph::json::Parse (Utf8 (resultJson));
        if (!parsed.ok) {
            result.error = "the python runtime returned a malformed result";
            return result;
        }

        if (const JsonValue* log = parsed.value.Find ("log"); log != nullptr) {
            if (const JsonArray* lines = log->AsArray (); lines != nullptr) {
                for (const JsonValue& line : *lines) {
                    std::string text;
                    if (line.AsString (text))
                        result.log.push_back (std::move (text));
                }
            }
        }

        bool ok = false;
        if (const JsonValue* member = parsed.value.Find ("ok"); member != nullptr)
            member->AsBool (ok);
        if (!ok) {
            std::string message;
            if (const JsonValue* member = parsed.value.Find ("error"); member != nullptr)
                member->AsString (message);
            result.error = message.empty () ? "the script failed" : message;
            return result;
        }

        const JsonValue* produced = parsed.value.Find ("outputs");
        const JsonObject* values = produced == nullptr ? nullptr : produced->AsObject ();
        if (values == nullptr) {
            result.error = "the python runtime returned no outputs";
            return result;
        }

        // Decoded against the port's DECLARED type, exactly as the JavaScript
        // engine does. Python is dynamically typed and will happily leave a
        // string in a variable a header called a number; that has to fail here,
        // naming the port, rather than downstream where the wire type no longer
        // matches anything.
        for (const graph::PortSchema& port : request.outputs) {
            const auto entry = values->find (port.id);
            if (entry == values->end ()) {
                result.error = "the script did not set '" + port.id + "'";
                return result;
            }
            graph::Argument decoded;
            std::string error;
            if (!graph::ScriptValueFromJson (entry->second, port.valueType, decoded, error)) {
                result.error = "'" + port.id + "': " + error;
                return result;
            }
            result.outputs.insert_or_assign (port.id, std::move (decoded));
        }

        result.ok = true;
        return result;
    }
};

PythonScriptRuntime gPythonRuntime;

} // namespace

void InstallPythonScriptRuntime ()
{
    graph::SetActiveScriptRuntime (graph::ScriptLanguage::Python, &gPythonRuntime);
}

} // namespace evp
