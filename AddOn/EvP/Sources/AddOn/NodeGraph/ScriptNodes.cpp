#include "NodeGraph/ScriptNodes.hpp"

#include "NodeGraph/NodeInputs.hpp"
#include "NodeGraph/ScriptRuntime.hpp"

#include <sstream>
#include <stdexcept>
#include <utility>

namespace evp::nodegraph {
namespace {

std::string StoredText (const Node& node, const char* parameterId)
{
    const auto entry = node.parameters.find (parameterId);
    if (entry == node.parameters.end () || entry->second.Type () != ValueType::String)
        return {};
    return std::get<std::string> (entry->second.DataValue ());
}

NodeType ScriptNodeType (const char* id, const char* label, ScriptLanguage language, const char* description)
{
    NodeType type;
    type.id = id;
    type.label = label;
    type.category = "Script";
    type.description = description;

    // Worker, and Pure. A script sees its inputs and nothing else - no host, no
    // filesystem, no ACAPI - so there is nothing about it that needs the main
    // thread and nothing it can do to the world outside its own outputs. That is
    // what lets several script nodes on the same topological level run at once.
    type.executionDomain = ExecutionDomain::Worker;
    type.effect = EffectKind::Pure;
    type.display = NodeDisplay::Script;

    // The ports come from the file. See NodeType::instancePorts.
    type.instancePorts = true;

    ParameterUi pathUi;
    pathUi.widget = ParameterWidget::Text;
    pathUi.section = "Script";
    pathUi.order = 0;
    pathUi.help = "The file this node runs. Edit it in VSCode or Sublime; the node follows it.";
    ParameterSchema path { kScriptPathParameter, "File", ValueType::String, false, Value (std::string {}) };
    path.ui = std::move (pathUi);
    type.parameters.push_back (std::move (path));

    // Read-only and derived from the path's extension. It is a parameter rather
    // than something the client infers because the runtime is what decides which
    // engine ran, and a client guessing from an extension would eventually
    // disagree with the node about what it just executed.
    ParameterUi languageUi;
    languageUi.widget = ParameterWidget::ReadOnly;
    languageUi.section = "Script";
    languageUi.order = 1;
    ParameterSchema languageParameter { kScriptLanguageParameter, "Runtime", ValueType::String, false,
                                        Value (std::string (ScriptLanguageName (language))) };
    languageParameter.ui = std::move (languageUi);
    type.parameters.push_back (std::move (languageParameter));

    // ⚠️ NOT COSMETIC. It is in the parameter set so that it lands in the
    // evaluator's cache key: a saved file whose ports did not change would
    // otherwise reload into an identical node and be served the previous run's
    // cached outputs, which is precisely the bug "the node updates when I save"
    // is supposed to not have.
    ParameterUi hashUi;
    hashUi.widget = ParameterWidget::ReadOnly;
    hashUi.section = "Script";
    hashUi.order = 2;
    hashUi.help = "Identifies the loaded source. Changes whenever the file is reloaded.";
    ParameterSchema hash { kScriptSourceHashParameter, "Loaded", ValueType::String, false, Value (std::string {}) };
    hash.ui = std::move (hashUi);
    type.parameters.push_back (std::move (hash));

    return type;
}

} // namespace

bool IsScriptNodeType (const std::string& nodeTypeId)
{
    return nodeTypeId == kJavaScriptNodeType || nodeTypeId == kPythonNodeType;
}

bool ScriptNodeLanguage (const std::string& nodeTypeId, ScriptLanguage& language)
{
    if (nodeTypeId == kJavaScriptNodeType) {
        language = ScriptLanguage::JavaScript;
        return true;
    }
    if (nodeTypeId == kPythonNodeType) {
        language = ScriptLanguage::Python;
        return true;
    }
    return false;
}

void RegisterScriptNodes (NodeRegistry& registry)
{
    std::string error;
    if (!registry.Register (ScriptNodeType (kJavaScriptNodeType, "JavaScript", ScriptLanguage::JavaScript,
                                            "Runs a .js file from disk. Its ports come from the file's header."),
                            error))
        throw std::logic_error (error);
    if (!registry.Register (ScriptNodeType (kPythonNodeType, "Python", ScriptLanguage::Python,
                                            "Runs a .py file from disk. Its ports come from the file's header."),
                            error))
        throw std::logic_error (error);
}

std::string HashScriptSource (const std::string& source)
{
    // FNV-1a, then hex. Not cryptographic and does not need to be: nothing here
    // defends against a crafted collision, because whoever could craft one
    // already controls the file the node is about to run.
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char byte : source) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    std::ostringstream text;
    text << std::hex << hash;
    return text.str ();
}

ScriptState LoadScriptState (const std::string& path, ScriptLanguage language)
{
    ScriptState state;
    state.path = path;
    state.language = language;

    const ScriptWorkspace workspace = ResolveScriptWorkspace (path, language);
    if (!workspace.ok) {
        state.loadError = workspace.error;
        return state;
    }
    state.entryFile = workspace.entryFile;
    state.importRoots = workspace.importRoots;
    state.files = ListWorkspaceFiles (workspace);

    const ScriptRead read = ReadScript (workspace.entryFile);
    // ⚠️ THE STAMP IS THE FOLDER'S, NOT THE READ'S. ReadScript hands back a stamp
    // for main.py alone; a node whose calculations.py changed is just as stale,
    // and storing the narrower stamp is what would make that change invisible
    // until something happened to touch the entry file too.
    state.diskStamp = StampWorkspace (workspace);
    if (!read.ok) {
        state.loadError = read.error;
        return state;
    }

    state.source = read.source;
    state.sourceHash = HashScriptSource (read.source);
    state.loadedStamp = state.diskStamp;
    state.manifest = ParseScriptManifest (read.source, language);
    return state;
}

ScriptStore& ScriptStore::Get ()
{
    static ScriptStore store;
    return store;
}

ScriptState ScriptStore::State (const NodeId& nodeId) const
{
    const std::lock_guard<std::mutex> guard (mutex_);
    const auto entry = states_.find (nodeId);
    return entry == states_.end () ? ScriptState {} : entry->second;
}

bool ScriptStore::Knows (const NodeId& nodeId) const
{
    const std::lock_guard<std::mutex> guard (mutex_);
    return states_.contains (nodeId);
}

void ScriptStore::SetState (const NodeId& nodeId, ScriptState state)
{
    const std::lock_guard<std::mutex> guard (mutex_);
    states_.insert_or_assign (nodeId, std::move (state));
}

void ScriptStore::SetLog (const NodeId& nodeId, std::vector<std::string> log)
{
    const std::lock_guard<std::mutex> guard (mutex_);
    const auto entry = states_.find (nodeId);
    if (entry != states_.end ())
        entry->second.lastLog = std::move (log);
}

void ScriptStore::Forget (const NodeId& nodeId)
{
    const std::lock_guard<std::mutex> guard (mutex_);
    states_.erase (nodeId);
}

std::vector<std::string> ScriptStore::WatchedPaths () const
{
    const std::lock_guard<std::mutex> guard (mutex_);
    std::vector<std::string> paths;
    bool anyNode = false;
    for (const auto& [nodeId, state] : states_) {
        // The ENTRY FILE, so the watcher derives the node's own folder from it -
        // which is what makes a save to any helper in that folder arrive too.
        if (!state.entryFile.empty ()) {
            paths.push_back (state.entryFile);
            anyNode = true;
        }
    }
    // The shared library, once, as a path INSIDE it: the watcher takes directory
    // names from the parent of what it is handed, and a change to libs\geometry.py
    // has to reach every node that imports it. The name need not exist - the
    // derivation is a string operation, not a stat.
    const std::string shared = SharedLibraryRoot ();
    if (anyNode && !shared.empty ())
        paths.push_back (shared + "\\.watch");
    return paths;
}

std::vector<NodeId> ScriptStore::RefreshDiskStamps ()
{
    // The stat calls happen OUTSIDE the lock. Each one is a filesystem round
    // trip, and a network drive or a sleeping disk turns a handful of them into
    // a visible stall - held under the lock, that stall would be inflicted on
    // every worker thread trying to read a script at the same time.
    struct Pending {
        NodeId nodeId;
        std::string path;
        ScriptLanguage language;
    };
    std::vector<Pending> paths;
    {
        const std::lock_guard<std::mutex> guard (mutex_);
        for (const auto& [nodeId, state] : states_)
            if (!state.path.empty ())
                paths.push_back (Pending { nodeId, state.path, state.language });
    }

    std::vector<NodeId> stale;
    for (const auto& [nodeId, path, language] : paths) {
        const ScriptStamp stamp = StampWorkspace (ResolveScriptWorkspace (path, language));
        const std::lock_guard<std::mutex> guard (mutex_);
        const auto entry = states_.find (nodeId);
        // Gone or repointed while we were statting. Its stamp belongs to a path
        // this loop is no longer describing, so writing it would be worse than
        // skipping it.
        if (entry == states_.end () || entry->second.path != path)
            continue;
        entry->second.diskStamp = stamp;
        if (entry->second.IsStale ())
            stale.push_back (nodeId);
    }
    return stale;
}

bool ExecuteScriptNode (const Node& node, const ValueMap& inputs, const NodeExecutionContext& context,
                        ValueMap& outputs, std::string& error)
{
    ScriptLanguage language = ScriptLanguage::JavaScript;
    if (!ScriptNodeLanguage (node.nodeType, language)) {
        error = "not a script node";
        return false;
    }

    const ScriptState state = ScriptStore::Get ().State (node.id);

    if (!state.loadError.empty ()) {
        error = state.loadError;
        return false;
    }
    if (state.source.empty ()) {
        // Reached when the node has never been loaded - a graph just opened, or a
        // node just placed. Saying so is better than "the script did not set
        // 'x'", which is what running an empty source would produce.
        error = StoredText (node, kScriptPathParameter).empty ()
                    ? "this script node has no file yet; choose or create one"
                    : "the script has not been loaded yet; press Reload";
        return false;
    }
    if (!state.manifest.Ok ()) {
        // The first diagnostic, with its line. The rest are on the node already;
        // repeating all of them in a one-line node error would push the useful
        // one off the end.
        const ScriptDiagnostic& first = state.manifest.diagnostics.front ();
        error = first.line == 0 ? first.message : "line " + std::to_string (first.line) + ": " + first.message;
        return false;
    }

    IScriptRuntime* runtime = ActiveScriptRuntime (language);
    if (runtime == nullptr) {
        // A supported state, reported plainly. Python's runtime is absent in the
        // offline suite and in an add-on whose CPython did not resolve.
        error = std::string ("the ") + ScriptLanguageName (language) + " runtime is not available in this build";
        return false;
    }

    ScriptRunRequest request;
    request.language = language;
    // The ENTRY FILE, not the folder: this is what a traceback names, and the
    // author has that exact file open in another window.
    request.path = state.entryFile;
    request.source = state.source;
    // What the folder model is FOR. Without these the node's own helpers are not
    // importable and every script that outgrew one file has to write into
    // sys.path itself - which is the thing this model exists to remove.
    request.importRoots = state.importRoots;
    request.inputs = inputs;
    request.outputs = state.manifest.outputs;
    request.cancellation = context.cancellation;
    // The node's own budget rather than the run's: a script is the one node body
    // in the catalog written by whoever is sitting at the machine, so it is the
    // one that will actually contain an infinite loop. Kept short enough that
    // hitting it feels like a mistake being caught rather than Archicad hanging.
    request.timeBudgetMs = 5000.0;

    ScriptRunResult result = runtime->Run (request);
    // Kept whether the script succeeded or not, and written before the failure
    // returns: what a failing script printed on its way to failing is usually the
    // most useful thing on the screen.
    ScriptStore::Get ().SetLog (node.id, std::move (result.log));
    if (!result.ok) {
        error = result.error.empty () ? "the script failed" : result.error;
        return false;
    }
    outputs = std::move (result.outputs);
    return true;
}

} // namespace evp::nodegraph
