#include "NodeGraph/ScriptReload.hpp"

#include "NodeGraph/GraphEdit.hpp"
#include "NodeGraph/GraphRuntimeState.hpp"
#include "NodeGraph/ScriptSource.hpp"

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
    if (node == nullptr) {
        result.error = "unknown node: " + nodeId;
        return result;
    }
    ScriptLanguage language = ScriptLanguage::JavaScript;
    if (!ScriptNodeLanguage (node->nodeType, language)) {
        result.error = "'" + nodeId + "' is not a script node";
        return result;
    }

    const std::string path = StoredText (*node, kScriptPathParameter);

    ScriptState state;
    if (path.empty ()) {
        // Not an error to reload: it is the state of a node that has just been
        // placed, and the node reports it perfectly well itself.
        state.language = language;
        state.loadError = "this script node has no file yet; choose or create one";
    }
    else {
        state = LoadScriptState (path, language);
    }
    ScriptStore::Get ().SetState (nodeId, state);
    SyncScriptWatchList ();

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
