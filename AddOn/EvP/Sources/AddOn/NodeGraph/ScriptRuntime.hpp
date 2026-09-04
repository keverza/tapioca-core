#ifndef EVP_NODEGRAPH_SCRIPTRUNTIME_HPP
#define EVP_NODEGRAPH_SCRIPTRUNTIME_HPP

// Running a script node's body, in whichever language it is written in.
//
// ⚠️ ONE INTERFACE, TWO ENGINES, AND THE NODE CANNOT TELL WHICH IT GOT. That is
// the property worth protecting: a Python script node and a JavaScript one differ
// in the file's syntax and in nothing else - same ports, same value vocabulary,
// same failure shape, same log. The moment one engine grows a capability the
// other cannot express, the catalog has two node families pretending to be one.
//
// ⚠️ AND A SCRIPT IS UNTRUSTED CODE INSIDE ARCHICAD'S PROCESS, WITH THE USER'S
// UNSAVED PROJECT IN IT. Every implementation owes three things, and none of them
// is optional:
//
//   * a TIME BUDGET that a `while (true)` cannot outlast;
//   * NO HOST ACCESS. A script sees its inputs and nothing else - no filesystem,
//     no network, no ACAPI. A node that wants the model wires an Archicad node
//     into itself, which is a thing the graph can see, cache and re-run;
//   * FAILURE AS A RETURN VALUE. An exception, a syntax error, a stack overflow
//     and a timeout all come back as `ok == false` with a message a person can
//     act on. Nothing propagates out of Run.
//
// The JavaScript engine is portable C and compiles into the offline suite, so
// script nodes are covered by tests that need neither Archicad nor a rebuild.
// Python's runtime loads CPython through PythonHost and therefore installs
// itself from the add-on, the way ArchicadHost does.

#include "NodeGraph/Evaluator.hpp"
#include "NodeGraph/NodeType.hpp"
#include "NodeGraph/ScriptManifest.hpp"

#include <string>
#include <vector>

namespace evp::nodegraph {

struct ScriptRunRequest {
    ScriptLanguage language = ScriptLanguage::JavaScript;

    // The file this source came from. Used for stack traces and for the log, so
    // a message points at the file the user has open rather than at "<script>".
    std::string path;
    std::string source;

    // Folders the engine must let the script import from: the node's own folder
    // first, then the shared library. Empty for a node whose workspace would not
    // resolve, and empty is a legal request - a script with no imports runs the
    // same either way.
    //
    // ⚠️ THIS IS AN IMPORT PATH, NOT FILESYSTEM ACCESS, AND THE DISTINCTION IS THE
    // WHOLE SECURITY STORY. The rule above still holds: a script sees its inputs
    // and no host. What these roots add is that `import calculations` resolves to
    // a file the user put beside their own script - not `open()`, not a directory
    // listing, and not a way to reach anything the user did not author.
    std::vector<std::string> importRoots;

    // Inputs, keyed by port id, exactly as the evaluator gathered them. They are
    // injected as top-level variables of the same names: the script reads `radius`
    // because its header said `@in radius`, with no envelope object to unwrap.
    ValueMap inputs;

    // What the script is expected to leave behind, read back from top-level
    // variables of these names. Passed in rather than discovered, because a
    // script that failed to set one has to be told which one - "the script did
    // not produce 'area'" is actionable and "nothing happened" is not.
    std::vector<PortSchema> outputs;

    // The same budget the evaluator applies to every node body. An implementation
    // must enforce it from INSIDE the engine - an interrupt handler, not a thread
    // it hopes to kill - because a runaway script holds no lock anyone can take.
    double timeBudgetMs = 1000.0;

    CancellationToken cancellation;
};

struct ScriptRunResult {
    bool ok = false;
    ValueMap outputs;

    // One sentence, and where possible the line. A script author is looking at
    // the same file in another window; a message that does not say where is a
    // message that makes them re-read the whole file.
    std::string error;

    // Whatever the script printed - `print` in Python, `console.log` in
    // JavaScript. Captured rather than discarded because printf debugging is how
    // people actually debug a script node, and there is no console attached to a
    // node running on a worker thread inside Archicad.
    std::vector<std::string> log;
};

class IScriptRuntime {
  public:
    virtual ~IScriptRuntime () = default;

    // Never throws, never blocks past the budget, and never touches the host.
    // Reentrant across threads: the evaluator's worker pool may call this from
    // several threads at once, so an implementation holding one interpreter must
    // say so by serialising internally rather than by corrupting quietly.
    virtual ScriptRunResult Run (const ScriptRunRequest& request) = 0;
};

// The runtime for a language, or nullptr when this build has none installed.
//
// ⚠️ nullptr IS AN ORDINARY ANSWER AND MUST BE REPORTED, NOT ASSERTED. Python's
// runtime is absent in the offline suite and absent in an add-on whose CPython
// failed to resolve; in both cases the right behaviour is a node that fails with
// "the Python runtime is not available", not a crash and not a silent skip.
IScriptRuntime* ActiveScriptRuntime (ScriptLanguage language);
void SetActiveScriptRuntime (ScriptLanguage language, IScriptRuntime* runtime);

// The JavaScript engine, built into this binary. Installing it is a call rather
// than a static initialiser so the offline suite and the add-on both decide
// explicitly when the engine exists - a static initialiser would make the
// language available before anything had decided it should be.
void InstallJavaScriptRuntime ();

} // namespace evp::nodegraph

#endif
