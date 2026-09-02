#ifndef EVP_NODEGRAPH_SCRIPTMANIFEST_HPP
#define EVP_NODEGRAPH_SCRIPTMANIFEST_HPP

// The interface of a script node, as declared by the script itself.
//
// ⚠️ THE FILE IS THE SOURCE OF TRUTH FOR A SCRIPT NODE'S PORTS, AND THIS IS THE
// ONLY PLACE THAT DECIDES WHAT THE FILE SAID. A script node's whole point is
// that its behaviour is authored in VSCode or Sublime rather than in the
// palette; if its INTERFACE were authored in the palette instead, every edit
// would need doing twice and the two copies would disagree the first time
// someone renamed an argument. So the header declares the ports and the node
// follows.
//
// The header is a comment block, so a declared script is still a runnable script:
//
//     # @name   Offset polygon
//     # @in     polygon : polygon
//     # @in     distance : number = 0.5   "Offset distance"
//     # @out    result : polygon
//
// and in JavaScript exactly the same with `//`.
//
// ⚠️ ONLY THE LEADING COMMENT BLOCK IS READ. Scanning the whole file would mean
// a `@out` inside a docstring, a commented-out experiment or a block of vendored
// code could silently reshape the node and drop the user's wires. The block ends
// at the first line that is neither a comment nor blank, which is a rule someone
// can see by looking at their own file.
//
// This header is pure: no I/O, no runtime, no DevKit. Reading the file is
// ScriptSource's job and running it is the runtime's, so what the header MEANS
// is decided offline and covered by the offline suite.

#include "NodeGraph/NodeType.hpp"
#include "NodeGraph/Value.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace evp::nodegraph {

enum class ScriptLanguage {
    JavaScript,
    Python,
};

const char* ScriptLanguageName (ScriptLanguage language);

// The wire and file spelling: "javascript", "python". False for anything else,
// so an unknown language is a rejection rather than a silent fall back to one of
// them - running a Python file through the JS engine produces a syntax error
// forty lines from anything the user wrote.
bool ParseScriptLanguage (const std::string& name, ScriptLanguage& language);

// The language a path implies: .js/.mjs/.cjs or .py. False when the extension is
// neither, which is what makes "pick a file" able to fill the language in
// without ever guessing wrong.
bool ScriptLanguageFromPath (const std::string& path, ScriptLanguage& language);

// The comment prefix a language's header lines carry.
const char* ScriptCommentPrefix (ScriptLanguage language);

struct ScriptDiagnostic {
    // 1-based, and 0 when the problem is the file as a whole rather than a line.
    // The editor puts this straight in front of the user, who is looking at the
    // same file in another window with the same line numbers.
    size_t line = 0;
    std::string message;
};

struct ScriptManifest {
    // From @name / @description. Empty when the script says nothing, in which
    // case a client falls back to the file's own name - it does NOT invent one.
    std::string name;
    std::string description;

    std::vector<PortSchema> inputs;
    std::vector<PortSchema> outputs;

    // Literal defaults from `@in x : number = 2`, keyed by port id. They become
    // INTERNALISED INPUT VALUES - parameters stored under the input's own id -
    // which is the mechanism the rest of the catalog already uses for "the value
    // the node uses until something is wired to that port". Nothing new: see
    // GraphEdit's ValidateNode and NodeInputs.
    std::map<std::string, Value> defaults;

    std::vector<ScriptDiagnostic> diagnostics;

    bool Ok () const
    {
        return diagnostics.empty ();
    }
};

// Parses the leading comment block. NEVER throws and never fails outright: a
// manifest with diagnostics is still returned, with whatever parsed, because the
// editor has to be able to show the user both what it understood and what it did
// not. A caller decides what an unusable manifest means; see ScriptNodes.
ScriptManifest ParseScriptManifest (const std::string& source, ScriptLanguage language);

// The type words a header may use, as the user writes them. Deliberately a
// SEPARATE vocabulary from the file format's (GraphSerializer) and the wire's
// (NodeGraphCommandSupport): those two are formats this code owns on both ends,
// while this one is typed by a person in a text editor and is chosen to read
// well there - "number", not "double"; "text", not "string".
bool ParseScriptValueType (const std::string& word, ValueType& valueType);
const char* ScriptValueTypeWord (ValueType valueType);

} // namespace evp::nodegraph

#endif
