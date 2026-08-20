#ifndef EVP_APIDISPATCHER_HPP
#define EVP_APIDISPATCHER_HPP

// Layer 1 — the explicit command bus. Every operation a script performs, of any
// kind, funnels through here as one envelope. Debuggability first: there are no
// private channels, so a Layer-1 trace always tells the whole truth, and any
// misbehaving wrapper reduces to a single reproducible `evp.api.call` line.
//
// Namespaces state the backend explicitly — no hidden routing:
//   Tapioca.* native add-on commands, in-process (canonical from API 2)
//   EvP.*     legacy spelling accepted during the API 2 migration
//   API.*    Archicad's official JSON interface, executed in-process via
//            ACAPI_Command_ExecuteJSONRequest — no HTTP layer, no port
//   Tapir.*  an installed Tapir, until absorbed          (P6)
//
// Callable from ANY thread: it marshals to the main thread internally via
// MainThreadGate, because every native command is main-thread-only.

#include "UniString.hpp"

namespace evp {

// One semver for the whole `evp` surface. Reported in every envelope's meta and
// checked against each command's `requires_api`. Native request/response schemas
// are additive within a major; breaking changes bump the major.
constexpr const char* ApiVersion = "2.0.0";

// Executes one call and returns the response ENVELOPE as JSON:
//   {"ok":bool, "data":{...}, "error":{"code","message","detail"},
//    "meta":{"backend","zone","duration_ms","main_thread_ms","api_version","call_id"}}
//
// Exactly one of data/error is present. GS::ObjectState cannot represent a JSON
// null, so the absent key IS the null — which reads identically in Python, where
// envelope.get("error") yields None.
//
// Never throws and never returns malformed JSON: a failure is an envelope with
// ok=false, so the Python side always has something structured to raise from.
//
// `zone` is stamped into meta and is the ONLY thing that differs between an
// in-process (Zone B) call and one arriving over HTTP from a `runtime="external"`
// subprocess (Zone C). Both take this identical path — same gate, same undo
// scoping, same transactions — which is what makes the two zones transparent to a
// script rather than merely similar.
GS::UniString DispatchApiCall (const GS::UniString& command, const GS::UniString& paramsJson,
                               const GS::UniString& zone = "embedded");

// Toggles Layer-1 tracing: every request/response envelope (with its call_id)
// goes to %LOCALAPPDATA%\EvP\logs\api_trace.log. This is the debugging story.
void SetApiTracing (bool enabled);
bool IsApiTracingEnabled ();

}   // namespace evp

#endif
