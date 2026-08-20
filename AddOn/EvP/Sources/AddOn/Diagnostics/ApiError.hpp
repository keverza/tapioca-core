#ifndef EVP_DIAGNOSTICS_APIERROR_HPP
#define EVP_DIAGNOSTICS_APIERROR_HPP

#include "UniString.hpp"
#include "Definitions.hpp"
#include "Array.hpp"

// The API error trail — one place that turns a bare GSErrCode into something a
// human can act on, and one file it all lands in.
//
// WHY THIS EXISTS. Every ACAPI call reports failure the same way: a GSErrCode.
// Archicad keeps no failure log of its own, so a command that only prints the
// number ("ACAPI_Element_Create failed (-2130313098)") throws away everything
// that would have identified the bug — which symbolic code it was, what it
// means, which call site raised it, what the command was doing, and which bus
// call the user had triggered. Re-deriving that by hand has cost whole sessions.
// So: decode the code, name the site, carry the call, write it down.
//
// THREE LAYERS, USE THE ONE THAT FITS:
//
//   1. Decode only        DescribeErr (err)
//      "APIERR_IRREGULARPOLY (-2130313098) - The passed polygon or polyline is
//      irregular." Pure, no side effects, no log. Use inside a message you are
//      building yourself.
//
//   2. Report an ACAPI failure       EVP_ACAPI_FAIL ("ACAPI_Element_Create", err, ctx)
//      The normal case. Returns the decoded message to put in the response's
//      "error" AND appends a full block to logs\api_errors.log carrying the
//      site, the in-flight bus call and its params. `ctx` is optional and is
//      where you say what the call was ATTEMPTING — the one thing the code
//      cannot recover afterwards ("roof, 12 outline pts, story 2").
//
//   3. Report a logical failure      EVP_FAIL ("structure must be basic|composite", ctx)
//      No GSErrCode involved (bad params, a name that did not resolve). Same
//      trail, so a run's failures read in one place and in order.
//
// THE CALL SCOPE. ApiDispatcher opens an evp::CallScope around every bus call,
// so layers 2 and 3 stamp each entry with the call_id, the command name and the
// params JSON without any command having to pass them down. It is thread-local:
// the worker thread's scope and a main-thread job see their own, which is what
// makes the trail readable when a script fires calls back to back.
//
// ACAPI-FREE ON PURPOSE. This header pulls in GSRoot and the DevKit's error-code
// ENUM only — never ACAPI.h — so it can be included by NativeCommands, the
// dispatcher and the palette alike without dragging the API surface behind it.
// Keep it that way.

namespace evp {

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

// "APIERR_BADPARS" for a code in the table, "NoError" for 0, or nullptr when the
// code is not one we know by name. Never returns an invented symbol: an
// AI-plausible name for a code that does not exist is exactly the failure mode
// this file is meant to end.
const char* ErrCodeName (GSErrCode err);

// The DevKit's own one-line description of the code (copied verbatim from
// Support/Inc/APIdefs_ErrorCodes.h and GSRoot/Definitions.hpp), or nullptr.
const char* ErrCodeMeaning (GSErrCode err);

// The full human form, always non-empty:
//   known    "APIERR_BADPARS (-2130313112) - The passed parameters are inconsistent."
//   unknown  "unknown error -2130313999 (module 262 'API', sub-code 999)"
// An unknown code still gets its module and sub-code split out, because that is
// enough to find it in the DevKit headers by hand.
GS::UniString DescribeErr (GSErrCode err);

// ---------------------------------------------------------------------------
// Where it happened
// ---------------------------------------------------------------------------

// A call site. Built by the EVP_SITE macro so the file/line/function are the
// compiler's, not a string someone has to keep in sync. (Line numbers are
// forbidden in DOCS because they rot; in a log entry they are the whole point —
// the log is written and read within one run of one build.)
struct Site {
    const char* file     = nullptr;
    int         line     = 0;
    const char* function = nullptr;
};

#define EVP_SITE (evp::Site { __FILE__, __LINE__, __FUNCTION__ })

// ---------------------------------------------------------------------------
// Which bus call is in flight (thread-local)
// ---------------------------------------------------------------------------

// RAII. ApiDispatcher opens one per call; anything that fails inside is stamped
// with it. Nesting is allowed and the innermost wins — a transaction replay
// opens a step scope inside the CommitTransaction scope, so a failing step
// reports itself rather than the batch.
class CallScope {
public:
    CallScope (const GS::UniString& callId, const GS::UniString& command,
               const GS::UniString& paramsJson);
    ~CallScope ();

    CallScope (const CallScope&)            = delete;
    CallScope& operator= (const CallScope&) = delete;
};

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

// Overloads exist so EVP_ACAPI_FAIL's context argument can be omitted; do not
// call Ctx directly.
inline GS::UniString Ctx ()                          { return GS::UniString (); }
inline GS::UniString Ctx (const GS::UniString& text) { return text; }

// Appends a block to logs\api_errors.log and RETURNS the compact message to put
// in the command's response. `err` must not be NoError (a success is not a
// failure to report — the caller checks first, as it already does today).
GS::UniString ReportApiFailure (const Site& site, const char* acapiFunction,
                                GSErrCode err, const GS::UniString& context);

// Same trail, no GSErrCode: a refused parameter, a name that did not resolve.
GS::UniString ReportFailure (const Site& site, const GS::UniString& message,
                             const GS::UniString& context);

#define EVP_ACAPI_FAIL(acapiFunction, err, ...) \
    evp::ReportApiFailure (EVP_SITE, acapiFunction, err, evp::Ctx (__VA_ARGS__))

#define EVP_FAIL(message, ...) \
    evp::ReportFailure (EVP_SITE, message, evp::Ctx (__VA_ARGS__))

// The dispatcher's hook: record a failing ENVELOPE (the outcome the script
// actually sees). Logged even when the underlying failure was already reported
// by a command, because the two answer different questions — "what broke" vs
// "what did the script get back" — and a GateTimeout or a BadParams has no
// command behind it at all.
void LogEnvelopeFailure (const GS::UniString& command, const GS::UniString& callId,
                         const GS::UniString& code, const GS::UniString& message,
                         const GS::UniString& detail, const GS::UniString& paramsJson);

// %LOCALAPPDATA%\EvP\logs\api_errors.log — empty if %LOCALAPPDATA% is unset.
GS::UniString ApiErrorLogPath ();

// The last few failures this session, newest last, one line each. The palette
// appends these to a failed run's status so the user is told where to look
// without opening anything, and EvP.GetErrorTrail hands them to a script.
GS::Array<GS::UniString> RecentFailures (UInt32 maxCount);

// How many failures have been reported since the add-on loaded. A probe that
// expects exactly one failure can assert on the delta.
UInt64 FailureCount ();

// RecentFailures pre-formatted as an indented block for commands.log, ending
// with the path to the full trail. Empty when nothing has failed, so a caller
// can append it unconditionally without leaving a "no errors" line on every
// successful run.
GS::UniString FailureTrailBlock (UInt32 maxCount = 8);

}   // namespace evp

#endif
