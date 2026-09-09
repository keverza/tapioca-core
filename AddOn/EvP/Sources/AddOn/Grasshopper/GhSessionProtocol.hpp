#ifndef EVP_GRASSHOPPER_GHSESSIONPROTOCOL_HPP
#define EVP_GRASSHOPPER_GHSESSIONPROTOCOL_HPP

// The session half of the bridge: load a definition, read its schema, set its
// inputs, solve it, and be told what happened. Message TYPES live in
// GhProtocol.hpp with all the others -- this is one protocol, not a second one,
// which is why the version moved rather than a new header being invented -- and
// their PAYLOAD codecs live here, the way GhPreviewProtocol.hpp holds preview's.
//
// Same rules as GhProtocol.hpp and for the same reasons: DevKit-free, Win32-free
// and CLR-free, fixed-size little-endian, every length checked against what
// actually arrived before anything is allocated or indexed.
//
// ⚠️ EVERY SESSION MESSAGE CARRIES {hostGeneration, sessionId, requestRevision}
// AND NONE OF THE THREE IS DECORATION. HANDOFF-GHHost.md §7:
//
//   * hostGeneration -- a worker restart invalidates every host-side session.
//     A result stamped with a generation that is no longer current is evidence,
//     never a publication: the process that produced it does not exist any more,
//     and neither does the document it read.
//   * sessionId -- the host owns the numbering, because the host owns the
//     revision store the results land in. A worker that invented its own would
//     be the only thing that knew what a result belonged to.
//   * requestRevision -- THE NEWEST REQUESTED REVISION WINS, and a completed
//     stale solve is collected for diagnostics and never published. This is a
//     counter on REQUESTS; accepted solutions are counted separately by
//     solutionRevision, because "what the user asked for" and "what was actually
//     produced" advance at different rates and conflating them loses the
//     ordering test entirely.
//
// ⚠️ THE ENVELOPE IS THE FIRST TWELVE BYTES OF EVERY PAYLOAD BELOW, NEVER AN
// AFTERTHOUGHT AT THE END. A message whose routing fields cannot be read until
// its variable-length tail has been parsed cannot be ROUTED before it is
// TRUSTED, and a stale-generation message is precisely one this host wants to
// drop before it parses any further.

#include "GhProtocol.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace evp {
namespace grasshopper {
namespace protocol {

// hostGeneration, sessionId, requestRevision.
constexpr uint32_t SessionEnvelopeSize = 12;

// Ceilings, all checked before an allocation. Generous rather than tuned: real
// budgets wait on the measurements HANDOFF-GHHost.md §14 requires, and these
// exist so that a corrupt length is a refusal instead of a reserve.
constexpr uint32_t MaxSessionTextBytes = 64u * 1024u;
constexpr uint32_t MaxDefinitionPathBytes = 4096;
constexpr uint32_t MaxSchemaBytes = 4u * 1024u * 1024u;
constexpr uint32_t MaxSessionInputs = 4096;
constexpr uint32_t MaxSessionOutputs = 4096;
constexpr uint32_t MaxSessionDiagnostics = 1024;

// Which surface owns the document. HANDOFF-GHHost.md §6.
enum class SessionMode : uint32_t {
    // No editor is loaded or shown; solves only on explicit input changes the
    // controller accepted.
    Headless = 0,
    // The editor owns document edits and its solution completions schedule a
    // settled preview publication.
    Authoring = 1,
};

// The workflow session state machine, HANDOFF-GHHost.md §6. Reported by the
// worker rather than inferred by the host: the host knows what it ASKED for,
// and those are different claims the moment anything fails.
enum class SessionState : uint32_t {
    Empty = 0,
    Loading = 1,
    Loaded = 2,
    Solving = 3,
    Published = 4,
    Invalid = 5,
    Cancelling = 6,
    Reloading = 7,
    // The worker cannot recover this session; only a host restart can. Distinct
    // from Invalid, which is a bad DEFINITION and is fixed by loading another.
    HostRestartRequired = 8,
};

// What a solve request wants collected. Bits rather than a mode word: preview
// and data are routinely both wanted, and commit is routinely wanted with
// neither -- an Apply that re-solves is the thing §7 exists to prevent.
enum SolveWants : uint32_t {
    SolveWantsNothing = 0,
    SolveWantsPreview = 1u << 0,
    SolveWantsData = 1u << 1,
    SolveWantsCommit = 1u << 2,
};

constexpr uint32_t SolveWantsAll = SolveWantsPreview | SolveWantsData | SolveWantsCommit;

// The stable failure categories of HANDOFF-GHHost.md §11. The panel shows one
// concise status per category; the diagnostics text beside it carries the
// specifics. Numbering is part of the wire and may only be appended to.
enum class FailureCode : uint32_t {
    None = 0,
    RuntimeMissing = 1,
    LicenceUnavailable = 2,
    RuntimeStartFailed = 3,
    DefinitionInvalid = 4,
    DependencyMissing = 5,
    ContractInvalid = 6,
    InputInvalid = 7,
    SolveFailed = 8,
    SolveTimedOut = 9,
    Cancelled = 10,
    HostDisconnected = 11,
    HostCrashed = 12,
    PreviewInvalid = 13,
    CommitInvalid = 14,
    StaleRevision = 15,
    AccessDenied = 16,
    CapacityExceeded = 17,
    ProtocolMismatch = 18,
    // No session by that id on the worker. Its own category because it is the
    // ordinary consequence of a restart race rather than a fault: the host sent
    // a request for a session the new worker generation never opened.
    SessionUnknown = 19,
};

// Mirrors GH_RuntimeMessageLevel's three reported levels. Blank is not carried:
// a message with no level is not a diagnostic.
enum class DiagnosticLevel : uint32_t {
    Error = 0,
    Warning = 1,
    Remark = 2,
};

// The routing prefix on every message in this file.
struct SessionEnvelope {
    uint32_t hostGeneration = 0;
    uint32_t sessionId = 0;
    uint32_t requestRevision = 0;
};

struct OpenSessionPayload {
    SessionEnvelope envelope;
    SessionMode mode = SessionMode::Headless;
};

struct CloseSessionPayload {
    SessionEnvelope envelope;
};

struct SetSessionModePayload {
    SessionEnvelope envelope;
    SessionMode mode = SessionMode::Headless;
};

struct LoadDefinitionPayload {
    SessionEnvelope envelope;
    // An absolute path the HOST has already checked against its trusted roots
    // (§12). The worker does not re-derive trust; it also does not assume the
    // check happened, which is why the path is length-capped here too.
    std::string path; // UTF-8
};

struct ReloadDefinitionPayload {
    SessionEnvelope envelope;
};

struct GetSchemaPayload {
    SessionEnvelope envelope;
};

// The worker's answer to anything that does not produce a schema or a solution:
// OpenSession, CloseSession, SetSessionMode, LoadDefinition, ReloadDefinition,
// SetInputs and a refused CancelSolve.
//
// ⚠️ IT CARRIES THE SESSION'S STATE, NOT JUST "OK". A host that only learns
// whether its last request was accepted has to model the state machine twice and
// guess after every failure; one word here means the worker's account is the
// only account.
struct SessionAckPayload {
    SessionEnvelope envelope;
    SessionState state = SessionState::Empty;
    FailureCode failure = FailureCode::None;
    std::string message; // UTF-8, empty when there is nothing a user must act on
};

struct SchemaResultPayload {
    SessionEnvelope envelope;
    // What the schema was read FROM: path, byte hash and package fingerprints,
    // rendered by the worker. Recorded with every solution revision (§12) so a
    // stored revision can say which definition produced it.
    std::string definitionIdentity; // UTF-8
    // The WorkflowSchema TapiocaInputSchema.ToJson already emits. Carried, never
    // parsed here: the panel's InputModel is the only thing that reads it, and
    // putting a JSON parser on the transport path would make the transport
    // depend on a shape only one consumer knows.
    std::string schemaJson; // UTF-8
};

struct SessionInputValue {
    std::string id;    // UTF-8, the TapiocaId the schema declared
    std::string value; // UTF-8, the neutral text form TapiocaInputCore round-trips
};

struct SetInputsPayload {
    SessionEnvelope envelope;
    std::vector<SessionInputValue> inputs;
};

struct SolvePayload {
    SessionEnvelope envelope;
    uint32_t wants = SolveWantsPreview | SolveWantsData;
};

struct CancelSolvePayload {
    SessionEnvelope envelope;
};

struct SolutionStartedPayload {
    SessionEnvelope envelope;
};

struct SessionOutputValue {
    std::string id;   // UTF-8
    std::string type; // UTF-8: the neutral type name, not a RhinoCommon one
    // The complete GH tree path, "{0;1}" as Grasshopper itself renders it, or
    // empty for a value that has none. Preserved rather than flattened: §9 wants
    // an unsupported tree inspectable, and a flattened tree cannot be
    // unflattened.
    std::string path;
    std::string value; // UTF-8
};

struct SessionDiagnostic {
    DiagnosticLevel level = DiagnosticLevel::Error;
    std::string component; // UTF-8, nickname and GUID
    std::string text;      // UTF-8
};

// One ACCEPTED solution, as the worker produced it. The host decides whether to
// publish it: a payload whose requestRevision is older than the newest requested
// is collected for diagnostics and dropped (§7).
struct SolutionResultPayload {
    SessionEnvelope envelope;
    // Counted separately from requestRevision, and by the WORKER: it is the
    // number of solutions this session actually produced, which is what an
    // immutable store addresses and what Apply names.
    uint32_t solutionRevision = 0;
    uint32_t elapsedMs = 0;
    // Which preview batch carries this solution's geometry, or 0/0 when none was
    // wanted or produced. The batch travels over the preview messages; this is
    // the binding that makes it belong to a revision rather than to "now".
    uint32_t previewEpoch = 0;
    uint32_t previewRevision = 0;
    std::string definitionIdentity; // UTF-8
    // Hash of the input snapshot the solve actually ran with -- not of the one
    // the host last sent. They differ exactly when something raced, and that is
    // the case worth being able to see.
    std::string inputSnapshotHash; // UTF-8
    std::vector<SessionOutputValue> outputs;
    std::vector<SessionDiagnostic> diagnostics;
};

struct SolutionFailedPayload {
    SessionEnvelope envelope;
    FailureCode failure = FailureCode::SolveFailed;
    uint32_t elapsedMs = 0;
    std::string message; // UTF-8
    std::vector<SessionDiagnostic> diagnostics;
};

struct GetDiagnosticsPayload {
    SessionEnvelope envelope;
};

// The developer diagnostics of §11: dependency versions, solve phase, timings,
// host pid and generation, restart reason. One rendered block rather than
// fields, because it is read by a person and its contents change with every
// thing worth adding to it.
struct DiagnosticsResultPayload {
    SessionEnvelope envelope;
    std::string report; // UTF-8
};

std::vector<uint8_t> EncodeOpenSessionPayload (const OpenSessionPayload& message);
bool DecodeOpenSessionPayload (const uint8_t* bytes, size_t size, OpenSessionPayload& message, std::string& error);

std::vector<uint8_t> EncodeCloseSessionPayload (const CloseSessionPayload& message);
bool DecodeCloseSessionPayload (const uint8_t* bytes, size_t size, CloseSessionPayload& message, std::string& error);

std::vector<uint8_t> EncodeSetSessionModePayload (const SetSessionModePayload& message);
bool DecodeSetSessionModePayload (const uint8_t* bytes, size_t size, SetSessionModePayload& message,
                                  std::string& error);

std::vector<uint8_t> EncodeLoadDefinitionPayload (const LoadDefinitionPayload& message);
bool DecodeLoadDefinitionPayload (const uint8_t* bytes, size_t size, LoadDefinitionPayload& message,
                                  std::string& error);

std::vector<uint8_t> EncodeReloadDefinitionPayload (const ReloadDefinitionPayload& message);
bool DecodeReloadDefinitionPayload (const uint8_t* bytes, size_t size, ReloadDefinitionPayload& message,
                                    std::string& error);

std::vector<uint8_t> EncodeGetSchemaPayload (const GetSchemaPayload& message);
bool DecodeGetSchemaPayload (const uint8_t* bytes, size_t size, GetSchemaPayload& message, std::string& error);

std::vector<uint8_t> EncodeSessionAckPayload (const SessionAckPayload& message);
bool DecodeSessionAckPayload (const uint8_t* bytes, size_t size, SessionAckPayload& message, std::string& error);

std::vector<uint8_t> EncodeSchemaResultPayload (const SchemaResultPayload& message);
bool DecodeSchemaResultPayload (const uint8_t* bytes, size_t size, SchemaResultPayload& message, std::string& error);

std::vector<uint8_t> EncodeSetInputsPayload (const SetInputsPayload& message);
bool DecodeSetInputsPayload (const uint8_t* bytes, size_t size, SetInputsPayload& message, std::string& error);

std::vector<uint8_t> EncodeSolvePayload (const SolvePayload& message);
bool DecodeSolvePayload (const uint8_t* bytes, size_t size, SolvePayload& message, std::string& error);

std::vector<uint8_t> EncodeCancelSolvePayload (const CancelSolvePayload& message);
bool DecodeCancelSolvePayload (const uint8_t* bytes, size_t size, CancelSolvePayload& message, std::string& error);

std::vector<uint8_t> EncodeSolutionStartedPayload (const SolutionStartedPayload& message);
bool DecodeSolutionStartedPayload (const uint8_t* bytes, size_t size, SolutionStartedPayload& message,
                                   std::string& error);

std::vector<uint8_t> EncodeSolutionResultPayload (const SolutionResultPayload& message);
bool DecodeSolutionResultPayload (const uint8_t* bytes, size_t size, SolutionResultPayload& message,
                                  std::string& error);

std::vector<uint8_t> EncodeSolutionFailedPayload (const SolutionFailedPayload& message);
bool DecodeSolutionFailedPayload (const uint8_t* bytes, size_t size, SolutionFailedPayload& message,
                                  std::string& error);

std::vector<uint8_t> EncodeGetDiagnosticsPayload (const GetDiagnosticsPayload& message);
bool DecodeGetDiagnosticsPayload (const uint8_t* bytes, size_t size, GetDiagnosticsPayload& message,
                                  std::string& error);

std::vector<uint8_t> EncodeDiagnosticsResultPayload (const DiagnosticsResultPayload& message);
bool DecodeDiagnosticsResultPayload (const uint8_t* bytes, size_t size, DiagnosticsResultPayload& message,
                                     std::string& error);

// Names for logs and for the panel's one-line status. Kept beside the codec so
// the wording is covered by the offline tests rather than written twice.
const char* DescribeSessionState (SessionState state);
const char* DescribeSessionMode (SessionMode mode);
const char* DescribeFailureCode (FailureCode failure);
const char* DescribeDiagnosticLevel (DiagnosticLevel level);

// True when `value` is one this build knows. A message carrying an unknown one
// is refused rather than cast: an enum widened by the other half is the same
// class of mismatch as a version skew, and casting it would land an
// out-of-range value in a switch that has no case for it.
bool KnownSessionMode (uint32_t value);
bool KnownSessionState (uint32_t value);
bool KnownFailureCode (uint32_t value);
bool KnownDiagnosticLevel (uint32_t value);

} // namespace protocol
} // namespace grasshopper
} // namespace evp

#endif
