#include "GhProtocol.hpp"

namespace evp {
namespace grasshopper {
namespace protocol {

namespace {

// Spelled out byte by byte rather than memcpy'd from a struct. A packed struct
// would be shorter and would also make the wire format depend on the compiler
// that built each half — and the two halves here are a C++ .apx and a C# worker,
// which do not share one.
void AppendUInt32 (std::vector<uint8_t>& buffer, uint32_t value)
{
    buffer.push_back ((uint8_t) (value & 0xFFu));
    buffer.push_back ((uint8_t) ((value >> 8) & 0xFFu));
    buffer.push_back ((uint8_t) ((value >> 16) & 0xFFu));
    buffer.push_back ((uint8_t) ((value >> 24) & 0xFFu));
}

uint32_t ReadUInt32 (const uint8_t* bytes)
{
    return (uint32_t) bytes[0] | ((uint32_t) bytes[1] << 8) | ((uint32_t) bytes[2] << 16) | ((uint32_t) bytes[3] << 24);
}

void AppendBytes (std::vector<uint8_t>& buffer, const std::string& text)
{
    buffer.insert (buffer.end (), text.begin (), text.end ());
}

bool KnownMessageType (uint32_t value)
{
    switch ((MessageType) value) {
        case MessageType::Hello:
        case MessageType::HelloAck:
        case MessageType::Heartbeat:
        case MessageType::ApiRequest:
        case MessageType::ApiResponse:
        case MessageType::ShowEditor:
        case MessageType::HideEditor:
        case MessageType::Shutdown:
        case MessageType::Ack:
        case MessageType::Log:
        case MessageType::RunDefinition:
        case MessageType::CancelRun:
        case MessageType::RunResult:
        case MessageType::PreviewBeginBatch:
        case MessageType::PreviewAdded:
        case MessageType::PreviewChanged:
        case MessageType::PreviewRemoved:
        case MessageType::PreviewVisibility:
        case MessageType::PreviewSelection:
        case MessageType::PreviewEndBatch:
        case MessageType::PreviewDropAll:
        case MessageType::PreviewResyncRequest:
        case MessageType::PreviewBatchAck:
        case MessageType::PreviewPicked:
            return true;
    }
    return false;
}

bool ContainsNul (const std::string& text)
{
    return text.find (static_cast<char> (0)) != std::string::npos;
}

} // namespace

std::vector<uint8_t> EncodeHeader (MessageType type, uint32_t requestId, uint32_t correlationId, uint32_t payloadBytes)
{
    std::vector<uint8_t> header;
    header.reserve (HeaderSize);
    AppendUInt32 (header, Version);
    AppendUInt32 (header, (uint32_t) type);
    AppendUInt32 (header, requestId);
    AppendUInt32 (header, correlationId);
    AppendUInt32 (header, payloadBytes);
    return header;
}

bool DecodeHeader (const uint8_t* bytes, size_t size, Header& header, std::string& error)
{
    if (bytes == nullptr || size < HeaderSize) {
        error = "The message header was short.";
        return false;
    }

    const uint32_t version = ReadUInt32 (bytes);
    // ⚠️ THE VERSION IS CHECKED FIRST, BEFORE THE TYPE. A worker from another
    // version may legitimately use a type number this build has never heard of,
    // and reporting that as "unknown message type" would send whoever reads the
    // log after the wrong thing entirely.
    std::string refusal;
    if (!AcceptsVersion (version, refusal)) {
        error = refusal;
        return false;
    }

    const uint32_t type = ReadUInt32 (bytes + 4);
    if (!KnownMessageType (type)) {
        error = "The message type " + std::to_string (type) + " is not one this add-on knows.";
        return false;
    }

    const uint32_t payloadBytes = ReadUInt32 (bytes + 16);
    // Checked before any allocation, on purpose: a corrupt or hostile length
    // must be a refusal, not a reserve.
    if (payloadBytes > MaxPayloadBytes) {
        error = "The message payload claimed " + std::to_string (payloadBytes) + " bytes, over the " +
                std::to_string (MaxPayloadBytes) + "-byte limit.";
        return false;
    }

    header.protocolVersion = version;
    header.messageType = (MessageType) type;
    header.requestId = ReadUInt32 (bytes + 8);
    header.correlationId = ReadUInt32 (bytes + 12);
    header.payloadBytes = payloadBytes;
    return true;
}

std::vector<uint8_t> EncodeHelloPayload (const HelloPayload& hello)
{
    std::vector<uint8_t> payload;
    payload.reserve (8);
    AppendUInt32 (payload, hello.processId);
    AppendUInt32 (payload, hello.capabilities);
    return payload;
}

bool DecodeHelloPayload (const uint8_t* bytes, size_t size, HelloPayload& hello, std::string& error)
{
    if (bytes == nullptr || size < 8) {
        error = "The worker's hello was short.";
        return false;
    }

    const uint32_t processId = ReadUInt32 (bytes);
    if (processId == 0) {
        // The host supervises by pid: kill, restart generation and every log
        // line depend on it, so a hello without one is not a worker this host
        // can own.
        error = "The worker's hello carried no process id.";
        return false;
    }

    hello.processId = processId;
    hello.capabilities = ReadUInt32 (bytes + 4);
    return true;
}

std::vector<uint8_t> EncodeApiRequestPayload (const ApiRequestPayload& request)
{
    std::vector<uint8_t> payload;
    payload.reserve (8 + request.command.size () + request.parameters.size ());
    AppendUInt32 (payload, (uint32_t) request.command.size ());
    AppendUInt32 (payload, (uint32_t) request.parameters.size ());
    AppendBytes (payload, request.command);
    AppendBytes (payload, request.parameters);
    return payload;
}

bool DecodeApiRequestPayload (const uint8_t* bytes, size_t size, ApiRequestPayload& request, std::string& error)
{
    if (bytes == nullptr || size < 8) {
        error = "The API request payload was short.";
        return false;
    }

    const uint32_t commandBytes = ReadUInt32 (bytes);
    const uint32_t parameterBytes = ReadUInt32 (bytes + 4);
    if (commandBytes == 0) {
        error = "The API request carried no command name.";
        return false;
    }
    if (commandBytes > MaxCommandBytes) {
        error = "The API request's command name claimed " + std::to_string (commandBytes) + " bytes, over the " +
                std::to_string (MaxCommandBytes) + "-byte limit.";
        return false;
    }
    // The declared lengths are reconciled against what actually ARRIVED, not
    // against each other: the header's payloadBytes is the only authority for
    // how much there is, and a message that describes more than it carries is
    // the classic over-read.
    if ((uint64_t) commandBytes + (uint64_t) parameterBytes + 8u != (uint64_t) size) {
        error = "The API request's declared lengths do not match its payload.";
        return false;
    }

    request.command.assign ((const char*) bytes + 8, commandBytes);
    request.parameters.assign ((const char*) bytes + 8 + commandBytes, parameterBytes);
    if (ContainsNul (request.command) || ContainsNul (request.parameters)) {
        error = "The API request contained an embedded NUL.";
        return false;
    }
    return true;
}

std::vector<uint8_t> EncodeAckPayload (const AckPayload& ack)
{
    std::vector<uint8_t> payload;
    payload.reserve (4 + ack.message.size ());
    AppendUInt32 (payload, (uint32_t) ack.status);
    AppendBytes (payload, ack.message);
    return payload;
}

bool DecodeAckPayload (const uint8_t* bytes, size_t size, AckPayload& ack, std::string& error)
{
    if (bytes == nullptr || size < 4) {
        error = "The acknowledgement payload was short.";
        return false;
    }

    const uint32_t status = ReadUInt32 (bytes);
    if (status > (uint32_t) AckStatus::NotReady) {
        error = "The acknowledgement carried an unknown status " + std::to_string (status) + ".";
        return false;
    }

    ack.status = (AckStatus) status;
    ack.message.assign ((const char*) bytes + 4, size - 4);
    if (ContainsNul (ack.message)) {
        error = "The acknowledgement message contained an embedded NUL.";
        return false;
    }
    return true;
}

namespace {

// Every string in a run report is length-prefixed and every length is checked
// against what is LEFT, not against the whole payload: a count that overruns by
// one entry is the ordinary way a list codec reads off the end.
bool ReadString (const uint8_t* bytes, size_t size, size_t& offset, std::string& text, std::string& error)
{
    if (offset + 4 > size) {
        error = "The run report ended inside a string length.";
        return false;
    }
    const uint32_t length = ReadUInt32 (bytes + offset);
    offset += 4;
    if (length > size || offset + length > size) {
        error = "The run report declared a string longer than the payload that carries it.";
        return false;
    }
    text.assign ((const char*) bytes + offset, length);
    offset += length;
    if (ContainsNul (text)) {
        error = "The run report contained an embedded NUL.";
        return false;
    }
    return true;
}

void AppendString (std::vector<uint8_t>& buffer, const std::string& text)
{
    AppendUInt32 (buffer, (uint32_t) text.size ());
    AppendBytes (buffer, text);
}

} // namespace

std::vector<uint8_t> EncodeRunReportPayload (const RunReportPayload& report)
{
    std::vector<uint8_t> payload;
    AppendUInt32 (payload, report.ok ? 1u : 0u);
    AppendUInt32 (payload, report.elapsedMs);
    AppendUInt32 (payload, (uint32_t) report.errors.size ());
    AppendUInt32 (payload, (uint32_t) report.warnings.size ());
    AppendUInt32 (payload, (uint32_t) report.ledger.size ());
    AppendString (payload, report.headline);
    for (const std::string& text : report.errors)
        AppendString (payload, text);
    for (const std::string& text : report.warnings)
        AppendString (payload, text);
    for (const UndoLedgerEntry& entry : report.ledger) {
        AppendString (payload, entry.command);
        AppendUInt32 (payload, entry.invocations);
    }
    return payload;
}

bool DecodeRunReportPayload (const uint8_t* bytes, size_t size, RunReportPayload& report, std::string& error)
{
    if (bytes == nullptr || size < 20) {
        error = "The run report payload was short.";
        return false;
    }

    RunReportPayload decoded;
    decoded.ok = ReadUInt32 (bytes) != 0;
    decoded.elapsedMs = ReadUInt32 (bytes + 4);
    const uint32_t errorCount = ReadUInt32 (bytes + 8);
    const uint32_t warningCount = ReadUInt32 (bytes + 12);
    const uint32_t ledgerCount = ReadUInt32 (bytes + 16);

    // Before the loops, not inside them: each entry costs at least four bytes,
    // so a count that cannot fit is a corrupt count and must be refused rather
    // than reserved for. A ledger entry costs at least eight — a length, a
    // name and a count — so it is weighted accordingly.
    if ((uint64_t) errorCount + (uint64_t) warningCount + (uint64_t) ledgerCount * 2u > (uint64_t) size / 4u) {
        error = "The run report declared more messages than its payload can hold.";
        return false;
    }

    size_t offset = 20;
    if (!ReadString (bytes, size, offset, decoded.headline, error))
        return false;

    decoded.errors.resize (errorCount);
    for (uint32_t index = 0; index < errorCount; ++index) {
        if (!ReadString (bytes, size, offset, decoded.errors[index], error))
            return false;
    }
    decoded.warnings.resize (warningCount);
    for (uint32_t index = 0; index < warningCount; ++index) {
        if (!ReadString (bytes, size, offset, decoded.warnings[index], error))
            return false;
    }

    decoded.ledger.resize (ledgerCount);
    for (uint32_t index = 0; index < ledgerCount; ++index) {
        if (!ReadString (bytes, size, offset, decoded.ledger[index].command, error))
            return false;
        if (offset + 4 > size) {
            error = "The run report ended inside a ledger count.";
            return false;
        }
        decoded.ledger[index].invocations = ReadUInt32 (bytes + offset);
        offset += 4;
    }

    report = decoded;
    return true;
}

std::string DescribeRunReport (const RunReportPayload& report)
{
    std::string text = report.headline;
    text += "\n\nSolved in " + std::to_string (report.elapsedMs) + " ms.";

    // Errors before warnings, and both before anything else: a user who pressed
    // Run wants to know what broke, not how long it took to break.
    if (!report.errors.empty ()) {
        text += "\n\nErrors:";
        for (const std::string& line : report.errors)
            text += "\n  " + line;
    }
    if (!report.warnings.empty ()) {
        text += "\n\nWarnings:";
        for (const std::string& line : report.warnings)
            text += "\n  " + line;
    }
    if (report.errors.empty () && report.warnings.empty ())
        text += "\nNo component reported an error or a warning.";

    // The undo cost, when the run was measured. Empty ledger means the Tapir
    // proxy was off, which is the default — and silence is the right answer
    // there, rather than a reassuring "0 undo steps" that was never counted.
    if (!report.ledger.empty ())
        text += "\n\n" + DescribeUndoBudget (EvaluateUndoBudget (report.ledger));

    return text;
}

std::vector<uint8_t> EncodeTextPayload (const std::string& utf8)
{
    return std::vector<uint8_t> (utf8.begin (), utf8.end ());
}

bool DecodeTextPayload (const uint8_t* bytes, size_t size, std::string& utf8, std::string& error)
{
    if (size == 0) {
        utf8.clear ();
        return true;
    }
    if (bytes == nullptr) {
        error = "The text payload was missing.";
        return false;
    }

    utf8.assign ((const char*) bytes, size);
    if (ContainsNul (utf8)) {
        error = "The text payload contained an embedded NUL.";
        return false;
    }
    return true;
}

bool AcceptsVersion (uint32_t workerVersion, std::string& refusal)
{
    if (workerVersion == Version)
        return true;

    // Both numbers, always. "Protocol mismatch" on its own has sent people off
    // to reinstall Rhino before now; the pair says immediately which half is
    // the stale one.
    refusal = "The Grasshopper worker speaks bridge protocol " + std::to_string (workerVersion) +
              " and this add-on speaks " + std::to_string (Version) + ". Rebuild and redeploy both halves together.";
    return false;
}

const char* DescribeMessageType (MessageType type)
{
    switch (type) {
        case MessageType::Hello:
            return "hello";
        case MessageType::HelloAck:
            return "hello-ack";
        case MessageType::Heartbeat:
            return "heartbeat";
        case MessageType::ApiRequest:
            return "api-request";
        case MessageType::ApiResponse:
            return "api-response";
        case MessageType::ShowEditor:
            return "show-editor";
        case MessageType::HideEditor:
            return "hide-editor";
        case MessageType::Shutdown:
            return "shutdown";
        case MessageType::Ack:
            return "ack";
        case MessageType::Log:
            return "log";
        case MessageType::RunDefinition:
            return "run-definition";
        case MessageType::CancelRun:
            return "cancel-run";
        case MessageType::RunResult:
            return "run-result";
        case MessageType::PreviewBeginBatch:
            return "preview-begin-batch";
        case MessageType::PreviewAdded:
            return "preview-added";
        case MessageType::PreviewChanged:
            return "preview-changed";
        case MessageType::PreviewRemoved:
            return "preview-removed";
        case MessageType::PreviewVisibility:
            return "preview-visibility";
        case MessageType::PreviewSelection:
            return "preview-selection";
        case MessageType::PreviewEndBatch:
            return "preview-end-batch";
        case MessageType::PreviewDropAll:
            return "preview-drop-all";
        case MessageType::PreviewResyncRequest:
            return "preview-resync-request";
        case MessageType::PreviewBatchAck:
            return "preview-batch-ack";
        case MessageType::PreviewPicked:
            return "preview-picked";
    }
    return "unknown";
}

} // namespace protocol
} // namespace grasshopper
} // namespace evp
