#ifndef EVP_NATIVECOMMANDS_COMMANDBASE_HPP
#define EVP_NATIVECOMMANDS_COMMANDBASE_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ObjectState.hpp"
// GSRoot's GS::ProcessControl (progress + TestBreak/cancel) — NOT GSModelDevLib's
// ProcessControl.hpp, which only forward-declares a different class of the same name.
#include "GSProcessControl.hpp"
// EVP_ACAPI_FAIL / EVP_FAIL — how a command reports a failure. Here rather than
// per-domain because EVERY domain file needs it and the alternative is that some
// of them keep printing bare error numbers. It is the one exception the "keep
// this header tiny" rule below tolerates, and it earns it by dragging in nothing
// (GSRoot + the DevKit's error ENUM; no ACAPI, no store, no engine).
#include "Diagnostics/ApiError.hpp"
#include "NativeCommands/CommandSchemas.hpp"
#include "NativeCommands/NativeCommandResult.hpp"

// The two bases every native command derives from. Kept deliberately tiny: this
// header is included by every NativeCommands/*.cpp, so anything that drags in a
// store or an engine belongs in that domain's own file, not here.
//
// The registration/dispatch side (MakeCommand, ExecuteNativeCommand, …) lives in
// NativeCommands/CommandRegistry.cpp; the public facade stays AddOnCommands.hpp.
namespace geomsrv {

// Shared boilerplate: every command runs on the main thread.
class MainThreadCommand : public API_AddOnCommand {
public:
    GS::String GetNamespace () const override { return "Tapioca"; }

    GS::Optional<GS::UniString> GetSchemaDefinitions () const override
    {
        return GetNativeSchemaDefinitions ();
    }
    GS::Optional<GS::UniString> GetInputParametersSchema () const override
    {
        return GetNativeInputSchema (GetName ());
    }
    GS::Optional<GS::UniString> GetResponseSchema () const override
    {
        return GetNativeResponseSchema (GetName ());
    }

    API_AddOnCommandExecutionPolicy GetExecutionPolicy () const override
    {
        // Archicad's JSON command queue IS pumped while a Python palette script
        // runs (unlike CallFromEventLoop). This is what makes ACAPI reachable
        // on demand from Python.
        return API_AddOnCommandExecutionPolicy::ScheduleForExecutionOnMainThread;
    }

    // Fast commands stay silent. Long ones override this to true so Archicad shows
    // its native progress window — which also gives the user a Cancel button.
    bool IsProcessWindowVisible () const override { return false; }
    void OnResponseValidationFailed (const GS::ObjectState&) const override {}

    GS::ObjectState Execute (const GS::ObjectState& params, GS::ProcessControl& processControl) const final
    {
        NativeCommandResult result = ExecuteNative (params, processControl);
        if (result.ok)
            return std::move (result.data);

        // Archicad's separate JSON-port interface has no native failure channel.
        // In-process Tapioca calls use ExecuteNative and never see this payload.
        GS::ObjectState failure;
        failure.Add ("ok", false);
        failure.Add ("error", result.error);
        return failure;
    }

    virtual NativeCommandResult ExecuteNative (const GS::ObjectState& params,
                                                GS::ProcessControl& processControl) const = 0;

    // Does Execute modify the project? Writes need an undo scope; reads must NOT
    // get one (an undoable command that changes nothing still costs the user an
    // undo step).
    virtual bool IsWrite () const { return false; }

    // Does Execute modify the project's DATA STRUCTURE rather than its elements —
    // and therefore must run with NO undo scope open? See StructuralCommand.
    // Distinct from IsWrite because the two are mutually exclusive: a structural
    // command changes the project (so it is not a read) but an undo scope makes
    // it FAIL (so it cannot be a write).
    virtual bool IsStructural () const { return false; }

    // Does Execute touch ACAPI at all? Almost everything does, and must be
    // marshalled to the main thread. The geometry QUERIES do not: they read an
    // immutable snapshot through a mutex-guarded store, exactly like the zero-copy
    // buffer API. Marshalling those would cost ~3ms per call for nothing — fatal
    // for a heatmap firing thousands of rays. Overriding this to false is a
    // promise that Execute calls NO ACAPI.
    virtual bool NeedsMainThread () const { return true; }
};

// Base for every command that modifies the project.
//
// THE RULE (learned the hard way, P1): a write command must NEVER open its own
// undo scope. ACAPI_CallUndoableCommand cannot nest — the SDK refuses it with
// APIERR_REFUSEDCMD ("the current command is undoable") — so a command that
// wraps itself can never take part in a transaction, and transactions are the
// only way to get one undo step and true atomicity for a multi-write script.
//
// So Execute() is the bare core and assumes a scope is ALREADY OPEN. The caller
// supplies exactly one:
//   * transaction  -> one ACAPI_CallUndoableCommand around the whole batch,
//                     so a mid-batch failure rolls everything back;
//   * immediate    -> the dispatcher wraps this single call.
// One implementation, one undo scope, supplied from outside. Reads never get one.
class WriteCommand : public MainThreadCommand {
public:
    bool IsWrite () const override { return true; }
};

// Base for a command that modifies the project's DATA STRUCTURE — creating or
// deleting a database (worksheet, detail, layout, master layout, 3D document),
// a layout, or a Layout Book subset.
//
// THE RULE IS THE EXACT INVERSE OF WriteCommand'S, which is why this is a third
// category and not a flag on that one. Every ACAPI call in this group is
// documented as a "non-undoable data structure modifier" and REFUSES to run
// inside an undo scope — ACAPI_Database_NewDatabase,
// ACAPI_Database_DeleteDatabase, ACAPI_Navigator_CreateLayout and
// ACAPI_Navigator_CreateSubSet each list APIERR_REFUSEDCMD ("called inside an
// undo scope" / "cannot be called from an opened undo session") in their OWN
// error tables in the AC29 headers. So:
//
//   * IsWrite() stays FALSE — not because nothing changes (plenty does) but
//     because the dispatcher's undo scope would make the call FAIL. This is the
//     one place where "not a write" does not mean "read-only".
//   * IsStructural() is TRUE, and that is what the transaction path branches on
//     to REFUSE the command with a real explanation instead of letting Archicad
//     return a bare APIERR_REFUSEDCMD from inside a batch nobody can debug.
//
// ⚠️ THE CONSEQUENCE THE USER FEELS: these changes are NOT UNDOABLE. Deleting a
// worksheet or a 3D document does not go on the undo stack, so Ctrl+Z will not
// bring it back. A command that deletes must therefore say what it is about to
// delete and require an explicit opt-in — see EvP.DeleteDatabase's `confirm`.
class StructuralCommand : public MainThreadCommand {
public:
    bool IsWrite ()      const override { return false; }
    bool IsStructural () const override { return true; }
};

} // namespace geomsrv

#endif
