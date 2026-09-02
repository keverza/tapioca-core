#ifndef EVP_NODEGRAPH_SCRIPTNODES_HPP
#define EVP_NODEGRAPH_SCRIPTNODES_HPP

// The script node family: a node whose behaviour AND interface are authored in a
// file on disk, edited in VSCode or Sublime, and picked up when it is saved.
//
// ⚠️ THERE IS NO EDITOR IN THE PALETTE, AND THERE MUST NOT BE ONE. The file
// belongs to whatever the user already edits code in. A second editable copy
// inside Archicad would be a second source of truth, and the way that fails is
// specific and bad: the user edits in VSCode, the palette still holds what it
// loaded, something writes the palette's copy back, and the VSCode edit is gone.
// The palette reads, reports and reloads. It does not write over a script.
//
// ⚠️ AND THE NODE IS NOT WHERE THE PORTS ARE DECIDED. The header in the file is -
// see ScriptManifest. The node follows the file, which is what makes "rename the
// argument, save, watch the port rename itself" work, and what makes a script
// that has been shared with someone else arrive complete.
//
// The three pieces, and why they are separate:
//
//   * the DOCUMENT holds the path, the language, a hash of the loaded source and
//     the node's ports. All of that persists, because the edges in a saved graph
//     are validated against those ports before anything has read a file.
//   * the STORE below holds the loaded source, its manifest, its diagnostics and
//     the stamp it was read at. None of that persists: it is a cache of a file
//     that may have changed since, and a stale copy restored from a graph file
//     would be a node claiming to run code that is no longer on disk.
//   * the WATCHER notices a save and asks for a reload. It is advisory - a node
//     whose file changed while nothing was watching still reloads on evaluation.

#include "NodeGraph/Evaluator.hpp"
#include "NodeGraph/NodeRegistry.hpp"
#include "NodeGraph/ScriptManifest.hpp"
#include "NodeGraph/ScriptSource.hpp"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace evp::nodegraph {

// The node type ids. One type per language rather than one type with a language
// parameter: the two are picked from the component palette by name, and a user
// looking for "Python" should find a node called Python rather than a node they
// must place and then reconfigure.
constexpr const char* kJavaScriptNodeType = "script.javascript";
constexpr const char* kPythonNodeType = "script.python";

// Parameters every script node carries. Named constants because three files
// write them and one erases around them - a literal typo would silently create a
// second parameter rather than fail.
constexpr const char* kScriptPathParameter = "scriptPath";
constexpr const char* kScriptLanguageParameter = "scriptLanguage";
constexpr const char* kScriptSourceHashParameter = "scriptSourceHash";

bool IsScriptNodeType (const std::string& nodeTypeId);

// The language a script node type runs. False when the id is not a script node.
bool ScriptNodeLanguage (const std::string& nodeTypeId, ScriptLanguage& language);

void RegisterScriptNodes (NodeRegistry& registry);

bool ExecuteScriptNode (const Node& node, const ValueMap& inputs, const NodeExecutionContext& context,
                        ValueMap& outputs, std::string& error);

// ---------------------------------------------------------------------------
// What one node currently knows about its file.
struct ScriptState {
    std::string path;
    ScriptLanguage language = ScriptLanguage::JavaScript;

    // Empty until a successful load. A node with no source fails with the reason
    // it has none, which is nearly always a path that is wrong or a file that has
    // been moved - both worth saying out loud.
    std::string source;
    std::string sourceHash;
    ScriptStamp loadedStamp;
    ScriptManifest manifest;

    // Why the last load attempt failed, empty when it did not. Separate from the
    // manifest's diagnostics: "the file is not there" and "line 4 of the header
    // is wrong" are different problems and lead to different actions.
    std::string loadError;

    // The stamp on disk the last time anything looked, which is how "the file has
    // changed since this node loaded it" is answered without re-reading. The
    // editor draws a badge from it, so a user who saved while the watcher was
    // unavailable can still see that a reload is owed.
    ScriptStamp diskStamp;

    // Whatever the script printed on its last run - `print`, `console.log`.
    // Session-only and overwritten each run, so the editor shows the output of
    // the run the user is looking at rather than an accumulating transcript.
    std::vector<std::string> lastLog;

    bool IsStale () const
    {
        return diskStamp.exists && diskStamp != loadedStamp;
    }
};

// Session state, keyed by node id, for one graph. Deliberately NOT part of
// GraphDocument: see the note at the top of this file.
//
// ⚠️ IT IS TOUCHED FROM THREE THREADS - the command thread on a reload, the
// watcher's thread when a file changes, and a worker thread while a node body
// runs - so every method locks. The store hands out COPIES rather than
// references for the same reason: a worker holding a reference to a source string
// while a save replaces it is a use-after-free with a plausible-looking stack.
class ScriptStore {
  public:
    static ScriptStore& Get ();

    ScriptState State (const NodeId& nodeId) const;

    // Whether this node has ever been loaded in this session. Distinct from
    // "State().path is empty", which is also true of a script node that has been
    // loaded and simply has no file chosen yet - and confusing the two makes
    // adoption re-load that node on every pass.
    bool Knows (const NodeId& nodeId) const;
    void SetState (const NodeId& nodeId, ScriptState state);

    // Replaces just the log. Called from a WORKER thread as a node body finishes,
    // which is why it is not a read-modify-write of the whole state: a worker
    // writing back a ScriptState it copied before the run would undo a reload
    // that landed while the script was executing.
    void SetLog (const NodeId& nodeId, std::vector<std::string> log);
    void Forget (const NodeId& nodeId);

    // Every path any script node currently points at, for handing to the watcher.
    std::vector<std::string> WatchedPaths () const;

    // Re-stats every known path and returns the nodes whose file has moved on
    // since they loaded it. This is the fallback that makes the feature work with
    // no watcher at all, and the reconciliation that makes it correct WITH one -
    // a notification can be missed, coalesced, or arrive for a directory whose
    // interesting file was not the one that changed.
    std::vector<NodeId> RefreshDiskStamps ();

  private:
    ScriptStore () = default;

    mutable std::mutex mutex_;
    std::map<NodeId, ScriptState> states_;
};

// Reads the file, parses its header and returns the state that follows. Pure
// enough to test: it does I/O, but it decides nothing about the document. What to
// DO with a reshaped interface is ApplyScriptReload's business.
ScriptState LoadScriptState (const std::string& path, ScriptLanguage language);

// A stable, cheap identity for a source text. Only ever compared, so what matters
// is that different text gives a different string - not that it is cryptographic.
std::string HashScriptSource (const std::string& source);

} // namespace evp::nodegraph

#endif
