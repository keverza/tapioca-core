// Grasshopper/GhSessionProtocol.cpp — the session half of the Archicad <->
// Tapioca.GhWorker.exe wire.
//
// The framing argument from test_ghprotocol.cpp applies here unchanged, and one
// more sits on top of it. These payloads carry the ROUTING that decides whether
// a solution may be published: {hostGeneration, sessionId, requestRevision} on
// the way in, and {solutionRevision} on the way back. A generation misread is a
// result from a dead worker accepted as current. A revision misread is a stale
// solve published over a newer one — which is not an error on screen, it is the
// WRONG ANSWER on screen, with an Apply button beside it.
//
// So the round trips below are the cheap half and the refusals are the point,
// exactly as they are for the control protocol.

#include "Grasshopper/GhSessionProtocol.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace evp::grasshopper::protocol;

namespace {

SessionEnvelope EnvelopeOf (uint32_t generation, uint32_t session, uint32_t revision)
{
    SessionEnvelope envelope;
    envelope.hostGeneration = generation;
    envelope.sessionId = session;
    envelope.requestRevision = revision;
    return envelope;
}

SessionDiagnostic DiagnosticOf (DiagnosticLevel level, const std::string& component, const std::string& text)
{
    SessionDiagnostic diagnostic;
    diagnostic.level = level;
    diagnostic.component = component;
    diagnostic.text = text;
    return diagnostic;
}

// Every session payload begins with the same twelve bytes, so the checks that
// are about the ENVELOPE rather than about one message are written once and run
// against each of them through this.
template <typename Payload, typename Decoder>
void ExpectsAnEnvelope (const std::vector<uint8_t>& valid, Decoder decode, const char* what)
{
    Payload message;
    std::string error;

    // Short of the envelope.
    for (size_t size = 0; size < SessionEnvelopeSize; ++size) {
        std::vector<uint8_t> truncated (valid.begin (), valid.begin () + (long) size);
        EXPECT_FALSE (decode (truncated.data (), truncated.size (), message, error))
            << what << " accepted " << size << " bytes";
    }

    // Generation zero.
    std::vector<uint8_t> generationless = valid;
    generationless[0] = 0;
    generationless[1] = 0;
    generationless[2] = 0;
    generationless[3] = 0;
    EXPECT_FALSE (decode (generationless.data (), generationless.size (), message, error))
        << what << " accepted a message with no host generation";
}

} // namespace

TEST (GhSessionProtocol, EnvelopeIsTheFirstTwelveBytesAndIsLittleEndian)
{
    // Pinned rather than round-tripped. The two halves are a C++ .apx and a C#
    // worker with no shared header, and the envelope is the part the host reads
    // BEFORE it decides whether to parse the rest — so its offsets are a
    // contract, not an implementation detail. A change here is a change to
    // SessionProtocol.cs too.
    CloseSessionPayload message;
    message.envelope = EnvelopeOf (0x04030201u, 0x08070605u, 0x0C0B0A09u);
    const std::vector<uint8_t> bytes = EncodeCloseSessionPayload (message);

    ASSERT_EQ (SessionEnvelopeSize, bytes.size ());
    EXPECT_EQ (0x01u, bytes[0]);
    EXPECT_EQ (0x04u, bytes[3]);
    EXPECT_EQ (0x05u, bytes[4]);
    EXPECT_EQ (0x08u, bytes[7]);
    EXPECT_EQ (0x09u, bytes[8]);
    EXPECT_EQ (0x0Cu, bytes[11]);
}

TEST (GhSessionProtocol, OpenSessionRoundTrips)
{
    OpenSessionPayload sent;
    sent.envelope = EnvelopeOf (3, 11, 0);
    sent.mode = SessionMode::Authoring;

    const std::vector<uint8_t> bytes = EncodeOpenSessionPayload (sent);
    OpenSessionPayload received;
    std::string error;
    ASSERT_TRUE (DecodeOpenSessionPayload (bytes.data (), bytes.size (), received, error)) << error;
    EXPECT_EQ (3u, received.envelope.hostGeneration);
    EXPECT_EQ (11u, received.envelope.sessionId);
    EXPECT_EQ (SessionMode::Authoring, received.mode);
}

TEST (GhSessionProtocol, OpenSessionRefusesSessionZero)
{
    // The host assigns ids, so a zero means it did not fill one in — and every
    // later message would then address a session nothing owns. This is the ONE
    // message that creates a session, so it is the one place the check belongs.
    OpenSessionPayload sent;
    sent.envelope = EnvelopeOf (3, 0, 0);
    const std::vector<uint8_t> bytes = EncodeOpenSessionPayload (sent);

    OpenSessionPayload received;
    std::string error;
    EXPECT_FALSE (DecodeOpenSessionPayload (bytes.data (), bytes.size (), received, error));
    EXPECT_NE (std::string::npos, error.find ("session id"));
}

TEST (GhSessionProtocol, OpenSessionRefusesAnUnknownMode)
{
    OpenSessionPayload sent;
    sent.envelope = EnvelopeOf (3, 11, 0);
    std::vector<uint8_t> bytes = EncodeOpenSessionPayload (sent);
    ASSERT_EQ (SessionEnvelopeSize + 4, bytes.size ());
    bytes[SessionEnvelopeSize] = 7;

    OpenSessionPayload received;
    std::string error;
    EXPECT_FALSE (DecodeOpenSessionPayload (bytes.data (), bytes.size (), received, error));
}

TEST (GhSessionProtocol, EveryEnvelopeOnlyMessageChecksItsEnvelope)
{
    CloseSessionPayload close;
    close.envelope = EnvelopeOf (2, 5, 0);
    ExpectsAnEnvelope<CloseSessionPayload> (EncodeCloseSessionPayload (close), DecodeCloseSessionPayload,
                                            "close-session");

    GetSchemaPayload schema;
    schema.envelope = EnvelopeOf (2, 5, 0);
    ExpectsAnEnvelope<GetSchemaPayload> (EncodeGetSchemaPayload (schema), DecodeGetSchemaPayload, "get-schema");

    CancelSolvePayload cancel;
    cancel.envelope = EnvelopeOf (2, 5, 4);
    ExpectsAnEnvelope<CancelSolvePayload> (EncodeCancelSolvePayload (cancel), DecodeCancelSolvePayload, "cancel-solve");
}

TEST (GhSessionProtocol, EnvelopeOnlyMessagesRefuseATrailingByte)
{
    // A payload with a tail nobody reads is either a version skew the handshake
    // was supposed to catch or a sender writing a shape this build does not
    // have. Silently ignoring it would let the two halves drift apart with
    // nothing to show for it until something much later went wrong.
    CloseSessionPayload sent;
    sent.envelope = EnvelopeOf (1, 1, 0);
    std::vector<uint8_t> bytes = EncodeCloseSessionPayload (sent);
    bytes.push_back (0);

    CloseSessionPayload received;
    std::string error;
    EXPECT_FALSE (DecodeCloseSessionPayload (bytes.data (), bytes.size (), received, error));
}

TEST (GhSessionProtocol, LoadDefinitionRoundTripsAndRefusesAnEmptyPath)
{
    LoadDefinitionPayload sent;
    sent.envelope = EnvelopeOf (4, 2, 0);
    sent.path = "C:\\Definitions\\facade.gh";

    const std::vector<uint8_t> bytes = EncodeLoadDefinitionPayload (sent);
    LoadDefinitionPayload received;
    std::string error;
    ASSERT_TRUE (DecodeLoadDefinitionPayload (bytes.data (), bytes.size (), received, error)) << error;
    EXPECT_EQ (sent.path, received.path);

    LoadDefinitionPayload pathless;
    pathless.envelope = sent.envelope;
    const std::vector<uint8_t> empty = EncodeLoadDefinitionPayload (pathless);
    EXPECT_FALSE (DecodeLoadDefinitionPayload (empty.data (), empty.size (), received, error));
}

TEST (GhSessionProtocol, LoadDefinitionRefusesAPathLongerThanItCarries)
{
    // The classic over-read: a declared length reconciled against nothing.
    LoadDefinitionPayload sent;
    sent.envelope = EnvelopeOf (4, 2, 0);
    sent.path = "short.gh";
    std::vector<uint8_t> bytes = EncodeLoadDefinitionPayload (sent);

    // Claim one more byte of path than the payload holds.
    bytes[SessionEnvelopeSize] = (uint8_t) (sent.path.size () + 1);

    LoadDefinitionPayload received;
    std::string error;
    EXPECT_FALSE (DecodeLoadDefinitionPayload (bytes.data (), bytes.size (), received, error));
}

TEST (GhSessionProtocol, LoadDefinitionRefusesAPathOverTheCeiling)
{
    // Refused BEFORE the string is built, which is the whole reason the ceiling
    // exists: a corrupt length must be a refusal, not a reserve.
    LoadDefinitionPayload sent;
    sent.envelope = EnvelopeOf (4, 2, 0);
    sent.path = "x";
    std::vector<uint8_t> bytes = EncodeLoadDefinitionPayload (sent);

    const uint32_t oversized = MaxDefinitionPathBytes + 1;
    bytes[SessionEnvelopeSize + 0] = (uint8_t) (oversized & 0xFFu);
    bytes[SessionEnvelopeSize + 1] = (uint8_t) ((oversized >> 8) & 0xFFu);
    bytes[SessionEnvelopeSize + 2] = (uint8_t) ((oversized >> 16) & 0xFFu);
    bytes[SessionEnvelopeSize + 3] = (uint8_t) ((oversized >> 24) & 0xFFu);

    LoadDefinitionPayload received;
    std::string error;
    EXPECT_FALSE (DecodeLoadDefinitionPayload (bytes.data (), bytes.size (), received, error));
    EXPECT_NE (std::string::npos, error.find ("limit"));
}

TEST (GhSessionProtocol, SetInputsRoundTripsEveryPair)
{
    SetInputsPayload sent;
    sent.envelope = EnvelopeOf (1, 1, 12);
    sent.inputs.push_back ({ "height", "3200" });
    sent.inputs.push_back ({ "material", "Concrete" });
    sent.inputs.push_back ({ "mirrored", "true" });

    const std::vector<uint8_t> bytes = EncodeSetInputsPayload (sent);
    SetInputsPayload received;
    std::string error;
    ASSERT_TRUE (DecodeSetInputsPayload (bytes.data (), bytes.size (), received, error)) << error;
    ASSERT_EQ (3u, received.inputs.size ());
    EXPECT_EQ ("height", received.inputs[0].id);
    EXPECT_EQ ("3200", received.inputs[0].value);
    EXPECT_EQ ("mirrored", received.inputs[2].id);
    EXPECT_EQ ("true", received.inputs[2].value);
}

TEST (GhSessionProtocol, SetInputsRefusesAnInputWithNoId)
{
    // An id-less value cannot be routed to a parameter, and applying it to
    // whichever input came next would be a wrong answer rather than a failure.
    SetInputsPayload sent;
    sent.envelope = EnvelopeOf (1, 1, 12);
    sent.inputs.push_back ({ "", "3200" });

    const std::vector<uint8_t> bytes = EncodeSetInputsPayload (sent);
    SetInputsPayload received;
    std::string error;
    EXPECT_FALSE (DecodeSetInputsPayload (bytes.data (), bytes.size (), received, error));
}

TEST (GhSessionProtocol, SetInputsRefusesACountOverTheCeiling)
{
    SetInputsPayload sent;
    sent.envelope = EnvelopeOf (1, 1, 12);
    std::vector<uint8_t> bytes = EncodeSetInputsPayload (sent);

    const uint32_t oversized = MaxSessionInputs + 1;
    bytes[SessionEnvelopeSize + 0] = (uint8_t) (oversized & 0xFFu);
    bytes[SessionEnvelopeSize + 1] = (uint8_t) ((oversized >> 8) & 0xFFu);
    bytes[SessionEnvelopeSize + 2] = (uint8_t) ((oversized >> 16) & 0xFFu);
    bytes[SessionEnvelopeSize + 3] = (uint8_t) ((oversized >> 24) & 0xFFu);

    SetInputsPayload received;
    std::string error;
    EXPECT_FALSE (DecodeSetInputsPayload (bytes.data (), bytes.size (), received, error));
}

TEST (GhSessionProtocol, SetInputsRefusesACountThatOverrunsItsPairs)
{
    // Off by one entry is the ordinary way a list codec reads off the end.
    SetInputsPayload sent;
    sent.envelope = EnvelopeOf (1, 1, 12);
    sent.inputs.push_back ({ "height", "3200" });
    std::vector<uint8_t> bytes = EncodeSetInputsPayload (sent);
    bytes[SessionEnvelopeSize] = 2;

    SetInputsPayload received;
    std::string error;
    EXPECT_FALSE (DecodeSetInputsPayload (bytes.data (), bytes.size (), received, error));
}

TEST (GhSessionProtocol, SolveRoundTripsItsWants)
{
    SolvePayload sent;
    sent.envelope = EnvelopeOf (2, 3, 44);
    sent.wants = SolveWantsPreview | SolveWantsCommit;

    const std::vector<uint8_t> bytes = EncodeSolvePayload (sent);
    SolvePayload received;
    std::string error;
    ASSERT_TRUE (DecodeSolvePayload (bytes.data (), bytes.size (), received, error)) << error;
    EXPECT_EQ (44u, received.envelope.requestRevision);
    EXPECT_EQ (SolveWantsPreview | SolveWantsCommit, received.wants);
}

TEST (GhSessionProtocol, SolveRefusesAnUnknownWantsBit)
{
    // Unlike a capability bit, which is additive within a version: honouring
    // only the bits this build knows would collect less than the host asked for
    // and report success for it.
    SolvePayload sent;
    sent.envelope = EnvelopeOf (2, 3, 44);
    sent.wants = SolveWantsData | (1u << 9);

    const std::vector<uint8_t> bytes = EncodeSolvePayload (sent);
    SolvePayload received;
    std::string error;
    EXPECT_FALSE (DecodeSolvePayload (bytes.data (), bytes.size (), received, error));
}

TEST (GhSessionProtocol, SolveRefusesWantingNothing)
{
    SolvePayload sent;
    sent.envelope = EnvelopeOf (2, 3, 44);
    sent.wants = SolveWantsNothing;

    const std::vector<uint8_t> bytes = EncodeSolvePayload (sent);
    SolvePayload received;
    std::string error;
    EXPECT_FALSE (DecodeSolvePayload (bytes.data (), bytes.size (), received, error));
}

TEST (GhSessionProtocol, SessionAckRoundTripsStateFailureAndMessage)
{
    SessionAckPayload sent;
    sent.envelope = EnvelopeOf (6, 1, 0);
    sent.state = SessionState::Invalid;
    sent.failure = FailureCode::DependencyMissing;
    sent.message = "This definition needs Kangaroo, which is not installed.";

    const std::vector<uint8_t> bytes = EncodeSessionAckPayload (sent);
    SessionAckPayload received;
    std::string error;
    ASSERT_TRUE (DecodeSessionAckPayload (bytes.data (), bytes.size (), received, error)) << error;
    EXPECT_EQ (SessionState::Invalid, received.state);
    EXPECT_EQ (FailureCode::DependencyMissing, received.failure);
    EXPECT_EQ (sent.message, received.message);
}

TEST (GhSessionProtocol, SessionAckRefusesAnUnknownStateOrFailure)
{
    SessionAckPayload sent;
    sent.envelope = EnvelopeOf (6, 1, 0);
    const std::vector<uint8_t> valid = EncodeSessionAckPayload (sent);

    std::vector<uint8_t> badState = valid;
    badState[SessionEnvelopeSize] = 200;
    SessionAckPayload received;
    std::string error;
    EXPECT_FALSE (DecodeSessionAckPayload (badState.data (), badState.size (), received, error));

    std::vector<uint8_t> badFailure = valid;
    badFailure[SessionEnvelopeSize + 4] = 200;
    EXPECT_FALSE (DecodeSessionAckPayload (badFailure.data (), badFailure.size (), received, error));
}

TEST (GhSessionProtocol, SchemaResultCarriesIdentityAndJsonUntouched)
{
    // The transport CARRIES the schema and never parses it: putting a JSON
    // parser on this path would make the transport depend on a shape only the
    // panel knows. So the bytes must come back exactly as they went in, quotes,
    // braces, backslashes and all.
    SchemaResultPayload sent;
    sent.envelope = EnvelopeOf (2, 1, 0);
    sent.definitionIdentity = "C:\\Definitions\\facade.gh sha256:abc packages:[Kangaroo 2.5.2]";
    sent.schemaJson = "{\"workflowId\":\"abc\",\"inputs\":[{\"id\":\"h\",\"default\":\"3\\\"\"}],\"errors\":[]}";

    const std::vector<uint8_t> bytes = EncodeSchemaResultPayload (sent);
    SchemaResultPayload received;
    std::string error;
    ASSERT_TRUE (DecodeSchemaResultPayload (bytes.data (), bytes.size (), received, error)) << error;
    EXPECT_EQ (sent.definitionIdentity, received.definitionIdentity);
    EXPECT_EQ (sent.schemaJson, received.schemaJson);
}

TEST (GhSessionProtocol, SolutionResultRoundTripsEveryFieldAndPreservesTreePaths)
{
    SolutionResultPayload sent;
    sent.envelope = EnvelopeOf (5, 2, 77);
    sent.solutionRevision = 4;
    sent.elapsedMs = 1834;
    sent.previewEpoch = 5;
    sent.previewRevision = 9;
    sent.definitionIdentity = "facade.gh sha256:beef";
    sent.inputSnapshotHash = "0011223344556677";
    sent.outputs.push_back ({ "area", "number", "{0}", "42.5" });
    sent.outputs.push_back ({ "area", "number", "{0;1}", "17.25" });
    sent.diagnostics.push_back (DiagnosticOf (DiagnosticLevel::Warning, "Divide {abc}", "Empty branch."));

    const std::vector<uint8_t> bytes = EncodeSolutionResultPayload (sent);
    SolutionResultPayload received;
    std::string error;
    ASSERT_TRUE (DecodeSolutionResultPayload (bytes.data (), bytes.size (), received, error)) << error;

    EXPECT_EQ (77u, received.envelope.requestRevision);
    EXPECT_EQ (4u, received.solutionRevision);
    EXPECT_EQ (1834u, received.elapsedMs);
    EXPECT_EQ (5u, received.previewEpoch);
    EXPECT_EQ (9u, received.previewRevision);
    EXPECT_EQ (sent.definitionIdentity, received.definitionIdentity);
    EXPECT_EQ (sent.inputSnapshotHash, received.inputSnapshotHash);

    ASSERT_EQ (2u, received.outputs.size ());
    // Two items of one output, told apart ONLY by their path. A codec that
    // dropped the path would make this pair indistinguishable, which is exactly
    // the flattening §9 forbids.
    EXPECT_EQ ("{0}", received.outputs[0].path);
    EXPECT_EQ ("{0;1}", received.outputs[1].path);
    EXPECT_EQ ("42.5", received.outputs[0].value);

    ASSERT_EQ (1u, received.diagnostics.size ());
    EXPECT_EQ (DiagnosticLevel::Warning, received.diagnostics[0].level);
    EXPECT_EQ ("Divide {abc}", received.diagnostics[0].component);
}

TEST (GhSessionProtocol, SolutionResultRefusesRevisionZero)
{
    // Apply addresses {hostGeneration, sessionId, solutionRevision}. A zero
    // there is an address the store can neither hold nor refuse meaningfully.
    SolutionResultPayload sent;
    sent.envelope = EnvelopeOf (5, 2, 77);
    sent.solutionRevision = 0;

    const std::vector<uint8_t> bytes = EncodeSolutionResultPayload (sent);
    SolutionResultPayload received;
    std::string error;
    EXPECT_FALSE (DecodeSolutionResultPayload (bytes.data (), bytes.size (), received, error));
    EXPECT_NE (std::string::npos, error.find ("solution revision"));
}

TEST (GhSessionProtocol, SolutionResultRefusesAnOutputCountThatOverruns)
{
    SolutionResultPayload sent;
    sent.envelope = EnvelopeOf (5, 2, 77);
    sent.solutionRevision = 1;
    sent.outputs.push_back ({ "area", "number", "{0}", "42.5" });

    std::vector<uint8_t> bytes = EncodeSolutionResultPayload (sent);
    // outputCount sits 16 bytes past the envelope.
    bytes[SessionEnvelopeSize + 16] = 2;

    SolutionResultPayload received;
    std::string error;
    EXPECT_FALSE (DecodeSolutionResultPayload (bytes.data (), bytes.size (), received, error));
}

TEST (GhSessionProtocol, SolutionResultLeavesTheCallersValueUntouchedOnRefusal)
{
    // Half-decoded output is worse than none: a caller that logged the message
    // after a refusal would print fields from two different solutions.
    SolutionResultPayload received;
    received.solutionRevision = 99;
    received.definitionIdentity = "untouched";

    const std::vector<uint8_t> nonsense (SessionEnvelopeSize + 8, 0x41);
    std::string error;
    EXPECT_FALSE (DecodeSolutionResultPayload (nonsense.data (), nonsense.size (), received, error));
    EXPECT_EQ (99u, received.solutionRevision);
    EXPECT_EQ ("untouched", received.definitionIdentity);
}

TEST (GhSessionProtocol, SolutionFailedRoundTripsAndRefusesNoFailure)
{
    SolutionFailedPayload sent;
    sent.envelope = EnvelopeOf (7, 1, 3);
    sent.failure = FailureCode::SolveTimedOut;
    sent.elapsedMs = 60000;
    sent.message = "The solution ran for 60 s.";
    sent.diagnostics.push_back (DiagnosticOf (DiagnosticLevel::Error, "Script {1}", "while True: pass"));

    const std::vector<uint8_t> bytes = EncodeSolutionFailedPayload (sent);
    SolutionFailedPayload received;
    std::string error;
    ASSERT_TRUE (DecodeSolutionFailedPayload (bytes.data (), bytes.size (), received, error)) << error;
    EXPECT_EQ (FailureCode::SolveTimedOut, received.failure);
    EXPECT_EQ (60000u, received.elapsedMs);
    ASSERT_EQ (1u, received.diagnostics.size ());
    EXPECT_EQ (DiagnosticLevel::Error, received.diagnostics[0].level);

    SolutionFailedPayload codeless;
    codeless.envelope = sent.envelope;
    codeless.failure = FailureCode::None;
    const std::vector<uint8_t> empty = EncodeSolutionFailedPayload (codeless);
    EXPECT_FALSE (DecodeSolutionFailedPayload (empty.data (), empty.size (), received, error));
}

TEST (GhSessionProtocol, DiagnosticsRefuseAnUnknownLevel)
{
    SolutionFailedPayload sent;
    sent.envelope = EnvelopeOf (7, 1, 3);
    sent.failure = FailureCode::SolveFailed;
    sent.diagnostics.push_back (DiagnosticOf (DiagnosticLevel::Remark, "c", "t"));

    std::vector<uint8_t> bytes = EncodeSolutionFailedPayload (sent);
    // The level word is the first field after the fixed header and the message.
    const size_t levelAt = SessionEnvelopeSize + 12 + 4 + sent.message.size ();
    ASSERT_LT (levelAt, bytes.size ());
    bytes[levelAt] = 42;

    SolutionFailedPayload received;
    std::string error;
    EXPECT_FALSE (DecodeSolutionFailedPayload (bytes.data (), bytes.size (), received, error));
}

TEST (GhSessionProtocol, EmbeddedNulsAreRefusedRatherThanTruncating)
{
    // A NUL inside a length-prefixed string is legal on the wire and illegal in
    // everything that reads one afterwards: it truncates silently on the way
    // into a C string, which turns a definition path into a shorter path that
    // may well exist.
    LoadDefinitionPayload sent;
    sent.envelope = EnvelopeOf (1, 1, 0);
    sent.path = std::string ("C:\\ok.gh") + '\0' + "trailing";

    const std::vector<uint8_t> bytes = EncodeLoadDefinitionPayload (sent);
    LoadDefinitionPayload received;
    std::string error;
    EXPECT_FALSE (DecodeLoadDefinitionPayload (bytes.data (), bytes.size (), received, error));
    EXPECT_NE (std::string::npos, error.find ("NUL"));
}

TEST (GhSessionProtocol, EveryFailureCodeHasItsOwnSentence)
{
    // §11's categories exist so the panel can say ONE concise thing per
    // failure. A code that fell through to "unrecognised" would be a category
    // the user is shown nothing useful about.
    for (uint32_t value = 0; value <= (uint32_t) FailureCode::SessionUnknown; ++value) {
        ASSERT_TRUE (KnownFailureCode (value)) << value;
        const std::string sentence = DescribeFailureCode ((FailureCode) value);
        EXPECT_FALSE (sentence.empty ()) << value;
        EXPECT_EQ (std::string::npos, sentence.find ("unrecognised")) << value;
    }

    EXPECT_FALSE (KnownFailureCode ((uint32_t) FailureCode::SessionUnknown + 1));
}

TEST (GhSessionProtocol, EveryStateAndModeIsNamed)
{
    for (uint32_t value = 0; value <= (uint32_t) SessionState::HostRestartRequired; ++value) {
        ASSERT_TRUE (KnownSessionState (value)) << value;
        EXPECT_STRNE ("unknown", DescribeSessionState ((SessionState) value)) << value;
    }

    EXPECT_FALSE (KnownSessionState ((uint32_t) SessionState::HostRestartRequired + 1));
    EXPECT_STREQ ("headless", DescribeSessionMode (SessionMode::Headless));
    EXPECT_STREQ ("authoring", DescribeSessionMode (SessionMode::Authoring));
    EXPECT_FALSE (KnownSessionMode (2));
}

TEST (GhSessionProtocol, SessionMessageTypesAreKnownToTheControlHeader)
{
    // The session messages travel in the SAME frame as every other one, so a
    // type this build did not add to the header's known set would be refused at
    // the frame before its payload was ever reached.
    const MessageType types[] = {
        MessageType::OpenSession,    MessageType::CloseSession,     MessageType::SetSessionMode,
        MessageType::LoadDefinition, MessageType::ReloadDefinition, MessageType::GetSchema,
        MessageType::SchemaResult,   MessageType::SetInputs,        MessageType::Solve,
        MessageType::CancelSolve,    MessageType::SolutionStarted,  MessageType::SolutionResult,
        MessageType::SolutionFailed, MessageType::GetDiagnostics,   MessageType::DiagnosticsResult,
        MessageType::SessionEvent,
    };

    for (MessageType type : types) {
        const std::vector<uint8_t> bytes = EncodeHeader (type, 1, 0, 0);
        Header header;
        std::string error;
        ASSERT_TRUE (DecodeHeader (bytes.data (), bytes.size (), header, error))
            << DescribeMessageType (type) << ": " << error;
        EXPECT_EQ (type, header.messageType);
        EXPECT_STRNE ("unknown", DescribeMessageType (type));
    }
}
