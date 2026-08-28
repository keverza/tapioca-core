#include "DynamoGraphInputs.hpp"

#include "Python/PathUtils.hpp"

#include "ObjectStateJSONConversion.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <utility>

namespace evp::dynamo {
namespace {

// JSON string escaping, done over the UTF-8 BYTES rather than GS::UniChar: a
// UniChar is a class, not an integral type, so it can be neither switched on nor
// compared to a literal. Bytes are safe here because every character this has to
// escape is ASCII, and no ASCII byte ever appears inside a UTF-8 multi-byte
// sequence — so a continuation byte is copied through untouched.
GS::UniString Escape (const GS::UniString& value)
{
    const std::string utf8 (value.ToCStr (0, MaxUSize, CC_UTF8).Get ());
    std::string out;
    out.reserve (utf8.size ());
    for (const char byte : utf8) {
        const unsigned char ch = (unsigned char) byte;
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (ch < 0x20) {
                    char escaped[8] = {0};
                    std::snprintf (escaped, sizeof (escaped), "\\u%04x", (unsigned) ch);
                    out += escaped;
                }
                else {
                    out += byte;
                }
                break;
        }
    }
    return GS::UniString (out.c_str (), CC_UTF8);
}

bool Number (const GS::UniString& text, double& value)
{
    const std::string utf8 (text.ToCStr (0, MaxUSize, CC_UTF8).Get ());
    char* end = nullptr;
    errno = 0;
    value = std::strtod (utf8.c_str (), &end);
    return errno == 0 && end != utf8.c_str () && *end == '\0';
}

GS::UniString GraphParam (const GS::UniString& path)
{
    return "{\"name\":\"graph\",\"label\":\"Dynamo graph\",\"type\":\"FilePath\","
           "\"required\":true,\"extensions\":[\"dyn\"],\"mode\":\"open\",\"default\":\"" +
           Escape (path) + "\"}";
}

} // namespace

CommandInfo LoaderCommand ()
{
    CommandInfo command;
    command.folder = "__TapiocaDynamoLoader";
    command.title = "Dynamo Command Loader";
    command.category = "Dynamo";
    command.description = "Select a .dyn graph. Inputs exposed for Dynamo Player are added below and executed "
                          "in Tapioca's background Dynamo process.";
    command.runtime = "dynamo";
    command.timeoutSeconds = 360.0;
    command.paramJsons.Push (GraphParam (GS::UniString ()));
    return command;
}

bool PopulateLoaderInputs (const GS::UniString& graphPath, CommandInfo& command, GS::UniString& error)
{
    GS::UniString text;
    if (!ReadTextFile (graphPath, text)) {
        error = "Could not read the selected Dynamo graph.";
        return false;
    }
    GS::ObjectState root;
    if (JSON::ConvertToObjectState (text, root) != NoError) {
        error = "The selected .dyn file is not valid JSON.";
        return false;
    }
    GS::Array<GS::ObjectState> inputs;
    if (!root.Get ("Inputs", inputs)) {
        error = "The selected graph has no Dynamo Player input metadata.";
        return false;
    }

    CommandInfo populated = LoaderCommand ();
    populated.paramJsons.Clear ();
    populated.paramJsons.Push (GraphParam (graphPath));
    std::set<std::string> ids;
    for (const GS::ObjectState& input : inputs) {
        GS::UniString id, label, type, value, numberType;
        input.Get ("Id", id);
        input.Get ("Name", label);
        input.Get ("Type", type);
        input.Get ("Value", value);
        input.Get ("NumberType", numberType);
        if (id.IsEmpty () || !ids.insert (id.ToCStr (0, MaxUSize, CC_UTF8).Get ()).second) {
            error = "The graph contains a missing or duplicate exposed input id.";
            return false;
        }
        if (label.IsEmpty ())
            label = id;

        GS::UniString param = "{\"name\":\"" + Escape (id) + "\",\"label\":\"" + Escape (label) + "\",";
        if (type == "number") {
            double number = 0.0;
            if (!Number (value, number)) {
                error = "Dynamo input '" + label + "' has an invalid numeric default.";
                return false;
            }
            if (numberType == "Integer")
                param += "\"type\":\"Int\",\"default\":" + GS::UniString::Printf ("%d", (int) number);
            else
                param += "\"type\":\"Float\",\"default\":" + GS::UniString::Printf ("%.17g", number);
            double minimum = 0.0, maximum = 0.0;
            if (input.Get ("MinimumValue", minimum))
                param += ",\"minimum\":" + GS::UniString::Printf ("%.17g", minimum);
            if (input.Get ("MaximumValue", maximum))
                param += ",\"maximum\":" + GS::UniString::Printf ("%.17g", maximum);
        }
        else if (type == "boolean") {
            const bool enabled = value == "true" || value == "True" || value == "1";
            param += enabled ? "\"type\":\"bool\",\"default\":true" : "\"type\":\"bool\",\"default\":false";
        }
        else if (type == "string") {
            param += "\"type\":\"str\",\"default\":\"" + Escape (value) + "\"";
        }
        else {
            error = "Dynamo input '" + label + "' uses unsupported Player type '" + type +
                    "'. The first headless slice supports number, boolean and string inputs.";
            return false;
        }
        param += "}";
        populated.paramJsons.Push (param);
    }
    command = std::move (populated);
    return true;
}

} // namespace evp::dynamo
