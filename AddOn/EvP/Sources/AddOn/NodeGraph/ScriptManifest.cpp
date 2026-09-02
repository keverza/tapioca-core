#include "NodeGraph/ScriptManifest.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace evp::nodegraph {
namespace {

// Bounds on the header scan. The file is user input arriving from disk, and it
// may be a 200MB minified bundle someone pointed the node at by mistake; the
// parse must cost the same either way. Both limits are generous next to any
// header a person would write and tiny next to the file that makes them matter.
constexpr size_t kMaxHeaderLines = 200;
constexpr size_t kMaxHeaderBytes = 64 * 1024;

// The per-side ceiling. A node with a thousand ports is not a node anyone meant
// to make, and refusing it here keeps the editor from having to survive one.
constexpr size_t kMaxPorts = 64;

struct TypeWord {
    const char* word;
    ValueType type;
};

// "any" maps to Absent, which the edit rules already read as "accepts anything"
// on an input. On an OUTPUT it means the port only wires into another `any`,
// because nothing downstream can be told what it will receive - stated in the
// header's own documentation rather than left for someone to discover.
constexpr TypeWord kTypeWords[] = {
    { "any", ValueType::Absent },        { "bool", ValueType::Bool },
    { "integer", ValueType::Integer },   { "number", ValueType::Double },
    { "text", ValueType::String },       { "point", ValueType::Point3 },
    { "polyline", ValueType::Polyline }, { "polygon", ValueType::Polygon },
    { "mesh", ValueType::Mesh },         { "element", ValueType::ArchicadElementRef },
    { "list", ValueType::List },
};

std::string Trim (const std::string& text)
{
    const auto notSpace = [] (unsigned char character) { return std::isspace (character) == 0; };
    const auto begin = std::find_if (text.begin (), text.end (), notSpace);
    const auto end = std::find_if (text.rbegin (), text.rend (), notSpace).base ();
    return begin >= end ? std::string {} : std::string (begin, end);
}

bool IsIdentifier (const std::string& text)
{
    if (text.empty ())
        return false;
    // A port id crosses the bridge as a JSON key and is matched against edges by
    // string equality. Restricting it to what a language would call an identifier
    // keeps a port id from ever needing quoting, escaping or normalisation on any
    // of the three sides that handle one.
    if (std::isalpha (static_cast<unsigned char> (text.front ())) == 0 && text.front () != '_')
        return false;
    return std::all_of (text.begin (), text.end (),
                        [] (unsigned char character) { return std::isalnum (character) != 0 || character == '_'; });
}

// Splits a directive's tail into `<body> "<quoted>"`. The quoted part is the
// port's label - the words a person wants to see on the node, which are prose
// and so cannot be an identifier.
//
// â ï¸ THE LABEL IS THE LAST QUOTED GROUP, SCANNED FROM THE END, AND TAKING
// THE FIRST ONE WAS A BUG. A text input may carry a quoted DEFAULT, so the line
//
//     @in label : text = "wall"   "Some text"
//
// has two quoted groups. Reading the first made `"wall"` the label and left the
// default empty - and nothing failed: the port appeared, correctly typed, simply
// mislabelled and with no value. The example file that carries every type is what
// caught it.
void SplitTrailingLabel (const std::string& tail, std::string& body, std::string& label)
{
    body = tail;
    label.clear ();
    const std::string trimmed = Trim (tail);
    if (trimmed.size () < 2 || trimmed.back () != '"')
        return;
    const size_t open = trimmed.find_last_of ('"', trimmed.size () - 2);
    if (open == std::string::npos)
        return;

    const std::string remainder = Trim (trimmed.substr (0, open));
    // â ï¸ ONE QUOTED GROUP AFTER `=` IS THE DEFAULT, NOT A LABEL, and this
    // is the whole ambiguity in the grammar:
    //
    //     @in a : text = "only"                 the quotes are the VALUE
    //     @in a : text = "wall"   "Some text"   the last group is the label
    //
    // Both are natural to write and they differ only in what follows. Deciding by
    // what is LEFT BEHIND settles it without lookahead: if removing the trailing
    // group leaves a body ending in `=`, the group was the value that `=` was
    // introducing, so it is put back. Guessing the other way would make
    // `text = "only"` a port with a label of "only" and no value at all.
    if (!remainder.empty () && remainder.back () == '=') {
        label.clear ();
        return;
    }

    label = trimmed.substr (open + 1, trimmed.size () - open - 2);
    body = remainder;
}

// A literal default, in the small set a header can express. Geometry is
// deliberately absent: a point or a mesh typed into a comment would be a second,
// worse authoring surface for something the graph already has nodes for.
bool ParseLiteral (const std::string& text, ValueType expected, Value& value)
{
    if (text.empty ())
        return false;
    if (text.front () == '"' && text.size () >= 2 && text.back () == '"') {
        value = Value (text.substr (1, text.size () - 2));
        return expected == ValueType::String || expected == ValueType::Absent;
    }
    if (text == "true" || text == "false") {
        value = Value (text == "true");
        return expected == ValueType::Bool || expected == ValueType::Absent;
    }
    char* end = nullptr;
    const double number = std::strtod (text.c_str (), &end);
    if (end == nullptr || end == text.c_str () || *end != '\0')
        return false;
    // An Integer port keeps an integer default. Handing it a Double would make
    // the stored parameter's type disagree with its port, which ValidateNode
    // rejects - and the rejection would name the node, not the header line that
    // caused it.
    if (expected == ValueType::Integer) {
        value = Value (static_cast<int64_t> (number));
        return true;
    }
    value = Value (number);
    return expected == ValueType::Double || expected == ValueType::Absent;
}

} // namespace

const char* ScriptLanguageName (ScriptLanguage language)
{
    switch (language) {
        case ScriptLanguage::JavaScript:
            return "javascript";
        case ScriptLanguage::Python:
            return "python";
    }
    return "javascript";
}

bool ParseScriptLanguage (const std::string& name, ScriptLanguage& language)
{
    for (const ScriptLanguage candidate : { ScriptLanguage::JavaScript, ScriptLanguage::Python }) {
        if (name == ScriptLanguageName (candidate)) {
            language = candidate;
            return true;
        }
    }
    return false;
}

const char* ScriptCommentPrefix (ScriptLanguage language)
{
    return language == ScriptLanguage::Python ? "#" : "//";
}

bool ScriptLanguageFromPath (const std::string& path, ScriptLanguage& language)
{
    const size_t dot = path.find_last_of ('.');
    if (dot == std::string::npos)
        return false;
    std::string extension = path.substr (dot);
    std::transform (extension.begin (), extension.end (), extension.begin (),
                    [] (unsigned char character) { return static_cast<char> (std::tolower (character)); });
    if (extension == ".js" || extension == ".mjs" || extension == ".cjs") {
        language = ScriptLanguage::JavaScript;
        return true;
    }
    if (extension == ".py") {
        language = ScriptLanguage::Python;
        return true;
    }
    return false;
}

bool ParseScriptValueType (const std::string& word, ValueType& valueType)
{
    for (const TypeWord& candidate : kTypeWords) {
        if (word == candidate.word) {
            valueType = candidate.type;
            return true;
        }
    }
    return false;
}

const char* ScriptValueTypeWord (ValueType valueType)
{
    for (const TypeWord& candidate : kTypeWords)
        if (candidate.type == valueType)
            return candidate.word;
    return "any";
}

ScriptManifest ParseScriptManifest (const std::string& source, ScriptLanguage language)
{
    ScriptManifest manifest;
    const std::string prefix = ScriptCommentPrefix (language);

    const auto fail = [&manifest] (size_t line, std::string message) {
        manifest.diagnostics.push_back ({ line, std::move (message) });
    };

    size_t lineNumber = 0;
    size_t offset = 0;
    const size_t scanEnd = std::min (source.size (), kMaxHeaderBytes);
    bool sawDirective = false;

    while (offset < scanEnd && lineNumber < kMaxHeaderLines) {
        size_t lineEnd = source.find ('\n', offset);
        if (lineEnd == std::string::npos || lineEnd > scanEnd)
            lineEnd = scanEnd;
        std::string line = Trim (source.substr (offset, lineEnd - offset));
        offset = lineEnd + 1;
        ++lineNumber;

        if (line.empty ())
            continue;
        // A shebang is not a comment in either language's sense, but it is a
        // legitimate first line of a Python file and skipping it costs nothing.
        if (lineNumber == 1 && line.rfind ("#!", 0) == 0)
            continue;
        if (line.rfind (prefix, 0) != 0)
            break; // The leading comment block is over. See the header.

        line = Trim (line.substr (prefix.size ()));
        if (line.empty () || line.front () != '@')
            continue;

        const size_t space = line.find_first_of (" \t");
        const std::string directive = line.substr (0, space);
        const std::string tail = space == std::string::npos ? std::string {} : Trim (line.substr (space));

        if (directive == "@name") {
            manifest.name = tail;
            continue;
        }
        if (directive == "@description") {
            // Appended, so a description can wrap over several lines the way one
            // naturally would in a comment block.
            if (!manifest.description.empty ())
                manifest.description += " ";
            manifest.description += tail;
            continue;
        }
        const bool isInput = directive == "@in";
        if (!isInput && directive != "@out") {
            // An unrecognised @word is left alone rather than reported. Comment
            // blocks are full of @param, @returns and @author from whatever other
            // tool the author uses, and a node covered in warnings about its own
            // docstring would teach the user to ignore the warnings that matter.
            continue;
        }
        sawDirective = true;

        std::string body, label;
        SplitTrailingLabel (tail, body, label);

        std::string defaultText;
        if (const size_t equals = body.find ('='); equals != std::string::npos) {
            defaultText = Trim (body.substr (equals + 1));
            body = Trim (body.substr (0, equals));
        }

        std::string typeWord;
        if (const size_t colon = body.find (':'); colon != std::string::npos) {
            typeWord = Trim (body.substr (colon + 1));
            body = Trim (body.substr (0, colon));
        }

        PortSchema port;
        port.id = body;
        port.label = label.empty () ? port.id : label;

        if (!IsIdentifier (port.id)) {
            fail (lineNumber, "'" + port.id + "' is not a usable port name; use letters, digits and underscores");
            continue;
        }
        if (typeWord.empty ()) {
            // An input may omit its type and mean "any". An output may not: the
            // evaluator checks a produced value against its port's type, so an
            // untyped output would have to accept anything - which turns every
            // wiring mistake into a surprise somewhere downstream instead.
            if (!isInput) {
                fail (lineNumber, "output '" + port.id + "' needs a type, as in `@out " + port.id + " : number`");
                continue;
            }
            port.valueType = ValueType::Absent;
        }
        else if (!ParseScriptValueType (typeWord, port.valueType)) {
            fail (lineNumber, "'" + typeWord + "' is not a known type for port '" + port.id + "'");
            continue;
        }

        std::vector<PortSchema>& ports = isInput ? manifest.inputs : manifest.outputs;
        const auto duplicate = std::find_if (ports.begin (), ports.end (),
                                             [&port] (const PortSchema& existing) { return existing.id == port.id; });
        if (duplicate != ports.end ()) {
            fail (lineNumber, "port '" + port.id + "' is declared twice");
            continue;
        }
        if (ports.size () >= kMaxPorts) {
            fail (lineNumber, "too many ports; a script node may declare at most 64 inputs and 64 outputs");
            continue;
        }

        if (!defaultText.empty ()) {
            if (!isInput) {
                fail (lineNumber, "output '" + port.id + "' cannot have a default; only an input can");
                continue;
            }
            Value defaultValue;
            if (!ParseLiteral (defaultText, port.valueType, defaultValue)) {
                fail (lineNumber, "'" + defaultText + "' is not a " + ScriptValueTypeWord (port.valueType) +
                                      " literal for '" + port.id + "'");
                continue;
            }
            manifest.defaults.insert_or_assign (port.id, std::move (defaultValue));
            // An input with a default is not REQUIRED: the node has a value to
            // work with the moment it is placed, and the port exists so something
            // upstream can take over. Exactly the rule NodeRegistry::Register
            // derives for every other type in the catalog.
            port.required = false;
        }

        ports.push_back (std::move (port));
    }

    if (!sawDirective) {
        manifest.diagnostics.push_back ({ 0, "the script declares no ports; add `" + prefix +
                                                 " @in name : number` and `" + prefix +
                                                 " @out name : number` lines to the comment block at the top" });
        return manifest;
    }
    if (manifest.outputs.empty ())
        manifest.diagnostics.push_back (
            { 0, "the script declares no outputs, so nothing downstream could ever read it" });
    return manifest;
}

} // namespace evp::nodegraph
