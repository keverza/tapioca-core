#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/NodeGraphScriptCommands.hpp"

#include "NativeCommands/NodeGraphCommandSupport.hpp"

#include "NodeGraph/ScriptNodes.hpp"
#include "NodeGraph/ScriptReload.hpp"
#include "NodeGraph/ScriptSource.hpp"
#include "NodeGraph/ScriptIntelligence.hpp"
#include "NodeGraph/ScriptWorkspace.hpp"

// ShellExecuteW. Not reached through the SDK prelude, and this is the only file
// in the command tree that hands a path to the shell.
#include <shellapi.h>
// SHParseDisplayName / SHOpenFolderAndSelectItems. See GraphScriptRevealCommand
// for why the folder is opened through item IDs rather than a command line.
#include <shlobj.h>

#include <string>

namespace geomsrv {
namespace {

// ---------------------------------------------------------------------------
// The script node's verbs.
//
// Eleven, and the shape of the set is the design:
//
//   GraphScriptStatus      what does this node's folder look like right now
//   GraphScriptReload      read it again and reshape the node
//   GraphScriptCreate      scaffold a NEW node folder and point the node at it
//   GraphScriptRead        hand one file's source to the embedded editor
//   GraphScriptWrite       save an editor buffer back, guarded by its base hash
//   GraphScriptAddFile     create one empty helper inside the node's folder
//   GraphScriptOpen        hand the folder to whatever the user edits code in
//   GraphScriptReveal      show the folder in Explorer
//   GraphScriptLibrary     what is in the workflow library, for the picker
//   GraphScriptSetName     rewrite the entry file's @name when the node is renamed
//   GraphScriptOpenLibrary show the workflow library itself in Explorer
//
// ⚠️ A NODE IS A FOLDER, AND EVERY VERB THAT NAMES A FILE NAMES IT RELATIVELY.
// Read, Write and AddFile take a `file` that is a plain name inside the node's
// own folder, or `libs/<name>` in the shared library, and nothing else -
// ScriptWorkspace::ResolveWorkspaceFile does the refusing. These names arrive
// from a browser, so the absence of an absolute path in this API is the point:
// there is no spelling of "read C:\Windows\..." for the browser to send.
//
// ⚠️ GraphScriptWrite IS GUARDED, AND THE GUARD IS THE WHOLE REASON IT MAY
// EXIST. A script node's file is normally open in VSCode at the same time. The
// verb takes the hash the buffer was READ at and refuses when disk no longer
// matches it, returning the other version instead of applying the write - so the
// two editors cannot silently overwrite one another, in either direction. There
// is no unguarded overwrite verb, and adding one would undo this.
//
// ⚠️ AND STATUS IS STILL NOT A SIDE CHANNEL FOR THE SOURCE TEXT. It reports the
// path, the runtime, the stamps, the parsed ports, the diagnostics and the last
// run's log - everything needed to DRAW the node - and never its contents. The
// source crosses the bridge only through Read, which is asked for once when a
// buffer is opened, so the polling that every script node does forever stays as
// cheap as it was before there was an editor.
// ---------------------------------------------------------------------------

constexpr const char kNodeInputSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1},"nodeId":{"type":"string","minLength":1}},"additionalProperties":false,"required":["nodeId"]})json";

constexpr const char kCreateInputSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1},"nodeId":{"type":"string","minLength":1},"path":{"type":"string","minLength":1}},"additionalProperties":false,"required":["nodeId","path"]})json";

// The verbs that name one file inside the node's folder. `file` is optional and
// defaults to the entry file, so the editor can open a node without first
// learning whether its entry is main.py or main.js.
constexpr const char kFileInputSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1},"nodeId":{"type":"string","minLength":1},"file":{"type":"string"}},"additionalProperties":false,"required":["nodeId"]})json";

constexpr const char kAddFileInputSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1},"nodeId":{"type":"string","minLength":1},"file":{"type":"string","minLength":1}},"additionalProperties":false,"required":["nodeId","file"]})json";

constexpr const char kStatusResponseSchema[] =
    R"json({"type":"object","properties":{"ok":{"type":"boolean"},"error":{"type":"string"},"graphId":{"type":"string"},"nodeId":{"type":"string"},"path":{"type":"string"},"language":{"type":"string"},"exists":{"type":"boolean"},"stale":{"type":"boolean"},"watching":{"type":"boolean"},"loadedAtMs":{"type":"integer"},"modifiedAtMs":{"type":"integer"},"sizeBytes":{"type":"integer","minimum":0},"loadError":{"type":"string"},"name":{"type":"string"},"description":{"type":"string"},"inputs":{"type":"array"},"outputs":{"type":"array"},"diagnostics":{"type":"array"},"log":{"type":"array","items":{"type":"string"}},"droppedEdges":{"type":"array"},"interfaceChanged":{"type":"boolean"},"revision":{"type":"integer","minimum":0},"workspaceRoot":{"type":"string"},"entryFile":{"type":"string"},"files":{"type":"array"},"importRoots":{"type":"array","items":{"type":"string"}},"migratedFrom":{"type":"string"}},"additionalProperties":false,"required":["ok","error","graphId","nodeId","path","language","exists","stale","watching","loadedAtMs","modifiedAtMs","sizeBytes","loadError","name","description","inputs","outputs","diagnostics","log","droppedEdges","interfaceChanged","revision","workspaceRoot","entryFile","files","importRoots","migratedFrom"]})json";

constexpr const char kWriteInputSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1},"nodeId":{"type":"string","minLength":1},"file":{"type":"string"},"source":{"type":"string"},"baseHash":{"type":"string"}},"additionalProperties":false,"required":["nodeId","source","baseHash"]})json";

constexpr const char kReadResponseSchema[] =
    R"json({"type":"object","properties":{"ok":{"type":"boolean"},"error":{"type":"string"},"path":{"type":"string"},"file":{"type":"string"},"shared":{"type":"boolean"},"language":{"type":"string"},"exists":{"type":"boolean"},"source":{"type":"string"},"sourceHash":{"type":"string"},"modifiedAtMs":{"type":"integer"},"sizeBytes":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["ok","error","path","file","shared","language","exists","source","sourceHash","modifiedAtMs","sizeBytes"]})json";

constexpr const char kWriteResponseSchema[] =
    R"json({"type":"object","properties":{"ok":{"type":"boolean"},"error":{"type":"string"},"conflict":{"type":"boolean"},"path":{"type":"string"},"file":{"type":"string"},"sourceHash":{"type":"string"},"diskSource":{"type":"string"},"modifiedAtMs":{"type":"integer"},"sizeBytes":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["ok","error","conflict","path","file","sourceHash","diskSource","modifiedAtMs","sizeBytes"]})json";

constexpr const char kOpenResponseSchema[] =
    R"json({"type":"object","properties":{"ok":{"type":"boolean"},"error":{"type":"string"},"path":{"type":"string"}},"additionalProperties":false,"required":["ok","error","path"]})json";

// The alias verb. `name` may legitimately be empty - clearing a nickname puts the
// node back to being named by its folder - so it carries no minLength.
constexpr const char kSetNameInputSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1},"nodeId":{"type":"string","minLength":1},"name":{"type":"string"}},"additionalProperties":false,"required":["nodeId","name"]})json";

// Opening the library takes nothing at all: there is one workflow library per
// machine, and which node asked has no bearing on where it is.
constexpr const char kEmptyInputSchema[] = R"json({"type":"object","properties":{},"additionalProperties":false})json";
// A completion request. `line` and `character` are zero-based and `character` is
// a UTF-16 code-unit offset - which is what a JavaScript string index already is,
// so the browser sends CodeMirror's own column unconverted.
constexpr const char kCompleteInputSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1},"nodeId":{"type":"string","minLength":1},"file":{"type":"string"},"source":{"type":"string"},"line":{"type":"integer","minimum":0},"character":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["nodeId","source","line","character"]})json";

constexpr const char kCompleteResponseSchema[] =
    R"json({"type":"object","properties":{"ok":{"type":"boolean"},"error":{"type":"string"},"items":{"type":"array","items":{"type":"object","properties":{"label":{"type":"string"},"insertText":{"type":"string"},"kind":{"type":"string"},"detail":{"type":"string"},"documentation":{"type":"string"}},"additionalProperties":false,"required":["label","insertText","kind","detail","documentation"]}}},"additionalProperties":false,"required":["ok","error","items"]})json";

// The language server's availability, and the on-demand install that changes it.
constexpr const char kIntelligenceResponseSchema[] =
    R"json({"type":"object","properties":{"ok":{"type":"boolean"},"error":{"type":"string"},"state":{"type":"string"},"message":{"type":"string"},"executable":{"type":"string"}},"additionalProperties":false,"required":["ok","error","state","message","executable"]})json";

// What the script picker draws from. `template` and `suggestedName` are here
// rather than in a verb of their own because they are asked for at exactly the
// same moment - the palette opens a node that has no folder yet - and a second
// round trip for two strings the same call already knows is a second round trip.
constexpr const char kLibraryResponseSchema[] =
    R"json({"type":"object","properties":{"ok":{"type":"boolean"},"error":{"type":"string"},"root":{"type":"string"},"language":{"type":"string"},"entries":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"title":{"type":"string"},"hasEntry":{"type":"boolean"},"fileCount":{"type":"integer","minimum":0},"modifiedAtMs":{"type":"integer"}},"additionalProperties":false,"required":["name","title","hasEntry","fileCount","modifiedAtMs"]}},"suggestedName":{"type":"string"},"template":{"type":"string"}},"additionalProperties":false,"required":["ok","error","root","language","entries","suggestedName","template"]})json";

GS::Array<GS::ObjectState> EncodePorts (const std::vector<graph::PortSchema>& ports, bool asInput)
{
    GS::Array<GS::ObjectState> encoded;
    for (const graph::PortSchema& port : ports) {
        GS::ObjectState state;
        state.Add ("portId", GraphText (port.id));
        state.Add ("label", GraphText (port.label));
        state.Add ("valueType", GraphValueTypeName (port.valueType));
        // The header's own word for the type, alongside the wire's. The editor
        // shows the user what they typed in their file rather than translating it
        // back - "number" is what the header says, and being shown "double"
        // beside it is a small, constant puzzle.
        state.Add ("typeWord", GS::UniString (graph::ScriptValueTypeWord (port.valueType), CC_UTF8));
        if (asInput)
            state.Add ("required", port.required);
        encoded.Push (std::move (state));
    }
    return encoded;
}

// Fills every field of the status response from one node's state. Shared by
// status and reload so the two cannot disagree about what a node looks like -
// the editor applies a reload's response directly, and a reload that reported a
// different shape than the next status would flicker.
GS::ObjectState EncodeStatus (const graph::GraphId& graphId, const graph::NodeId& nodeId,
                              const graph::ScriptState& state, bool ok, const std::string& error,
                              const std::vector<graph::Edge>& droppedEdges, bool interfaceChanged,
                              const std::string& migratedFrom = {})
{
    GS::ObjectState response;
    response.Add ("ok", ok);
    response.Add ("error", GraphText (error));
    response.Add ("graphId", GraphText (graphId));
    response.Add ("nodeId", GraphText (nodeId));
    response.Add ("path", GraphText (state.path));
    response.Add ("language", GS::UniString (graph::ScriptLanguageName (state.language), CC_UTF8));
    response.Add ("exists", state.diskStamp.exists);
    response.Add ("stale", state.IsStale ());
    // Reported rather than assumed, because it changes what the node should
    // promise the user. Without a watcher the node reloads on evaluation and on
    // Reload, and it should say so instead of implying a save will be noticed.
    response.Add ("watching", graph::ActiveScriptWatcher () != nullptr);
    response.Add ("loadedAtMs", static_cast<GS::Int64> (state.loadedStamp.modifiedUnixMs));
    response.Add ("modifiedAtMs", static_cast<GS::Int64> (state.diskStamp.modifiedUnixMs));
    response.Add ("sizeBytes", static_cast<GS::Int64> (state.diskStamp.sizeBytes));
    response.Add ("loadError", GraphText (state.loadError));
    response.Add ("name", GraphText (state.manifest.name));
    response.Add ("description", GraphText (state.manifest.description));
    response.Add ("inputs", EncodePorts (state.manifest.inputs, true));
    response.Add ("outputs", EncodePorts (state.manifest.outputs, false));

    GS::Array<GS::ObjectState> diagnostics;
    for (const graph::ScriptDiagnostic& diagnostic : state.manifest.diagnostics) {
        GS::ObjectState encoded;
        encoded.Add ("line", static_cast<GS::Int64> (diagnostic.line));
        encoded.Add ("message", GraphText (diagnostic.message));
        diagnostics.Push (std::move (encoded));
    }
    response.Add ("diagnostics", diagnostics);

    GS::Array<GS::UniString> log;
    for (const std::string& line : state.lastLog)
        log.Push (GraphText (line));
    response.Add ("log", log);

    GS::Array<GS::ObjectState> dropped;
    for (const graph::Edge& edge : droppedEdges) {
        GS::ObjectState encoded;
        encoded.Add ("sourceNode", GraphText (edge.sourceNode));
        encoded.Add ("sourcePort", GraphText (edge.sourcePort));
        encoded.Add ("targetNode", GraphText (edge.targetNode));
        encoded.Add ("targetPort", GraphText (edge.targetPort));
        dropped.Push (std::move (encoded));
    }
    response.Add ("droppedEdges", dropped);
    response.Add ("interfaceChanged", interfaceChanged);

    // The folder model's own fields. `files` is what the editor draws tabs from,
    // and it is reported by the SAME verb as everything else so a node's tab
    // strip cannot disagree with the node it belongs to.
    response.Add ("workspaceRoot", GraphText (state.path));
    response.Add ("entryFile", GraphText (state.entryFile));

    GS::Array<GS::ObjectState> files;
    for (const graph::WorkspaceFile& file : state.files) {
        GS::ObjectState encoded;
        encoded.Add ("name", GraphText (file.name));
        encoded.Add ("entry", file.entry);
        encoded.Add ("shared", file.shared);
        encoded.Add ("sizeBytes", static_cast<GS::Int64> (file.sizeBytes));
        files.Push (std::move (encoded));
    }
    response.Add ("files", files);

    GS::Array<GS::UniString> importRoots;
    for (const std::string& root : state.importRoots)
        importRoots.Push (GraphText (root));
    response.Add ("importRoots", importRoots);

    // Non-empty only when THIS call converted a single-file node into a folder.
    // Reported because it moved the user's file: the graph looks unchanged and
    // the filesystem does not, and the editor they had it open in still points
    // at where it used to be.
    response.Add ("migratedFrom", GraphText (migratedFrom));
    response.Add ("revision", static_cast<GS::Int64> (graph::GraphRuntimeState::Get ().Document (graphId).Revision ()));
    return response;
}

bool ReadNodeId (const GS::ObjectState& params, graph::NodeId& nodeId)
{
    GS::UniString text;
    if (!params.Get ("nodeId", text) || text.IsEmpty ())
        return false;
    nodeId = GraphUtf8 (text);
    return true;
}

class GraphScriptStatusCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        graph::NodeId nodeId;
        if (!ReadNodeId (params, nodeId))
            return NativeCommandResult::Failure (GS::UniString ("nodeId is required", CC_UTF8));
        const graph::GraphId graphId = ReadGraphIdParam (params);

        // Re-stat before answering. The status verb is what the editor polls
        // when there is no watcher, so an answer computed from a cached stamp
        // would report "not stale" forever on exactly the machines that need it
        // most.
        graph::ScriptStore::Get ().RefreshDiskStamps ();
        return EncodeStatus (graphId, nodeId, graph::ScriptStore::Get ().State (nodeId), true, {}, {}, false);
    }
};

class GraphScriptReloadCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        graph::NodeId nodeId;
        if (!ReadNodeId (params, nodeId))
            return NativeCommandResult::Failure (GS::UniString ("nodeId is required", CC_UTF8));
        const graph::GraphId graphId = ReadGraphIdParam (params);

        const graph::ScriptReloadResult result = graph::ReloadScriptNode (graphId, nodeId);
        // A file that would not load is a REPORTED outcome, not a failed command:
        // it is the ordinary state of a script halfway through being written, and
        // the editor draws it on the node. Only a caller mistake - an unknown
        // node, a node that is not a script node - fails the verb.
        if (!result.ok && !result.error.empty ())
            return NativeCommandResult::Failure (GraphText (result.error));
        return EncodeStatus (graphId, nodeId, result.state, true, {}, result.droppedEdges, result.interfaceChanged,
                             result.migratedFrom);
    }
};

class GraphScriptCreateCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        graph::NodeId nodeId;
        GS::UniString pathText;
        if (!ReadNodeId (params, nodeId) || !params.Get ("path", pathText) || pathText.IsEmpty ())
            return NativeCommandResult::Failure (GS::UniString ("nodeId and path are required", CC_UTF8));
        const graph::GraphId graphId = ReadGraphIdParam (params);
        const std::string path = GraphUtf8 (pathText);

        // The folder, not a file. `path` may be relative - a bare
        // `apartment_metrics` scaffolds inside the workflow library, which is
        // what makes that location a preset rather than something the user has
        // to spell out every time.
        //
        // ⚠️ THE DOCUMENT IS BOUND TO A NAMED LOCAL, AND IT HAS TO BE.
        // `GraphRuntimeState::Document` returns a COPY - it takes the slot's
        // mutex and hands back a snapshot - so `Document(id).FindNode(id)` in one
        // expression returns a pointer INTO A TEMPORARY that is destroyed at the
        // semicolon. Reading `placed->nodeType` after that is reading freed
        // memory: it produced a garbage node type, which failed the language
        // check, which reported a perfectly ordinary Python node as "not a script
        // node" and made Create unusable. Never dereference a pointer obtained
        // from a by-value accessor's temporary.
        const graph::GraphDocument document = graph::GraphRuntimeState::Get ().Document (graphId);
        const graph::Node* placed = document.FindNode (nodeId);
        graph::ScriptLanguage language = graph::ScriptLanguage::Python;
        if (placed == nullptr || !graph::ScriptNodeLanguage (placed->nodeType, language))
            return NativeCommandResult::Failure (GraphText ("'" + nodeId + "' is not a script node"));

        const graph::ScriptWorkspace workspace = graph::ResolveScriptWorkspace (path, language);
        std::string error;
        if (!graph::WriteWorkspaceTemplate (workspace, error))
            return NativeCommandResult::Failure (GraphText (error));

        // Point the node at what was just written, through the ordinary parameter
        // edit - so it is validated, revisioned and undoable exactly like the
        // user typing the path in themselves.
        graph::SetParameterEdit pointed;
        pointed.nodeId = nodeId;
        pointed.parameterId = graph::kScriptPathParameter;
        pointed.value = graph::Value (path);
        const graph::EditResult applied =
            graph::GraphRuntimeState::Get ().Apply (graphId, graph::GraphEdit { pointed });
        if (!applied.accepted)
            return NativeCommandResult::Failure (GraphText (applied.error));

        const graph::ScriptReloadResult result = graph::ReloadScriptNode (graphId, nodeId);
        return EncodeStatus (graphId, nodeId, result.state, true, result.error, result.droppedEdges,
                             result.interfaceChanged);
    }
};

// ---------------------------------------------------------------------------
// The two verbs the embedded editor is made of.
//
// They are a pair and they only work as one: Read hands out the text TOGETHER
// WITH the hash of exactly those bytes, and Write will not apply a buffer whose
// base hash has stopped matching disk. Neither is safe without the other, and
// dropping the hash from either of them is what would make a silent overwrite
// possible.

class GraphScriptReadCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        graph::NodeId nodeId;
        if (!ReadNodeId (params, nodeId))
            return NativeCommandResult::Failure (GS::UniString ("nodeId is required", CC_UTF8));

        const graph::ScriptState state = graph::ScriptStore::Get ().State (nodeId);
        GS::UniString requested;
        params.Get ("file", requested);
        const std::string file = GraphUtf8 (requested);

        // ⚠️ THE ONLY WAY A NAME BECOMES A PATH. An absolute path, a `..`, a
        // nested folder or a file of the wrong language is refused here, and
        // there is no other route from this verb to the filesystem.
        const graph::ScriptWorkspace workspace = graph::ResolveScriptWorkspace (state.path, state.language);
        std::string absolute;
        std::string refusal;
        if (!graph::ResolveWorkspaceFile (workspace, file, absolute, refusal)) {
            GS::ObjectState refused;
            refused.Add ("ok", false);
            refused.Add ("error", GraphText (refusal));
            refused.Add ("path", GraphText (state.path));
            refused.Add ("file", GraphText (file));
            refused.Add ("shared", false);
            refused.Add ("language", GS::UniString (graph::ScriptLanguageName (state.language), CC_UTF8));
            refused.Add ("exists", false);
            refused.Add ("source", GS::UniString ());
            refused.Add ("sourceHash", GS::UniString ());
            refused.Add ("modifiedAtMs", static_cast<GS::Int64> (0));
            refused.Add ("sizeBytes", static_cast<GS::Int64> (0));
            return refused;
        }
        const graph::ScriptRead read = graph::ReadScript (absolute);

        GS::ObjectState response;
        // ⚠️ A FILE THAT WOULD NOT READ IS A REPORTED OUTCOME, NOT A FAILED
        // COMMAND. It is the ordinary state of a node whose path is half typed,
        // and the editor draws it as an empty buffer with a reason rather than as
        // a bridge error thrown over the whole panel.
        response.Add ("ok", read.ok);
        response.Add ("error", GraphText (read.error));
        response.Add ("path", GraphText (state.path));
        // Echoed back so a response that arrives after the user switched tabs can
        // be discarded rather than painted into the wrong buffer.
        response.Add ("file", GraphText (file));
        response.Add ("shared", file.rfind ("libs/", 0) == 0);
        response.Add ("language", GS::UniString (graph::ScriptLanguageName (state.language), CC_UTF8));
        response.Add ("exists", read.stamp.exists);
        response.Add ("source", GraphText (read.source));
        // The hash of the bytes IN THIS RESPONSE, computed here rather than taken
        // from the node's loaded state: the node may still be running an older
        // load, and a buffer guarded by the hash of text it was never shown would
        // refuse every save for a reason nobody could see.
        response.Add ("sourceHash", GraphText (read.ok ? graph::HashScriptSource (read.source) : std::string {}));
        response.Add ("modifiedAtMs", static_cast<GS::Int64> (read.stamp.modifiedUnixMs));
        response.Add ("sizeBytes", static_cast<GS::Int64> (read.stamp.sizeBytes));
        return response;
    }
};

class GraphScriptWriteCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        graph::NodeId nodeId;
        GS::UniString sourceText;
        GS::UniString baseHashText;
        if (!ReadNodeId (params, nodeId))
            return NativeCommandResult::Failure (GS::UniString ("nodeId is required", CC_UTF8));
        // Both are required by the schema, and `source` may legitimately be
        // empty - a user who selected everything and deleted it is saving an
        // empty file, which is a save and not a mistake.
        params.Get ("source", sourceText);
        params.Get ("baseHash", baseHashText);
        GS::UniString requested;
        params.Get ("file", requested);
        const std::string file = GraphUtf8 (requested);

        const graph::ScriptState state = graph::ScriptStore::Get ().State (nodeId);
        const graph::ScriptWorkspace workspace = graph::ResolveScriptWorkspace (state.path, state.language);
        std::string absolute;
        std::string refusal;
        if (!graph::ResolveWorkspaceFile (workspace, file, absolute, refusal))
            return NativeCommandResult::Failure (GraphText (refusal));

        const graph::ScriptWrite written = graph::WriteScriptSource (
            absolute, GraphUtf8 (sourceText), GraphUtf8 (baseHashText), &graph::HashScriptSource);

        // A conflict is an ANSWER, not a failure: the editor has a choice to
        // offer and needs the other version to offer it with. Only a caller
        // mistake or a filesystem that refused the write fails the verb.
        if (!written.ok && !written.conflict)
            return NativeCommandResult::Failure (GraphText (written.error));

        GS::ObjectState response;
        response.Add ("ok", written.ok);
        response.Add ("error", GraphText (written.error));
        response.Add ("conflict", written.conflict);
        response.Add ("path", GraphText (state.path));
        response.Add ("file", GraphText (file));
        response.Add ("sourceHash", GraphText (written.diskHash));
        response.Add ("diskSource", GraphText (written.diskSource));
        response.Add ("modifiedAtMs", static_cast<GS::Int64> (written.stamp.modifiedUnixMs));
        response.Add ("sizeBytes", static_cast<GS::Int64> (written.stamp.sizeBytes));
        // ⚠️ NO RELOAD HERE, ON PURPOSE. The caller reloads through the existing
        // verb, which is the one place that reshapes ports and reports dropped
        // edges - a second path into that would be a second set of rules for what
        // a save does to a node's interface.
        return response;
    }
};

class GraphScriptAddFileCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        graph::NodeId nodeId;
        GS::UniString requested;
        if (!ReadNodeId (params, nodeId) || !params.Get ("file", requested) || requested.IsEmpty ())
            return NativeCommandResult::Failure (GS::UniString ("nodeId and file are required", CC_UTF8));

        const graph::ScriptState state = graph::ScriptStore::Get ().State (nodeId);
        const graph::ScriptWorkspace workspace = graph::ResolveScriptWorkspace (state.path, state.language);
        const std::string file = GraphUtf8 (requested);

        // ⚠️ AN EMPTY FILE, NOT A SECOND COPY OF THE TEMPLATE. A helper that
        // arrived carrying `@in` / `@out` directives would declare ports from a
        // file that is not the entry point - which parses as nothing and reads as
        // the editor having done something inexplicable.
        std::string absolute;
        std::string error;
        if (!graph::CreateWorkspaceFile (workspace, file, absolute, error))
            return NativeCommandResult::Failure (GraphText (error));

        GS::ObjectState response;
        response.Add ("ok", true);
        response.Add ("error", GS::UniString ());
        response.Add ("path", GraphText (absolute));
        return response;
    }
};

class GraphScriptOpenCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        graph::NodeId nodeId;
        if (!ReadNodeId (params, nodeId))
            return NativeCommandResult::Failure (GS::UniString ("nodeId is required", CC_UTF8));

        const graph::ScriptState state = graph::ScriptStore::Get ().State (nodeId);
        if (state.path.empty ())
            return NativeCommandResult::Failure (GS::UniString ("this script node has no folder yet", CC_UTF8));
        // The FOLDER, so the editor opens the node as a project and sees its
        // helpers - which is the whole reason an external editor is still the
        // primary one. ShellExecute on a directory opens it in Explorer or in
        // whatever is registered for a folder.
        if (graph::StatScript (state.entryFile).exists == false)
            return NativeCommandResult::Failure (
                GraphText ("no " + std::string (graph::EntryFileName (state.language)) + " in " + state.path));

        // ⚠️ THE SHELL'S DEFAULT HANDLER, NOT A CONFIGURED EDITOR PATH. Whichever
        // of VSCode, Sublime or something else the user has associated with .py
        // and .js is the one they want, it is already correct on their machine,
        // and an editor path setting here would be a second thing to keep right
        // that is wrong by default on every machine but the author's.
        //
        // The path is passed as a file to ShellExecute, never composed into a
        // command line: it comes from a text field the user typed, and a path
        // with a quote in it must not be able to become an argument.
        const GS::UniString wide = GraphText (state.path);
        const HINSTANCE launched = ShellExecuteW (nullptr, L"open", reinterpret_cast<LPCWSTR> (wide.ToUStr ().Get ()),
                                                  nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR> (launched) <= 32)
            return NativeCommandResult::Failure (
                GraphText ("Windows would not open " + state.path + "; no program is associated with that file type"));

        GS::ObjectState response;
        response.Add ("ok", true);
        response.Add ("error", GS::UniString ());
        response.Add ("path", GraphText (state.path));
        return response;
    }
};

class GraphScriptRevealCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        graph::NodeId nodeId;
        if (!ReadNodeId (params, nodeId))
            return NativeCommandResult::Failure (GS::UniString ("nodeId is required", CC_UTF8));

        const graph::ScriptState state = graph::ScriptStore::Get ().State (nodeId);
        if (state.path.empty ())
            return NativeCommandResult::Failure (GS::UniString ("this script node has no folder yet", CC_UTF8));

        const GS::UniString wide = GraphText (state.entryFile);
        const LPCWSTR native = reinterpret_cast<LPCWSTR> (wide.ToUStr ().Get ());

        // ⚠️ ITEM IDS, NOT A COMMAND LINE, AND THAT IS THE WHOLE REASON THIS IS
        // NOT ShellExecute. The obvious implementation is
        // `explorer.exe /select,"<path>"` - and that path is a string the USER
        // typed into a text field on the node. A path containing a quote closes
        // the argument early and everything after it becomes further arguments to
        // a process this add-on is launching. SHParseDisplayName takes the path
        // as data and never parses it as a command, so there is no quoting rule
        // to get right and nothing to escape.
        //
        // A file that has since been deleted simply fails to parse, which is
        // reported rather than silently opening the wrong folder.
        PIDLIST_ABSOLUTE item = nullptr;
        const HRESULT parsed = SHParseDisplayName (native, nullptr, &item, 0, nullptr);
        if (FAILED (parsed) || item == nullptr)
            return NativeCommandResult::Failure (GraphText ("no file at " + state.path));

        // The shell wants an apartment-threaded COM context. Archicad's main
        // thread already has one, and this verb is gate-free, so it may arrive on
        // another - hence the explicit initialise/uninitialise pair. RPC_E_CHANGED_MODE
        // means somebody already initialised this thread differently, which is
        // fine to proceed under and must NOT be balanced by a CoUninitialize.
        const HRESULT com = CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const HRESULT shown = SHOpenFolderAndSelectItems (item, 0, nullptr, 0);
        if (SUCCEEDED (com))
            CoUninitialize ();
        CoTaskMemFree (item);

        if (FAILED (shown))
            return NativeCommandResult::Failure (GraphText ("Windows would not show " + state.path));

        GS::ObjectState response;
        response.Add ("ok", true);
        response.Add ("error", GS::UniString ());
        response.Add ("path", GraphText (state.path));
        return response;
    }
};

class GraphScriptLibraryCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        graph::NodeId nodeId;
        if (!ReadNodeId (params, nodeId))
            return NativeCommandResult::Failure (GS::UniString ("nodeId is required", CC_UTF8));
        const graph::GraphId graphId = ReadGraphIdParam (params);

        // The LANGUAGE comes from the node type, so a Python node is never
        // offered a folder whose only entry is main.js. Read from the DOCUMENT
        // rather than from the script store, so a node placed a second ago -
        // which has no stored script state at all yet - still gets a listing.
        //
        // The document is a named local because `Document` returns a COPY; see
        // GraphScriptCreateCommand for what a pointer into that temporary does.
        const graph::GraphDocument document = graph::GraphRuntimeState::Get ().Document (graphId);
        const graph::Node* placed = document.FindNode (nodeId);
        graph::ScriptLanguage language = graph::ScriptLanguage::Python;
        if (placed == nullptr || !graph::ScriptNodeLanguage (placed->nodeType, language))
            return NativeCommandResult::Failure (GraphText ("'" + nodeId + "' is not a script node"));

        GS::Array<GS::ObjectState> encoded;
        for (const graph::WorkflowEntry& entry : graph::ListWorkflowLibrary (language)) {
            GS::ObjectState row;
            row.Add ("name", GraphText (entry.name));
            row.Add ("title", GraphText (entry.title));
            row.Add ("hasEntry", entry.hasEntry);
            row.Add ("fileCount", static_cast<GS::Int64> (entry.fileCount));
            row.Add ("modifiedAtMs", static_cast<GS::Int64> (entry.modifiedUnixMs));
            encoded.Push (std::move (row));
        }

        GS::ObjectState response;
        response.Add ("ok", true);
        response.Add ("error", GS::UniString ());
        response.Add ("root", GraphText (graph::DefaultWorkflowRoot ()));
        response.Add ("language", GS::UniString (graph::ScriptLanguageName (language), CC_UTF8));
        response.Add ("entries", encoded);
        // ⚠️ THE NAME AND THE TEMPLATE TRAVEL WITH THE LISTING, BECAUSE A SCRIPT
        // NODE IS SCAFFOLDED ON ITS FIRST SAVE AND NOT ON PLACEMENT. The palette
        // opens a brand-new node on this text with nothing on disk behind it, and
        // writes it under this name when the user saves. Both are answered
        // natively so that neither the starter script nor the naming rule exists
        // twice - a second copy in the browser bundle is a copy that drifts.
        response.Add ("suggestedName", GraphText (graph::NextFreeWorkflowName ("script")));
        response.Add ("template", GraphText (graph::ScriptTemplateSource (language)));
        return response;
    }
};

class GraphScriptSetNameCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        graph::NodeId nodeId;
        GS::UniString nameText;
        if (!ReadNodeId (params, nodeId) || !params.Get ("name", nameText))
            return NativeCommandResult::Failure (GS::UniString ("nodeId and name are required", CC_UTF8));
        const graph::GraphId graphId = ReadGraphIdParam (params);

        const graph::ScriptState state = graph::ScriptStore::Get ().State (nodeId);
        if (state.path.empty ())
            return NativeCommandResult::Failure (GS::UniString ("this script node has no folder yet", CC_UTF8));

        const graph::ScriptWorkspace workspace = graph::ResolveScriptWorkspace (state.path, state.language);
        if (!workspace.ok)
            return NativeCommandResult::Failure (GraphText (workspace.error));

        // ⚠️ READ, TRANSFORM, WRITE UNDER THE HASH JUST READ. The alias lives in
        // the entry file's header, and that file is normally also open in VSCode.
        // Guarding the write with the hash of the bytes this command itself read
        // means a rename cannot silently overwrite an edit made a second earlier
        // in the other editor: it fails instead, and the node keeps its old
        // alias, which is the recoverable half of the two outcomes.
        const graph::ScriptRead read = graph::ReadScript (workspace.entryFile);
        if (!read.ok)
            return NativeCommandResult::Failure (
                GraphText (read.error.empty () ? "could not read " + workspace.entryFile : read.error));

        const std::string renamed = graph::WithScriptName (read.source, GraphUtf8 (nameText), state.language);
        if (renamed != read.source) {
            const graph::ScriptWrite written = graph::WriteScriptSource (
                workspace.entryFile, renamed, graph::HashScriptSource (read.source), &graph::HashScriptSource);
            if (!written.ok)
                return NativeCommandResult::Failure (GraphText (
                    written.conflict ? "the file changed while the node was being renamed; reload and try again"
                                     : written.error));
        }

        // Reload rather than patch: the header is what declares this node's name,
        // and there is one path by which a file changes a node.
        const graph::ScriptReloadResult result = graph::ReloadScriptNode (graphId, nodeId);
        return EncodeStatus (graphId, nodeId, result.state, true, result.error, result.droppedEdges,
                             result.interfaceChanged);
    }
};

class GraphScriptOpenLibraryCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        const std::string root = graph::DefaultWorkflowRoot ();
        if (root.empty ())
            return NativeCommandResult::Failure (
                GS::UniString ("%LOCALAPPDATA% is not set, so there is no workflow library on this machine", CC_UTF8));

        // ⚠️ CREATED IF ABSENT, AND THAT IS NOT SCOPE CREEP. A machine that has
        // never had a workflow deployed has no such folder, and "the button did
        // nothing" is the worst possible answer to "show me where my scripts go".
        // An empty directory is also exactly what someone needs in order to put
        // the first script there by hand.
        std::string created;
        if (!graph::EnsureWorkflowRoot (created))
            return NativeCommandResult::Failure (GraphText ("could not create " + root));

        // ShellExecute on a DIRECTORY, with the path passed as data rather than
        // composed into a command line - see GraphScriptRevealCommand for why no
        // verb in this file ever builds an explorer.exe argument string.
        const GS::UniString wide = GraphText (root);
        const HINSTANCE launched = ShellExecuteW (nullptr, L"open", reinterpret_cast<LPCWSTR> (wide.ToUStr ().Get ()),
                                                  nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR> (launched) <= 32)
            return NativeCommandResult::Failure (GraphText ("Windows would not open " + root));

        GS::ObjectState response;
        response.Add ("ok", true);
        response.Add ("error", GS::UniString ());
        response.Add ("path", GraphText (root));
        return response;
    }
};

// ---------------------------------------------------------------------------
// Code intelligence.
//
// ⚠️ THREE VERBS, AND THE SPLIT IS THE DESIGN. Completion is asked for on every
// keystroke after a dot and must be cheap and gate-free; the status is polled by
// a panel; the install is a minute of network that returns immediately and is
// watched through the status. Folding them into one verb would put a download
// behind something the editor calls while somebody is typing.
//
// ⚠️ AND THE BROWSER NEVER SPEAKS LSP. It sends a position and a buffer and gets
// back labels - see ScriptCompletion. Relaying raw JSON-RPC would be less code
// here and would hand a browser inside Archicad the ability to ask a language
// server to read any file on the machine; the whole reason the script verbs name
// files relatively is that a name from a browser must not be able to become an
// arbitrary path.

class GraphScriptCompleteCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        graph::NodeId nodeId;
        if (!ReadNodeId (params, nodeId))
            return NativeCommandResult::Failure (GS::UniString ("nodeId is required", CC_UTF8));

        GS::UniString sourceText;
        params.Get ("source", sourceText);
        GS::UniString requested;
        params.Get ("file", requested);
        Int32 line = 0;
        Int32 character = 0;
        params.Get ("line", line);
        params.Get ("character", character);

        const graph::ScriptState state = graph::ScriptStore::Get ().State (nodeId);
        const graph::ScriptWorkspace workspace = graph::ResolveScriptWorkspace (state.path, state.language);

        // ⚠️ THE SAME RESOLVER EVERY OTHER FILE VERB USES. `file` arrives from a
        // browser, and completion is not a reason to let it name a path the read
        // and write verbs would refuse.
        std::string absolute;
        std::string refusal;
        const std::string file = GraphUtf8 (requested);
        if (!graph::ResolveWorkspaceFile (workspace, file, absolute, refusal))
            return NativeCommandResult::Failure (GraphText (refusal));

        // The entry file, when the editor did not name one - the same default the
        // read verb applies, so a node opens without the browser first learning
        // whether its entry is main.py or main.js.
        const std::string name = file.empty () ? std::string (graph::EntryFileName (workspace.language)) : file;

        std::string error;
        const std::vector<graph::ScriptCompletion> completions = graph::ScriptIntelligence::Get ().Complete (
            workspace, name, GraphUtf8 (sourceText), static_cast<int> (line), static_cast<int> (character), error);

        GS::Array<GS::ObjectState> encoded;
        for (const graph::ScriptCompletion& completion : completions) {
            GS::ObjectState item;
            item.Add ("label", GraphText (completion.label));
            item.Add ("insertText", GraphText (completion.insertText));
            item.Add ("kind", GraphText (completion.kind));
            item.Add ("detail", GraphText (completion.detail));
            item.Add ("documentation", GraphText (completion.documentation));
            encoded.Push (std::move (item));
        }

        GS::ObjectState response;
        // ⚠️ A SERVER THAT COULD NOT ANSWER IS ok:false WITH AN EMPTY LIST, NOT A
        // FAILED COMMAND. This is called while somebody is typing; a bridge error
        // thrown over the panel on every keystroke because the server is still
        // starting would be far worse than a menu that does not appear.
        response.Add ("ok", error.empty ());
        response.Add ("error", GraphText (error));
        response.Add ("items", encoded);
        return response;
    }
};

GS::ObjectState EncodeIntelligenceStatus ()
{
    const graph::IntelligenceStatus status = graph::ScriptIntelligence::Get ().Status ();
    const char* state = "notInstalled";
    switch (status.state) {
        case graph::IntelligenceState::NotInstalled:
            state = "notInstalled";
            break;
        case graph::IntelligenceState::Installing:
            state = "installing";
            break;
        case graph::IntelligenceState::Ready:
            state = "ready";
            break;
        case graph::IntelligenceState::Failed:
            state = "failed";
            break;
    }
    GS::ObjectState response;
    response.Add ("ok", true);
    response.Add ("error", GS::UniString ());
    response.Add ("state", GS::UniString (state, CC_UTF8));
    response.Add ("message", GraphText (status.message));
    response.Add ("executable", GraphText (status.executable));
    return response;
}

class GraphScriptIntelligenceCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        return EncodeIntelligenceStatus ();
    }
};

class GraphScriptInstallIntelligenceCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        // ⚠️ RETURNS IMMEDIATELY, AND THE STATUS IS HOW PROGRESS IS WATCHED. The
        // install is ~56 MB over the network into the runtime's site-packages;
        // holding a native verb open for that would block a worker thread for a
        // minute and time out in the bridge long before it finished.
        graph::ScriptIntelligence::Get ().BeginInstall ();
        return EncodeIntelligenceStatus ();
    }
};

const NativeCommandRegistration registrations[] = {
    { "GraphScriptStatus", &MakeRegisteredNativeCommand<GraphScriptStatusCommand>, false, kNodeInputSchema,
      kStatusResponseSchema },
    { "GraphScriptReload", &MakeRegisteredNativeCommand<GraphScriptReloadCommand>, false, kNodeInputSchema,
      kStatusResponseSchema },
    { "GraphScriptCreate", &MakeRegisteredNativeCommand<GraphScriptCreateCommand>, false, kCreateInputSchema,
      kStatusResponseSchema },
    { "GraphScriptRead", &MakeRegisteredNativeCommand<GraphScriptReadCommand>, false, kFileInputSchema,
      kReadResponseSchema },
    { "GraphScriptAddFile", &MakeRegisteredNativeCommand<GraphScriptAddFileCommand>, false, kAddFileInputSchema,
      kOpenResponseSchema },
    { "GraphScriptWrite", &MakeRegisteredNativeCommand<GraphScriptWriteCommand>, false, kWriteInputSchema,
      kWriteResponseSchema },
    { "GraphScriptOpen", &MakeRegisteredNativeCommand<GraphScriptOpenCommand>, false, kNodeInputSchema,
      kOpenResponseSchema },
    { "GraphScriptReveal", &MakeRegisteredNativeCommand<GraphScriptRevealCommand>, false, kNodeInputSchema,
      kOpenResponseSchema },
    { "GraphScriptLibrary", &MakeRegisteredNativeCommand<GraphScriptLibraryCommand>, false, kNodeInputSchema,
      kLibraryResponseSchema },
    { "GraphScriptSetName", &MakeRegisteredNativeCommand<GraphScriptSetNameCommand>, false, kSetNameInputSchema,
      kStatusResponseSchema },
    { "GraphScriptOpenLibrary", &MakeRegisteredNativeCommand<GraphScriptOpenLibraryCommand>, false, kEmptyInputSchema,
      kOpenResponseSchema },
    { "GraphScriptComplete", &MakeRegisteredNativeCommand<GraphScriptCompleteCommand>, false, kCompleteInputSchema,
      kCompleteResponseSchema },
    { "GraphScriptIntelligence", &MakeRegisteredNativeCommand<GraphScriptIntelligenceCommand>, false, kEmptyInputSchema,
      kIntelligenceResponseSchema },
    { "GraphScriptInstallIntelligence", &MakeRegisteredNativeCommand<GraphScriptInstallIntelligenceCommand>, false,
      kEmptyInputSchema, kIntelligenceResponseSchema },
};

} // namespace

NativeCommandRegistrations GetNodeGraphScriptCommandRegistrations ()
{
    return MakeRegistrationView (registrations);
}

} // namespace geomsrv
