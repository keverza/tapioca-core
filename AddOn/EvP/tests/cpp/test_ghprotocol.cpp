// Grasshopper/GhProtocol.cpp — the Archicad <-> Tapioca.GhWorker.exe wire format.
//
// This is the one part of the process boundary that can be proved without
// Archicad, without a worker and without Rhino, and it is also the part whose
// failures are the quietest. Framing does not fail at the boundary: a length
// read as a type, a payload cap checked after the allocation, a version accepted
// because nobody compared it, a declared length reconciled against the wrong
// number — each produces a peer that reads one message as another, and from
// inside Archicad that surfaces as "Grasshopper did not work".
//
// So the round trips below are the cheap half and the REFUSALS are the point.

#include "Grasshopper/GhProtocol.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace evp::grasshopper::protocol;

namespace {

std::vector<uint8_t> HeaderFor (MessageType type, uint32_t requestId, uint32_t correlationId, uint32_t payloadBytes)
{
    return EncodeHeader (type, requestId, correlationId, payloadBytes);
}

} // namespace

TEST (GhProtocol, HeaderRoundTripsEveryField)
{
    const std::vector<uint8_t> bytes = HeaderFor (MessageType::ApiRequest, 7, 9, 42);
    ASSERT_EQ (HeaderSize, bytes.size ());

    Header header;
    std::string error;
    ASSERT_TRUE (DecodeHeader (bytes.data (), bytes.size (), header, error)) << error;
    EXPECT_EQ (Version, header.protocolVersion);
    EXPECT_EQ (MessageType::ApiRequest, header.messageType);
    EXPECT_EQ (7u, header.requestId);
    EXPECT_EQ (9u, header.correlationId);
    EXPECT_EQ (42u, header.payloadBytes);
}

TEST (GhProtocol, HeaderIsLittleEndianOnTheWire)
{
    // Spelled out rather than round-tripped: the two halves are a C++ .apx and a
    // C# worker with no shared header, so "both ends agree" is only true if the
    // bytes are pinned. A change here is a change to BridgeProtocol.cs too.
    const std::vector<uint8_t> bytes = HeaderFor (MessageType::Heartbeat, 0x04030201u, 0, 0);
    ASSERT_EQ (HeaderSize, bytes.size ());
    EXPECT_EQ (0x01u, bytes[8]);
    EXPECT_EQ (0x02u, bytes[9]);
    EXPECT_EQ (0x03u, bytes[10]);
    EXPECT_EQ (0x04u, bytes[11]);
}

TEST (GhProtocol, ShortHeaderIsRefused)
{
    std::vector<uint8_t> bytes = HeaderFor (MessageType::Heartbeat, 0, 0, 0);
    bytes.pop_back ();

    Header header;
    std::string error;
    EXPECT_FALSE (DecodeHeader (bytes.data (), bytes.size (), header, error));
    EXPECT_FALSE (error.empty ());
}

TEST (GhProtocol, AForeignProtocolVersionIsRefusedWithBothNumbers)
{
    std::vector<uint8_t> bytes = HeaderFor (MessageType::Hello, 0, 0, 0);
    bytes[0] = (uint8_t) (Version + 1);

    Header header;
    std::string error;
    ASSERT_FALSE (DecodeHeader (bytes.data (), bytes.size (), header, error));

    // Both numbers, because "protocol mismatch" alone does not say which half is
    // stale, and the fix ("rebuild and redeploy both halves") depends on that.
    EXPECT_NE (std::string::npos, error.find (std::to_string (Version + 1)));
    EXPECT_NE (std::string::npos, error.find (std::to_string (Version)));
}

TEST (GhProtocol, TheVersionIsCheckedBeforeTheMessageType)
{
    // A worker from another version may legitimately use a type number this
    // build has never heard of. Reporting that as "unknown message type" sends
    // whoever reads the log after entirely the wrong thing.
    std::vector<uint8_t> bytes = HeaderFor (MessageType::Hello, 0, 0, 0);
    bytes[0] = (uint8_t) (Version + 1);
    bytes[4] = 0xFE; // a type from a future protocol

    Header header;
    std::string error;
    ASSERT_FALSE (DecodeHeader (bytes.data (), bytes.size (), header, error));
    EXPECT_NE (std::string::npos, error.find ("protocol"));
}

TEST (GhProtocol, AnUnknownMessageTypeIsRefused)
{
    std::vector<uint8_t> bytes = HeaderFor (MessageType::Hello, 0, 0, 0);
    bytes[4] = 0x7F;

    Header header;
    std::string error;
    EXPECT_FALSE (DecodeHeader (bytes.data (), bytes.size (), header, error));
    EXPECT_NE (std::string::npos, error.find ("127"));
}

TEST (GhProtocol, AnOversizedPayloadIsRefusedBeforeItIsAllocated)
{
    // The whole reason the cap lives in the header decoder: the reader sizes its
    // buffer from this number, so a corrupt or hostile length must be a refusal
    // rather than a reserve.
    const std::vector<uint8_t> bytes = HeaderFor (MessageType::Log, 0, 0, MaxPayloadBytes + 1);

    Header header;
    std::string error;
    EXPECT_FALSE (DecodeHeader (bytes.data (), bytes.size (), header, error));
    EXPECT_NE (std::string::npos, error.find (std::to_string (MaxPayloadBytes)));
}

TEST (GhProtocol, ThePayloadCapItselfIsAccepted)
{
    const std::vector<uint8_t> bytes = HeaderFor (MessageType::Log, 0, 0, MaxPayloadBytes);

    Header header;
    std::string error;
    ASSERT_TRUE (DecodeHeader (bytes.data (), bytes.size (), header, error)) << error;
    EXPECT_EQ (MaxPayloadBytes, header.payloadBytes);
}

TEST (GhProtocol, HelloRoundTripsAndRefusesAPidlessWorker)
{
    HelloPayload sent;
    sent.processId = 4321;
    sent.capabilities = 0;
    const std::vector<uint8_t> bytes = EncodeHelloPayload (sent);

    HelloPayload received;
    std::string error;
    ASSERT_TRUE (DecodeHelloPayload (bytes.data (), bytes.size (), received, error)) << error;
    EXPECT_EQ (4321u, received.processId);

    // The host supervises by pid: kill, restart generation and every log line
    // depend on it, so a hello without one is not a worker it can own.
    HelloPayload zeroed;
    zeroed.processId = 0;
    const std::vector<uint8_t> pidless = EncodeHelloPayload (zeroed);
    EXPECT_FALSE (DecodeHelloPayload (pidless.data (), pidless.size (), received, error));
}

TEST (GhProtocol, ApiRequestRoundTrips)
{
    ApiRequestPayload sent;
    sent.command = "Tapioca.GetSelection";
    sent.parameters = "{\"includeHidden\":false}";
    const std::vector<uint8_t> bytes = EncodeApiRequestPayload (sent);

    ApiRequestPayload received;
    std::string error;
    ASSERT_TRUE (DecodeApiRequestPayload (bytes.data (), bytes.size (), received, error)) << error;
    EXPECT_EQ (sent.command, received.command);
    EXPECT_EQ (sent.parameters, received.parameters);
}

TEST (GhProtocol, ApiRequestAcceptsEmptyParameters)
{
    ApiRequestPayload sent;
    sent.command = "Tapioca.GetStatus";
    const std::vector<uint8_t> bytes = EncodeApiRequestPayload (sent);

    ApiRequestPayload received;
    std::string error;
    ASSERT_TRUE (DecodeApiRequestPayload (bytes.data (), bytes.size (), received, error)) << error;
    EXPECT_EQ (sent.command, received.command);
    EXPECT_TRUE (received.parameters.empty ());
}

TEST (GhProtocol, ApiRequestWithoutACommandIsRefused)
{
    ApiRequestPayload sent;
    sent.parameters = "{}";
    const std::vector<uint8_t> bytes = EncodeApiRequestPayload (sent);

    ApiRequestPayload received;
    std::string error;
    EXPECT_FALSE (DecodeApiRequestPayload (bytes.data (), bytes.size (), received, error));
}

TEST (GhProtocol, ApiRequestThatDescribesMoreThanItCarriesIsRefused)
{
    // ⚠️ THE OVER-READ. The declared lengths are reconciled against what ACTUALLY
    // ARRIVED, never against each other: the header's payloadBytes is the only
    // authority for how much there is, and a payload that claims a longer
    // parameter block than it carries would otherwise be read off the end of the
    // buffer.
    ApiRequestPayload sent;
    sent.command = "Tapioca.GetStatus";
    sent.parameters = "{}";
    std::vector<uint8_t> bytes = EncodeApiRequestPayload (sent);
    bytes[4] = 0xFF; // parameters now claim far more than the payload holds

    ApiRequestPayload received;
    std::string error;
    EXPECT_FALSE (DecodeApiRequestPayload (bytes.data (), bytes.size (), received, error));
}

TEST (GhProtocol, ApiRequestWithAnAbsurdCommandLengthIsRefused)
{
    std::vector<uint8_t> bytes (16, 0);
    bytes[0] = 0x00;
    bytes[1] = 0x10; // 4096 bytes of command name, over MaxCommandBytes

    ApiRequestPayload received;
    std::string error;
    ASSERT_FALSE (DecodeApiRequestPayload (bytes.data (), bytes.size (), received, error));
    EXPECT_NE (std::string::npos, error.find (std::to_string (MaxCommandBytes)));
}

TEST (GhProtocol, AckRoundTripsAndRefusesAnUnknownStatus)
{
    AckPayload sent;
    sent.status = AckStatus::Failed;
    sent.message = "Rhino would not start in the Tapioca worker process.";
    const std::vector<uint8_t> bytes = EncodeAckPayload (sent);

    AckPayload received;
    std::string error;
    ASSERT_TRUE (DecodeAckPayload (bytes.data (), bytes.size (), received, error)) << error;
    EXPECT_EQ (AckStatus::Failed, received.status);
    EXPECT_EQ (sent.message, received.message);

    std::vector<uint8_t> corrupt = bytes;
    corrupt[0] = 0x40;
    EXPECT_FALSE (DecodeAckPayload (corrupt.data (), corrupt.size (), received, error));
}

TEST (GhProtocol, TextPayloadsRoundTripIncludingEmptyAndUtf8)
{
    std::string decoded;
    std::string error;

    ASSERT_TRUE (DecodeTextPayload (nullptr, 0, decoded, error));
    EXPECT_TRUE (decoded.empty ());

    // A refusal reason travels this way and carries whatever the user's Archicad
    // is localized to.
    const std::string sent = "a worker that speaks protocol \xE2\x88\x9E";
    const std::vector<uint8_t> bytes = EncodeTextPayload (sent);
    ASSERT_TRUE (DecodeTextPayload (bytes.data (), bytes.size (), decoded, error)) << error;
    EXPECT_EQ (sent, decoded);
}

TEST (GhProtocol, AnEmbeddedNulIsRefusedRatherThanTruncating)
{
    // Silent truncation is the failure mode here: a command name cut at a NUL
    // would dispatch to a DIFFERENT command rather than fail.
    std::vector<uint8_t> bytes = EncodeTextPayload ("Tapioca.GetStatus");
    bytes[7] = 0;

    std::string decoded;
    std::string error;
    EXPECT_FALSE (DecodeTextPayload (bytes.data (), bytes.size (), decoded, error));
}

TEST (GhProtocol, RunReportRoundTripsEveryField)
{
    RunReportPayload sent;
    sent.ok = false;
    sent.elapsedMs = 1837;
    sent.headline = "Solved Apartment numbering.gh with 2 error(s).";
    sent.errors.push_back ("Deconstruct Brep: Solution exception");
    sent.errors.push_back ("Tapir Get Elements: Failed to connect to Archicad.");
    sent.warnings.push_back ("Number Slider: value clamped");

    const std::vector<uint8_t> bytes = EncodeRunReportPayload (sent);

    RunReportPayload received;
    std::string error;
    ASSERT_TRUE (DecodeRunReportPayload (bytes.data (), bytes.size (), received, error)) << error;
    EXPECT_FALSE (received.ok);
    EXPECT_EQ (1837u, received.elapsedMs);
    EXPECT_EQ (sent.headline, received.headline);
    ASSERT_EQ (2u, received.errors.size ());
    EXPECT_EQ (sent.errors[1], received.errors[1]);
    ASSERT_EQ (1u, received.warnings.size ());
    EXPECT_EQ (sent.warnings[0], received.warnings[0]);
}

TEST (GhProtocol, RunReportRoundTripsACleanRun)
{
    RunReportPayload sent;
    sent.ok = true;
    sent.elapsedMs = 12;
    sent.headline = "Solved sample.gh.";

    const std::vector<uint8_t> bytes = EncodeRunReportPayload (sent);

    RunReportPayload received;
    std::string error;
    ASSERT_TRUE (DecodeRunReportPayload (bytes.data (), bytes.size (), received, error)) << error;
    EXPECT_TRUE (received.ok);
    EXPECT_TRUE (received.errors.empty ());
    EXPECT_TRUE (received.warnings.empty ());
}

TEST (GhProtocol, ARunReportShorterThanItsFixedHeaderIsRefused)
{
    RunReportPayload sent;
    sent.headline = "x";
    std::vector<uint8_t> bytes = EncodeRunReportPayload (sent);
    bytes.resize (12);

    RunReportPayload received;
    std::string error;
    EXPECT_FALSE (DecodeRunReportPayload (bytes.data (), bytes.size (), received, error));
}

TEST (GhProtocol, ARunReportPromisingMoreMessagesThanItCarriesIsRefused)
{
    // ⚠️ THE LIST OVER-READ, and the reason the counts are checked BEFORE the
    // loops rather than trusted one entry at a time. A corrupt count of four
    // billion would otherwise be a four-billion-entry reserve before the first
    // read failed.
    RunReportPayload sent;
    sent.headline = "Solved.";
    std::vector<uint8_t> bytes = EncodeRunReportPayload (sent);
    bytes[8] = 0xFF;
    bytes[9] = 0xFF;
    bytes[10] = 0xFF;
    bytes[11] = 0xFF;

    RunReportPayload received;
    std::string error;
    ASSERT_FALSE (DecodeRunReportPayload (bytes.data (), bytes.size (), received, error));
    EXPECT_NE (std::string::npos, error.find ("more messages"));
}

TEST (GhProtocol, ARunReportStringLongerThanItsPayloadIsRefused)
{
    RunReportPayload sent;
    sent.headline = "Solved.";
    std::vector<uint8_t> bytes = EncodeRunReportPayload (sent);
    // The headline length sits right after the four fixed fields.
    bytes[16] = 0xFF;
    bytes[17] = 0xFF;

    RunReportPayload received;
    std::string error;
    EXPECT_FALSE (DecodeRunReportPayload (bytes.data (), bytes.size (), received, error));
}

TEST (GhProtocol, ARunReportReadsAsSomethingAUserCanActOn)
{
    // DescribeRunReport is the entire product surface of a Run today, so what it
    // puts in front of a user is worth pinning: the headline first, the errors
    // before the warnings, and every component named.
    RunReportPayload report;
    report.ok = false;
    report.elapsedMs = 1837;
    report.headline = "Solved numbering.gh with 1 error(s).";
    report.errors.push_back ("Deconstruct Brep: Solution exception");
    report.warnings.push_back ("Slider: clamped");

    const std::string text = DescribeRunReport (report);
    EXPECT_NE (std::string::npos, text.find ("Solved numbering.gh"));
    EXPECT_NE (std::string::npos, text.find ("1837 ms"));
    EXPECT_LT (text.find ("Deconstruct Brep"), text.find ("Slider"));
}

TEST (GhProtocol, ACleanRunSaysSoRatherThanShowingNothing)
{
    // An empty dialog after a successful Run reads as a broken dialog.
    RunReportPayload report;
    report.ok = true;
    report.headline = "Solved sample.gh.";
    EXPECT_NE (std::string::npos, DescribeRunReport (report).find ("No component reported"));
}

TEST (GhProtocol, ARunReportCarriesTheUndoLedgerBothWays)
{
    RunReportPayload sent;
    sent.ok = true;
    sent.elapsedMs = 42;
    sent.headline = "Solved numbering.gh.";
    sent.ledger.push_back ({ "TapirCommand.MoveElements", 14 });
    sent.ledger.push_back ({ "API.GetSelectedElements", 1 });

    const std::vector<uint8_t> bytes = EncodeRunReportPayload (sent);

    RunReportPayload received;
    std::string error;
    ASSERT_TRUE (DecodeRunReportPayload (bytes.data (), bytes.size (), received, error)) << error;
    ASSERT_EQ (2u, received.ledger.size ());
    EXPECT_EQ ("TapirCommand.MoveElements", received.ledger[0].command);
    EXPECT_EQ (14u, received.ledger[0].invocations);
    EXPECT_EQ ("API.GetSelectedElements", received.ledger[1].command);
    EXPECT_EQ (1u, received.ledger[1].invocations);
}

TEST (GhProtocol, ARunReportWithNoLedgerStillRoundTrips)
{
    // The default: the Tapir proxy is off, so nothing was counted. An absent
    // ledger must decode as absent, not as a zero-cost run that was measured.
    RunReportPayload sent;
    sent.ok = true;
    sent.headline = "Solved sample.gh.";

    const std::vector<uint8_t> bytes = EncodeRunReportPayload (sent);

    RunReportPayload received;
    std::string error;
    ASSERT_TRUE (DecodeRunReportPayload (bytes.data (), bytes.size (), received, error)) << error;
    EXPECT_TRUE (received.ledger.empty ());
    EXPECT_EQ (std::string::npos, DescribeRunReport (received).find ("undo step"));
}

TEST (GhProtocol, ARunReportPromisingMoreLedgerEntriesThanItCarriesIsRefused)
{
    RunReportPayload sent;
    sent.headline = "Solved.";
    sent.ledger.push_back ({ "TapirCommand.MoveElements", 1 });
    std::vector<uint8_t> bytes = EncodeRunReportPayload (sent);
    // The ledger count sits after ok, elapsed and the two message counts.
    bytes[16] = 0xFF;
    bytes[17] = 0xFF;
    bytes[18] = 0xFF;
    bytes[19] = 0xFF;

    RunReportPayload received;
    std::string error;
    EXPECT_FALSE (DecodeRunReportPayload (bytes.data (), bytes.size (), received, error));
}

TEST (GhProtocol, ARunReportEndingInsideALedgerCountIsRefused)
{
    // The invocation count trails its name, so a payload truncated between the
    // two is the one place a ledger read can run off the end.
    RunReportPayload sent;
    sent.headline = "Solved.";
    sent.ledger.push_back ({ "TapirCommand.MoveElements", 7 });
    std::vector<uint8_t> bytes = EncodeRunReportPayload (sent);
    bytes.resize (bytes.size () - 2);

    RunReportPayload received;
    std::string error;
    EXPECT_FALSE (DecodeRunReportPayload (bytes.data (), bytes.size (), received, error));
}

TEST (GhProtocol, AMeasuredRunPutsTheUndoCostInFrontOfTheUser)
{
    RunReportPayload report;
    report.ok = true;
    report.headline = "Solved numbering.gh.";
    report.ledger.push_back ({ "TapirCommand.MoveElements", 40 });

    const std::string text = DescribeRunReport (report);
    EXPECT_NE (std::string::npos, text.find ("40 undo step"));
    EXPECT_NE (std::string::npos, text.find ("MoveElements x40"));
}

TEST (GhProtocol, EveryMessageTypeHasAName)
{
    // DescribeMessageType feeds the log and the "wrong direction" refusal; an
    // unnamed type would surface as "unknown" in exactly the diagnostic someone
    // is reading.
    const MessageType types[] = { MessageType::Hello,      MessageType::HelloAck,    MessageType::Heartbeat,
                                  MessageType::ApiRequest, MessageType::ApiResponse, MessageType::ShowEditor,
                                  MessageType::HideEditor, MessageType::Shutdown,    MessageType::Ack,
                                  MessageType::Log };
    for (size_t index = 0; index < sizeof (types) / sizeof (types[0]); ++index)
        EXPECT_STRNE ("unknown", DescribeMessageType (types[index]));
}
