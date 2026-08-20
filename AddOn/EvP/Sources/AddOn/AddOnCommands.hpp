#ifndef GEOMETRYSERVER_ADDONCOMMANDS_HPP
#define GEOMETRYSERVER_ADDONCOMMANDS_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "ObjectState.hpp"
#include "NativeCommands/NativeCommandResult.hpp"

// Archicad JSON API "Add-On Commands" — the CONTROL PLANE.
//
// Why these exist: ACAPI work (geometry extraction, screenshots) is main-thread
// only, but Archicad's main event loop is BLOCKED while a Python palette script
// runs — verified empirically: ACAPI_AddOnAddOnCommunication_CallFromEventLoop
// posts fine but is never dispatched, so that bridge times out from the palette.
//
// The channel Archicad DOES pump during a palette script is its JSON API command
// queue (this is how Tapir performs main-thread work while the user keeps using
// Archicad). So anything needing the main thread is exposed here, with
// ScheduleForExecutionOnMainThread, and invoked from Python via Archicad's JSON
// port (the one the palette injects as --port).
//
// Everything that does NOT need the main thread (all geometry queries) stays on
// our own HTTP server, served from worker threads against the cached snapshot.
namespace geomsrv {

// Install the JSON commands. Call from Initialize().
GSErrCode InstallAddOnCommands ();

// In-process dispatch of the SAME command implementations for `evp.api.call`
// (Layer 1, P1). The native commands already speak GS::ObjectState, so the
// Python bus routes to them rather than to a parallel set — one implementation,
// one schema, reachable both over Archicad's JSON port and in-process.
//
// ⚠️ MAIN THREAD ONLY: these are the ScheduleForExecutionOnMainThread commands,
// called here without Archicad's scheduler, so the caller must already be on the
// main thread. evp::MainThreadGate guarantees that.
//
// ⚠️ NO UNDO SCOPE IS OPENED HERE. A write command's Execute is the bare core —
// it must never wrap itself, because ACAPI_CallUndoableCommand cannot nest
// (APIERR_REFUSEDCMD) and that would bar it from transactions. The caller opens
// exactly one scope: a transaction around the whole batch, or the dispatcher
// around a single immediate write. Ask IsWriteCommand which is needed.
//
// Native execution has its own status and failure-kind channel. Dispatch and
// transaction rollback never inspect response data to decide whether a call
// succeeded, and schema failures retain their classification through dispatch.
struct NativeCommandInfo {
    GS::String                  name;
    bool                        isWrite = false;
    bool                        isStructural = false;
    bool                        needsMainThread = true;
    bool                        cancelExempt = false;
    GS::Optional<GS::UniString> inputSchema;
    GS::Optional<GS::UniString> responseSchema;
};

struct NativeSchemaReport {
    UInt32                     total = 0;
    UInt32                     complete = 0;
    GS::Array<GS::UniString>   missingInput;
    GS::Array<GS::UniString>   missingResponse;
};

GS::Array<NativeCommandInfo> EnumerateNativeCommands ();
NativeSchemaReport GetNativeSchemaReport ();
bool ValidateNativeCommandCatalog (GS::UniString& error);
bool IsCancelExemptNativeCommand (const GS::String& name);

NativeCommandResult ExecuteNativeCommand (const GS::String&      name,
                                          const GS::ObjectState& params);

// Resolve a single project-info field: an EXACT match on its database key first,
// then a case-insensitive substring match on its UI description. The key is the
// identity (ACAPI_AutoText_SetAnAutoText writes by `autotextDbKey`); the
// description is display data and need not be unique, so the key path is what
// settles an ambiguous case. The description path remains because that is what a
// command's source spells out (`default_from="project:Sklypo plotas"`). Reads the
// API_ProjectInfo autotexts via ACAPI_AutoText_GetAutoTexts (NOT the Property
// API). MAIN THREAD ONLY. Returns true with `value` set to the matching field's
// string; false if nothing matches or the ACAPI read fails.
bool ProjectInfoField (const GS::UniString& needleText, GS::UniString& value);

// Every project-info field as the three parallel columns the evp.ProjectField
// picker needs: the DESCRIPTION it shows, the KEY it sends to the command, and the
// VALUE — which the picker reads only to decide whether a field is worth offering
// (evp.ProjectField(numeric=True) hides the ones no number can be read out of).
// Same scan as ProjectInfoField above, so a key listed here always resolves there.
// MAIN THREAD ONLY. Returns false if the ACAPI read fails; empty lists are a valid
// answer (a project need not define any field).
bool ProjectInfoFieldChoices (GS::Array<GS::UniString>& descriptions,
                              GS::Array<GS::UniString>& keys,
                              GS::Array<GS::UniString>& values);

// Does `name` modify the project (and therefore need an undo scope)?
// Returns false if the command is unknown.
bool IsWriteCommand (const GS::String& name, bool& isWrite);

// Does `name` modify the project's DATA STRUCTURE, and therefore need NO undo
// scope — indeed refuse to run inside one? See StructuralCommand in
// NativeCommands/CommandBase.hpp: these ACAPI calls are documented non-undoable
// data-structure modifiers that return APIERR_REFUSEDCMD from inside a scope.
// The transaction path asks this so it can refuse such a step by NAME instead of
// letting the batch fail on an undecodable error code. Returns false if the
// command is unknown.
bool IsStructuralCommand (const GS::String& name, bool& isStructural);

// False for the geometry QUERY commands, which read only the immutable snapshot
// and its mutex-guarded index — no ACAPI, so no main-thread hop. The dispatcher
// runs those inline: at ~3ms per hop, marshalling a heatmap's worth of rays would
// take minutes for nothing.
bool CommandNeedsMainThread (const GS::String& name, bool& needsMainThread);

} // namespace geomsrv

#endif
