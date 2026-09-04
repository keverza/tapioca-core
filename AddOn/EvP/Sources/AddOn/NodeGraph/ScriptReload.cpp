#include "NodeGraph/ScriptReload.hpp"

#include "NodeGraph/GraphEdit.hpp"
#include "NodeGraph/GraphRuntimeState.hpp"
#include "NodeGraph/ScriptSource.hpp"
#include "NodeGraph/ScriptWorkspace.hpp"

#include <algorithm>

namespace evp::nodegraph {
namespace {

std::string StoredText (const Node& node, const char* parameterId)
{
    const auto entry = node.parameters.find (parameterId);
    if (entry == node.parameters.end () || entry->second.Type () != ValueType::String)
        return {};
    return std::get<std::string> (entry->second.DataValue ());
}

bool SamePorts (const std::vector<PortSchema>& left, const std::vector<PortSchema>& right)
{
    if (left.size () != right.size ())
        return false;
    for (size_t index = 0; index < left.size (); ++index) {
        // Order counts. Ports are drawn top to bottom in the order the header
        // declares them, so a header whose lines were reordered HAS changed the
        // node even though the same set of ports came back.
        if (left[index].id != right[index].id || left[index].valueType != right[index].valueType ||
            left[index].label != right[index].label || left[index].required != right[index].required)
            return false;
    }
    return true;
}

} // namespace

ScriptReloadResult ReloadScriptNode (const GraphId& graphId, const NodeId& nodeId)
{
    ScriptReloadResult result;

    const GraphDocument document = GraphRuntimeState::Get ().Document (graphId);
    const Node* node = document.FindNode (nodeId);
    // Points into `document`, and the migration below can supersede that - so it
    // is re-pointed at a fresh snapshot afterwards. See the note further down.
    if (node == nullptr) {
        result.error = "unknown node: " + nodeId;
        return result;
    }
    ScriptLanguage language = ScriptLanguage::JavaScript;
    if (!ScriptNodeLanguage (node->nodeType, language)) {
        result.error = "'" + nodeId + "' is not a script node";
        return result;
    }

    std::string path = StoredText (*node, kScriptPathParameter);

    /*
     * ⚠️ THE MIGRATION OFF THE FILE MODEL, AND IT RUNS BEFORE THE LOAD. A saved
     * graph from before the folder model points `scriptPath` at `offset.py`;
     * this converts it to `offset\main.py` and repoints the node. It happens
     * HERE rather than in a one-shot upgrade pass because a graph is not the
     * only way a stale path arrives - the library, an import, someone typing an
     * old path from memory - and every one of those routes ends at a reload.
     *
     * A path that is already a folder, or that names nothing, is left exactly
     * alone: MigrateScriptFileToFolder reports that as ok-with-nothing-done, so
     * the common case costs one stat and changes no state.
     */
    std::string migrationError;
    if (!path.empty ()) {
        const ScriptMigration migration = MigrateScriptFileToFolder (path);
        if (!migration.ok) {
            // A migration that could not happen is REPORTED AND SURVIVED, not
            // fatal: the node still loads from the folder the path implies and
            // says why it could not be converted. A locked file must not make a
            // graph unopenable.
            //
            // It goes into the node's OWN load error rather than into
            // result.error, which means "this verb could not run" and fails the
            // command. This did run; the node simply has something to report,
            // and the panel is where the user is looking.
            migrationError = migration.error;
        }
        else if (migration.migrated) {
            // Through the ordinary parameter edit, so the repoint is validated,
            // revisioned and undoable exactly like the user typing it.
            SetParameterEdit pointed;
            pointed.nodeId = nodeId;
            pointed.parameterId = kScriptPathParameter;
            pointed.value = Value (migration.folder);
            const EditResult repointed = GraphRuntimeState::Get ().Apply (graphId, GraphEdit { pointed });
            if (repointed.accepted) {
                result.migratedFrom = path;
                result.migratedTo = migration.folder;
                path = migration.folder;
            }
        }
    }

    ScriptState state;
    if (path.empty ()) {
        // Not an error to reload: it is the state of a node that has just been
        // placed, and the node reports it perfectly well itself.
        state.language = language;
        state.loadError = "this script node has no folder yet; choose or create one";
    }
    else {
        state = LoadScriptState (path, language);
    }
    // A migration that failed outranks whatever the load then found: the file is
    // not where the node expects it BECAUSE the conversion did not happen, and
    // "no main.py in offset\" would send the user looking for the wrong problem.
    if (!migrationError.empty ())
        state.loadError = migrationError;
    ScriptStore::Get ().SetState (nodeId, state);
    SyncScriptWatchList ();

    // ⚠️ RE-READ AFTER THE MIGRATION. `node` came from a snapshot taken before
    // the repoint edit above, and the port comparison below is against ITS
    // dynamic ports. Those do not change under a repoint - but reading through a
    // pointer into a superseded document is the kind of thing that stays correct
    // only until somebody adds a second edit here.
    const GraphDocument current = GraphRuntimeState::Get ().Document (graphId);
    const Node* reloaded = current.FindNode (nodeId);
    if (reloaded == nullptr) {
        result.error = "unknown node: " + nodeId;
        return result;
    }
    node = reloaded;

    SetScriptInterfaceEdit edit;
    edit.nodeId = nodeId;
    edit.sourceHash = state.sourceHash;
    if (state.manifest.Ok () && !state.source.empty ()) {
        edit.inputs = state.manifest.inputs;
        edit.outputs = state.manifest.outputs;
        edit.defaults = state.manifest.defaults;
        result.interfaceChanged =
            !SamePorts (node->dynamicInputs, edit.inputs) || !SamePorts (node->dynamicOutputs, edit.outputs);
    }
    else {
        // The ports the node already has, carried through unchanged. The edit is
        // still applied, because the hash moved and the node must re-run to
        // report its new problem rather than serve a cached success.
        edit.inputs = node->dynamicInputs;
        edit.outputs = node->dynamicOutputs;
    }

    const EditResult applied = GraphRuntimeState::Get ().Apply (graphId, GraphEdit { edit });
    if (!applied.accepted) {
        result.error = applied.error;
        return result;
    }

    result.ok = true;
    result.state = std::move (state);
    result.droppedEdges = applied.droppedEdges;
    return result;
}

std::vector<NodeId> ReloadStaleScriptNodes (const GraphId& graphId)
{
    std::vector<NodeId> reloaded;
    // RefreshDiskStamps answers for every node the store knows, across every
    // graph. Filtering to this document is what keeps a reload of one graph from
    // reshaping nodes in another that nobody asked about.
    const GraphDocument document = GraphRuntimeState::Get ().Document (graphId);
    for (const NodeId& nodeId : ScriptStore::Get ().RefreshDiskStamps ()) {
        if (document.FindNode (nodeId) == nullptr)
            continue;
        if (ReloadScriptNode (graphId, nodeId).ok)
            reloaded.push_back (nodeId);
    }
    return reloaded;
}

void SyncScriptWatchList ()
{
    IScriptWatcher* watcher = ActiveScriptWatcher ();
    if (watcher == nullptr)
        return; // No watcher is a supported state - see ScriptSource.hpp.
    std::vector<std::string> paths = ScriptStore::Get ().WatchedPaths ();
    std::sort (paths.begin (), paths.end ());
    paths.erase (std::unique (paths.begin (), paths.end ()), paths.end ());
    watcher->WatchPaths (paths);
}

void AdoptScriptNodes (const GraphId& graphId)
{
    const GraphDocument document = GraphRuntimeState::Get ().Document (graphId);
    for (const auto& [nodeId, node] : document.Nodes ()) {
        if (!IsScriptNodeType (node.nodeType))
            continue;
        // Only nodes the store has never heard of. Re-reading one that is already
        // loaded would undo a reload that has not been evaluated yet, and would
        // make opening the library panel silently reshape the open graph.
        if (ScriptStore::Get ().Knows (nodeId))
            continue;
        ReloadScriptNode (graphId, nodeId);
    }
    SyncScriptWatchList ();
}

} // namespace evp::nodegraph
