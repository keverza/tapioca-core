#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/NodeGraphScriptCommands.hpp"

#include "NativeCommands/NodeGraphCommandSupport.hpp"

#include "NodeGraph/ScriptNodes.hpp"
#include "NodeGraph/ScriptReload.hpp"
#include "NodeGraph/ScriptSource.hpp"

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
// Seven, and the shape of the set is the design:
//
//   GraphScriptStatus   what does this node's file look like right now
//   GraphScriptReload   read it again and reshape the node
//   GraphScriptCreate   scaffold a NEW file and point the node at it
//   GraphScriptRead     hand the source text to the embedded editor
//   GraphScriptWrite    save an editor buffer back, guarded by its base hash
//   GraphScriptOpen     hand the file to whatever the user edits code in
//   GraphScriptReveal   show the file in Explorer, selected in its folder
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

constexpr const char kStatusResponseSchema[] =
    R"json({"type":"object","properties":{"ok":{"type":"boolean"},"error":{"type":"string"},"graphId":{"type":"string"},"nodeId":{"type":"string"},"path":{"type":"string"},"language":{"type":"string"},"exists":{"type":"boolean"},"stale":{"type":"boolean"},"watching":{"type":"boolean"},"loadedAtMs":{"type":"integer"},"modifiedAtMs":{"type":"integer"},"sizeBytes":{"type":"integer","minimum":0},"loadError":{"type":"string"},"name":{"type":"string"},"description":{"type":"string"},"inputs":{"type":"array"},"outputs":{"type":"array"},"diagnostics":{"type":"array"},"log":{"type":"array","items":{"type":"string"}},"droppedEdges":{"type":"array"},"interfaceChanged":{"type":"boolean"},"revision":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["ok","error","graphId","nodeId","path","language","exists","stale","watching","loadedAtMs","modifiedAtMs","sizeBytes","loadError","name","description","inputs","outputs","diagnostics","log","droppedEdges","interfaceChanged","revision"]})json";

constexpr const char kWriteInputSchema[] =
    R"json({"type":"object","properties":{"graphId":{"type":"string","minLength":1},"nodeId":{"type":"string","minLength":1},"source":{"type":"string"},"baseHash":{"type":"string"}},"additionalProperties":false,"required":["nodeId","source","baseHash"]})json";

constexpr const char kReadResponseSchema[] =
    R"json({"type":"object","properties":{"ok":{"type":"boolean"},"error":{"type":"string"},"path":{"type":"string"},"language":{"type":"string"},"exists":{"type":"boolean"},"source":{"type":"string"},"sourceHash":{"type":"string"},"modifiedAtMs":{"type":"integer"},"sizeBytes":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["ok","error","path","language","exists","source","sourceHash","modifiedAtMs","sizeBytes"]})json";

constexpr const char kWriteResponseSchema[] =
    R"json({"type":"object","properties":{"ok":{"type":"boolean"},"error":{"type":"string"},"conflict":{"type":"boolean"},"path":{"type":"string"},"sourceHash":{"type":"string"},"diskSource":{"type":"string"},"modifiedAtMs":{"type":"integer"},"sizeBytes":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["ok","error","conflict","path","sourceHash","diskSource","modifiedAtMs","sizeBytes"]})json";

constexpr const char kOpenResponseSchema[] =
    R"json({"type":"object","properties":{"ok":{"type":"boolean"},"error":{"type":"string"},"path":{"type":"string"}},"additionalProperties":false,"required":["ok","error","path"]})json";

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
                              const std::vector<graph::Edge>& droppedEdges, bool interfaceChanged)
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
        return EncodeStatus (graphId, nodeId, result.state, true, {}, result.droppedEdges, result.interfaceChanged);
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

        std::string error;
        if (!graph::WriteScriptTemplate (path, error))
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
        const graph::ScriptRead read = graph::ReadScript (state.path);

        GS::ObjectState response;
        // ⚠️ A FILE THAT WOULD NOT READ IS A REPORTED OUTCOME, NOT A FAILED
        // COMMAND. It is the ordinary state of a node whose path is half typed,
        // and the editor draws it as an empty buffer with a reason rather than as
        // a bridge error thrown over the whole panel.
        response.Add ("ok", read.ok);
        response.Add ("error", GraphText (read.error));
        response.Add ("path", GraphText (state.path));
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

        const graph::ScriptState state = graph::ScriptStore::Get ().State (nodeId);
        const graph::ScriptWrite written = graph::WriteScriptSource (
            state.path, GraphUtf8 (sourceText), GraphUtf8 (baseHashText), &graph::HashScriptSource);

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

class GraphScriptOpenCommand : public GateFreeGraphCommand {
  protected:
    NativeCommandResult ExecuteGraph (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        graph::NodeId nodeId;
        if (!ReadNodeId (params, nodeId))
            return NativeCommandResult::Failure (GS::UniString ("nodeId is required", CC_UTF8));

        const graph::ScriptState state = graph::ScriptStore::Get ().State (nodeId);
        if (state.path.empty ())
            return NativeCommandResult::Failure (GS::UniString ("this script node has no file yet", CC_UTF8));
        if (!graph::StatScript (state.path).exists)
            return NativeCommandResult::Failure (GraphText ("no file at " + state.path));

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
            return NativeCommandResult::Failure (GS::UniString ("this script node has no file yet", CC_UTF8));

        const GS::UniString wide = GraphText (state.path);
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

const NativeCommandRegistration registrations[] = {
    { "GraphScriptStatus", &MakeRegisteredNativeCommand<GraphScriptStatusCommand>, false, kNodeInputSchema,
      kStatusResponseSchema },
    { "GraphScriptReload", &MakeRegisteredNativeCommand<GraphScriptReloadCommand>, false, kNodeInputSchema,
      kStatusResponseSchema },
    { "GraphScriptCreate", &MakeRegisteredNativeCommand<GraphScriptCreateCommand>, false, kCreateInputSchema,
      kStatusResponseSchema },
    { "GraphScriptRead", &MakeRegisteredNativeCommand<GraphScriptReadCommand>, false, kNodeInputSchema,
      kReadResponseSchema },
    { "GraphScriptWrite", &MakeRegisteredNativeCommand<GraphScriptWriteCommand>, false, kWriteInputSchema,
      kWriteResponseSchema },
    { "GraphScriptOpen", &MakeRegisteredNativeCommand<GraphScriptOpenCommand>, false, kNodeInputSchema,
      kOpenResponseSchema },
    { "GraphScriptReveal", &MakeRegisteredNativeCommand<GraphScriptRevealCommand>, false, kNodeInputSchema,
      kOpenResponseSchema },
};

} // namespace

NativeCommandRegistrations GetNodeGraphScriptCommandRegistrations ()
{
    return MakeRegistrationView (registrations);
}

} // namespace geomsrv
