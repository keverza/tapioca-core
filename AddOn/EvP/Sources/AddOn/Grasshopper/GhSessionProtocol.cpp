#include "GhSessionProtocol.hpp"

namespace evp {
namespace grasshopper {
namespace protocol {

namespace {

// Byte by byte, for the reason GhProtocol.cpp gives: a packed struct would make
// the wire format depend on the compiler that built each half, and the two
// halves here are a C++ .apx and a C# worker that do not share one.
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

bool ContainsNul (const std::string& text)
{
    return text.find (static_cast<char> (0)) != std::string::npos;
}

void AppendString (std::vector<uint8_t>& buffer, const std::string& text)
{
    AppendUInt32 (buffer, (uint32_t) text.size ());
    buffer.insert (buffer.end (), text.begin (), text.end ());
}

// ⚠️ EVERY LENGTH IS CHECKED AGAINST WHAT IS LEFT, NOT AGAINST THE WHOLE
// PAYLOAD, AND AGAINST ITS OWN CEILING BEFORE THE STRING IS BUILT. A count that
// overruns by one entry is the ordinary way a list codec reads off the end, and
// `offset + length` on 32-bit values promoted to size_t is where the arithmetic
// is done so it cannot wrap.
bool ReadString (const uint8_t* bytes, size_t size, size_t& offset, uint32_t maxBytes, const char* what,
                 std::string& text, std::string& error)
{
    if (offset + 4 > size) {
        error = std::string ("The message ended inside the length of ") + what + ".";
        return false;
    }

    const uint32_t length = ReadUInt32 (bytes + offset);
    offset += 4;
    if (length > maxBytes) {
        error = std::string ("The message's ") + what + " claimed " + std::to_string (length) + " bytes, over the " +
                std::to_string (maxBytes) + "-byte limit.";
        return false;
    }
    if (offset + (size_t) length > size) {
        error = std::string ("The message declared more ") + what + " than it carried.";
        return false;
    }

    text.assign ((const char*) bytes + offset, length);
    offset += length;
    if (ContainsNul (text)) {
        error = std::string ("The message's ") + what + " contained an embedded NUL.";
        return false;
    }
    return true;
}

void AppendEnvelope (std::vector<uint8_t>& buffer, const SessionEnvelope& envelope)
{
    AppendUInt32 (buffer, envelope.hostGeneration);
    AppendUInt32 (buffer, envelope.sessionId);
    AppendUInt32 (buffer, envelope.requestRevision);
}

// The envelope first, always, and refused before anything after it is read.
bool ReadEnvelope (const uint8_t* bytes, size_t size, size_t& offset, SessionEnvelope& envelope, std::string& error)
{
    if (bytes == nullptr || size < SessionEnvelopeSize) {
        error = "The session message was shorter than its routing envelope.";
        return false;
    }

    envelope.hostGeneration = ReadUInt32 (bytes);
    envelope.sessionId = ReadUInt32 (bytes + 4);
    envelope.requestRevision = ReadUInt32 (bytes + 8);
    offset = SessionEnvelopeSize;

    // ⚠️ GENERATION 0 IS NOT A SESSION MESSAGE. The generation is what makes a
    // result from a dead worker droppable (§7); a message that does not carry
    // one cannot be placed in time at all, and accepting it would mean
    // publishing a solution whose provenance is unknown.
    if (envelope.hostGeneration == 0) {
        error = "The session message carried no host generation.";
        return false;
    }
    return true;
}

// The trailing envelope-only shape, shared by the five messages that are nothing
// but routing. Written once rather than five times: they differ only in the noun
// their error message names.
std::vector<uint8_t> EncodeEnvelopeOnly (const SessionEnvelope& envelope)
{
    std::vector<uint8_t> payload;
    payload.reserve (SessionEnvelopeSize);
    AppendEnvelope (payload, envelope);
    return payload;
}

bool DecodeEnvelopeOnly (const uint8_t* bytes, size_t size, const char* what, SessionEnvelope& envelope,
                         std::string& error)
{
    size_t offset = 0;
    if (!ReadEnvelope (bytes, size, offset, envelope, error))
        return false;
    if (size != SessionEnvelopeSize) {
        error = std::string ("The ") + what + " carried " + std::to_string (size) + " bytes where " +
                std::to_string (SessionEnvelopeSize) + " were expected.";
        return false;
    }
    return true;
}

bool ReadDiagnostics (const uint8_t* bytes, size_t size, size_t& offset, uint32_t count,
                      std::vector<SessionDiagnostic>& diagnostics, std::string& error)
{
    if (count > MaxSessionDiagnostics) {
        error = "The message declared " + std::to_string (count) + " diagnostics, over the " +
                std::to_string (MaxSessionDiagnostics) + " limit.";
        return false;
    }

    diagnostics.clear ();
    diagnostics.reserve (count);
    for (uint32_t index = 0; index < count; ++index) {
        if (offset + 4 > size) {
            error = "The message ended inside a diagnostic level.";
            return false;
        }

        const uint32_t level = ReadUInt32 (bytes + offset);
        offset += 4;
        if (!KnownDiagnosticLevel (level)) {
            error = "A diagnostic carried an unknown level " + std::to_string (level) + ".";
            return false;
        }

        SessionDiagnostic diagnostic;
        diagnostic.level = (DiagnosticLevel) level;
        if (!ReadString (bytes, size, offset, MaxSessionTextBytes, "a diagnostic's component", diagnostic.component,
                         error))
            return false;
        if (!ReadString (bytes, size, offset, MaxSessionTextBytes, "a diagnostic's text", diagnostic.text, error))
            return false;

        diagnostics.push_back (std::move (diagnostic));
    }
    return true;
}

void AppendDiagnostics (std::vector<uint8_t>& payload, const std::vector<SessionDiagnostic>& diagnostics)
{
    for (const SessionDiagnostic& diagnostic : diagnostics) {
        AppendUInt32 (payload, (uint32_t) diagnostic.level);
        AppendString (payload, diagnostic.component);
        AppendString (payload, diagnostic.text);
    }
}

// Nothing may follow the last field a message declares. A payload with a tail
// nobody reads is either a version skew this handshake was supposed to have
// caught or a sender writing a shape this build does not have; both are worth a
// refusal rather than a silent ignore.
bool AtEnd (size_t offset, size_t size, const char* what, std::string& error)
{
    if (offset == size)
        return true;
    error = std::string ("The ") + what + " carried " + std::to_string (size - offset) +
            " trailing bytes this build does not know.";
    return false;
}

} // namespace

bool KnownSessionMode (uint32_t value)
{
    return value <= (uint32_t) SessionMode::Authoring;
}

bool KnownSessionState (uint32_t value)
{
    return value <= (uint32_t) SessionState::HostRestartRequired;
}

bool KnownFailureCode (uint32_t value)
{
    return value <= (uint32_t) FailureCode::SessionUnknown;
}

bool KnownDiagnosticLevel (uint32_t value)
{
    return value <= (uint32_t) DiagnosticLevel::Remark;
}

std::vector<uint8_t> EncodeOpenSessionPayload (const OpenSessionPayload& message)
{
    std::vector<uint8_t> payload;
    payload.reserve (SessionEnvelopeSize + 4);
    AppendEnvelope (payload, message.envelope);
    AppendUInt32 (payload, (uint32_t) message.mode);
    return payload;
}

bool DecodeOpenSessionPayload (const uint8_t* bytes, size_t size, OpenSessionPayload& message, std::string& error)
{
    size_t offset = 0;
    SessionEnvelope envelope;
    if (!ReadEnvelope (bytes, size, offset, envelope, error))
        return false;
    if (offset + 4 > size) {
        error = "The open-session request ended before its mode.";
        return false;
    }

    const uint32_t mode = ReadUInt32 (bytes + offset);
    offset += 4;
    if (!KnownSessionMode (mode)) {
        error = "The open-session request asked for an unknown mode " + std::to_string (mode) + ".";
        return false;
    }
    if (!AtEnd (offset, size, "open-session request", error))
        return false;

    // ⚠️ A SESSION ID OF 0 IS REFUSED HERE AND NOWHERE ELSE. The host assigns
    // ids, so 0 means "the host did not fill it in" -- and every later message
    // for that session would then address a session nothing owns. Catching it at
    // the one message that CREATES a session is what makes the rest of the file
    // able to treat sessionId as trustworthy routing.
    if (envelope.sessionId == 0) {
        error = "The open-session request carried no session id.";
        return false;
    }

    message.envelope = envelope;
    message.mode = (SessionMode) mode;
    return true;
}

std::vector<uint8_t> EncodeCloseSessionPayload (const CloseSessionPayload& message)
{
    return EncodeEnvelopeOnly (message.envelope);
}

bool DecodeCloseSessionPayload (const uint8_t* bytes, size_t size, CloseSessionPayload& message, std::string& error)
{
    return DecodeEnvelopeOnly (bytes, size, "close-session request", message.envelope, error);
}

std::vector<uint8_t> EncodeSetSessionModePayload (const SetSessionModePayload& message)
{
    std::vector<uint8_t> payload;
    payload.reserve (SessionEnvelopeSize + 4);
    AppendEnvelope (payload, message.envelope);
    AppendUInt32 (payload, (uint32_t) message.mode);
    return payload;
}

bool DecodeSetSessionModePayload (const uint8_t* bytes, size_t size, SetSessionModePayload& message, std::string& error)
{
    size_t offset = 0;
    if (!ReadEnvelope (bytes, size, offset, message.envelope, error))
        return false;
    if (offset + 4 > size) {
        error = "The set-mode request ended before its mode.";
        return false;
    }

    const uint32_t mode = ReadUInt32 (bytes + offset);
    offset += 4;
    if (!KnownSessionMode (mode)) {
        error = "The set-mode request asked for an unknown mode " + std::to_string (mode) + ".";
        return false;
    }
    if (!AtEnd (offset, size, "set-mode request", error))
        return false;

    message.mode = (SessionMode) mode;
    return true;
}

std::vector<uint8_t> EncodeLoadDefinitionPayload (const LoadDefinitionPayload& message)
{
    std::vector<uint8_t> payload;
    payload.reserve (SessionEnvelopeSize + 4 + message.path.size ());
    AppendEnvelope (payload, message.envelope);
    AppendString (payload, message.path);
    return payload;
}

bool DecodeLoadDefinitionPayload (const uint8_t* bytes, size_t size, LoadDefinitionPayload& message, std::string& error)
{
    size_t offset = 0;
    if (!ReadEnvelope (bytes, size, offset, message.envelope, error))
        return false;
    if (!ReadString (bytes, size, offset, MaxDefinitionPathBytes, "the definition path", message.path, error))
        return false;
    if (message.path.empty ()) {
        error = "The load request carried no definition path.";
        return false;
    }
    return AtEnd (offset, size, "load request", error);
}

std::vector<uint8_t> EncodeReloadDefinitionPayload (const ReloadDefinitionPayload& message)
{
    return EncodeEnvelopeOnly (message.envelope);
}

bool DecodeReloadDefinitionPayload (const uint8_t* bytes, size_t size, ReloadDefinitionPayload& message,
                                    std::string& error)
{
    return DecodeEnvelopeOnly (bytes, size, "reload request", message.envelope, error);
}

std::vector<uint8_t> EncodeGetSchemaPayload (const GetSchemaPayload& message)
{
    return EncodeEnvelopeOnly (message.envelope);
}

bool DecodeGetSchemaPayload (const uint8_t* bytes, size_t size, GetSchemaPayload& message, std::string& error)
{
    return DecodeEnvelopeOnly (bytes, size, "schema request", message.envelope, error);
}

std::vector<uint8_t> EncodeSessionAckPayload (const SessionAckPayload& message)
{
    std::vector<uint8_t> payload;
    payload.reserve (SessionEnvelopeSize + 12 + message.message.size ());
    AppendEnvelope (payload, message.envelope);
    AppendUInt32 (payload, (uint32_t) message.state);
    AppendUInt32 (payload, (uint32_t) message.failure);
    AppendString (payload, message.message);
    return payload;
}

bool DecodeSessionAckPayload (const uint8_t* bytes, size_t size, SessionAckPayload& message, std::string& error)
{
    size_t offset = 0;
    if (!ReadEnvelope (bytes, size, offset, message.envelope, error))
        return false;
    if (offset + 8 > size) {
        error = "The session acknowledgement ended before its state and failure code.";
        return false;
    }

    const uint32_t state = ReadUInt32 (bytes + offset);
    const uint32_t failure = ReadUInt32 (bytes + offset + 4);
    offset += 8;
    if (!KnownSessionState (state)) {
        error = "The session acknowledgement carried an unknown state " + std::to_string (state) + ".";
        return false;
    }
    if (!KnownFailureCode (failure)) {
        error = "The session acknowledgement carried an unknown failure code " + std::to_string (failure) + ".";
        return false;
    }
    if (!ReadString (bytes, size, offset, MaxSessionTextBytes, "the acknowledgement message", message.message, error))
        return false;
    if (!AtEnd (offset, size, "session acknowledgement", error))
        return false;

    message.state = (SessionState) state;
    message.failure = (FailureCode) failure;
    return true;
}

std::vector<uint8_t> EncodeSchemaResultPayload (const SchemaResultPayload& message)
{
    std::vector<uint8_t> payload;
    payload.reserve (SessionEnvelopeSize + 8 + message.definitionIdentity.size () + message.schemaJson.size ());
    AppendEnvelope (payload, message.envelope);
    AppendString (payload, message.definitionIdentity);
    AppendString (payload, message.schemaJson);
    return payload;
}

bool DecodeSchemaResultPayload (const uint8_t* bytes, size_t size, SchemaResultPayload& message, std::string& error)
{
    size_t offset = 0;
    if (!ReadEnvelope (bytes, size, offset, message.envelope, error))
        return false;
    if (!ReadString (bytes, size, offset, MaxSessionTextBytes, "the definition identity", message.definitionIdentity,
                     error))
        return false;
    if (!ReadString (bytes, size, offset, MaxSchemaBytes, "the workflow schema", message.schemaJson, error))
        return false;
    return AtEnd (offset, size, "schema result", error);
}

std::vector<uint8_t> EncodeSetInputsPayload (const SetInputsPayload& message)
{
    std::vector<uint8_t> payload;
    AppendEnvelope (payload, message.envelope);
    AppendUInt32 (payload, (uint32_t) message.inputs.size ());
    for (const SessionInputValue& input : message.inputs) {
        AppendString (payload, input.id);
        AppendString (payload, input.value);
    }
    return payload;
}

bool DecodeSetInputsPayload (const uint8_t* bytes, size_t size, SetInputsPayload& message, std::string& error)
{
    size_t offset = 0;
    if (!ReadEnvelope (bytes, size, offset, message.envelope, error))
        return false;
    if (offset + 4 > size) {
        error = "The set-inputs request ended before its input count.";
        return false;
    }

    const uint32_t count = ReadUInt32 (bytes + offset);
    offset += 4;
    if (count > MaxSessionInputs) {
        error = "The set-inputs request declared " + std::to_string (count) + " inputs, over the " +
                std::to_string (MaxSessionInputs) + " limit.";
        return false;
    }

    std::vector<SessionInputValue> inputs;
    inputs.reserve (count);
    for (uint32_t index = 0; index < count; ++index) {
        SessionInputValue input;
        if (!ReadString (bytes, size, offset, MaxSessionTextBytes, "an input id", input.id, error))
            return false;
        if (input.id.empty ()) {
            error = "The set-inputs request carried a value with no input id.";
            return false;
        }
        if (!ReadString (bytes, size, offset, MaxSessionTextBytes, "an input value", input.value, error))
            return false;
        inputs.push_back (std::move (input));
    }

    if (!AtEnd (offset, size, "set-inputs request", error))
        return false;

    message.inputs = std::move (inputs);
    return true;
}

std::vector<uint8_t> EncodeSolvePayload (const SolvePayload& message)
{
    std::vector<uint8_t> payload;
    payload.reserve (SessionEnvelopeSize + 4);
    AppendEnvelope (payload, message.envelope);
    AppendUInt32 (payload, message.wants);
    return payload;
}

bool DecodeSolvePayload (const uint8_t* bytes, size_t size, SolvePayload& message, std::string& error)
{
    size_t offset = 0;
    if (!ReadEnvelope (bytes, size, offset, message.envelope, error))
        return false;
    if (offset + 4 > size) {
        error = "The solve request ended before its wants word.";
        return false;
    }

    const uint32_t wants = ReadUInt32 (bytes + offset);
    offset += 4;
    // An unknown BIT is refused, unlike an unknown capability bit in the
    // handshake: capabilities are additive within a version and a solve's wants
    // are not. A bit this build cannot honour would make the worker collect less
    // than the sender asked for while reporting success.
    if ((wants & ~SolveWantsAll) != 0) {
        error = "The solve request asked for collection this build does not know: " + std::to_string (wants) + ".";
        return false;
    }
    if (wants == SolveWantsNothing) {
        error = "The solve request asked for nothing to be collected.";
        return false;
    }
    if (!AtEnd (offset, size, "solve request", error))
        return false;

    message.wants = wants;
    return true;
}

std::vector<uint8_t> EncodeCancelSolvePayload (const CancelSolvePayload& message)
{
    return EncodeEnvelopeOnly (message.envelope);
}

bool DecodeCancelSolvePayload (const uint8_t* bytes, size_t size, CancelSolvePayload& message, std::string& error)
{
    return DecodeEnvelopeOnly (bytes, size, "cancel request", message.envelope, error);
}

std::vector<uint8_t> EncodeSolutionStartedPayload (const SolutionStartedPayload& message)
{
    return EncodeEnvelopeOnly (message.envelope);
}

bool DecodeSolutionStartedPayload (const uint8_t* bytes, size_t size, SolutionStartedPayload& message,
                                   std::string& error)
{
    return DecodeEnvelopeOnly (bytes, size, "solution-started notice", message.envelope, error);
}

std::vector<uint8_t> EncodeSolutionResultPayload (const SolutionResultPayload& message)
{
    std::vector<uint8_t> payload;
    AppendEnvelope (payload, message.envelope);
    AppendUInt32 (payload, message.solutionRevision);
    AppendUInt32 (payload, message.elapsedMs);
    AppendUInt32 (payload, message.previewEpoch);
    AppendUInt32 (payload, message.previewRevision);
    AppendUInt32 (payload, (uint32_t) message.outputs.size ());
    AppendUInt32 (payload, (uint32_t) message.diagnostics.size ());
    AppendString (payload, message.definitionIdentity);
    AppendString (payload, message.inputSnapshotHash);
    for (const SessionOutputValue& output : message.outputs) {
        AppendString (payload, output.id);
        AppendString (payload, output.type);
        AppendString (payload, output.path);
        AppendString (payload, output.value);
    }
    AppendDiagnostics (payload, message.diagnostics);
    return payload;
}

bool DecodeSolutionResultPayload (const uint8_t* bytes, size_t size, SolutionResultPayload& message, std::string& error)
{
    size_t offset = 0;
    SolutionResultPayload decoded;
    if (!ReadEnvelope (bytes, size, offset, decoded.envelope, error))
        return false;
    if (offset + 24 > size) {
        error = "The solution result ended inside its fixed header.";
        return false;
    }

    decoded.solutionRevision = ReadUInt32 (bytes + offset);
    decoded.elapsedMs = ReadUInt32 (bytes + offset + 4);
    decoded.previewEpoch = ReadUInt32 (bytes + offset + 8);
    decoded.previewRevision = ReadUInt32 (bytes + offset + 12);
    const uint32_t outputCount = ReadUInt32 (bytes + offset + 16);
    const uint32_t diagnosticCount = ReadUInt32 (bytes + offset + 20);
    offset += 24;

    // ⚠️ A SOLUTION REVISION OF 0 IS NOT A SOLUTION. Apply addresses
    // {hostGeneration, sessionId, solutionRevision} (§7); a zero there would be
    // an address the store can neither hold nor refuse meaningfully.
    if (decoded.solutionRevision == 0) {
        error = "The solution result carried no solution revision.";
        return false;
    }
    if (outputCount > MaxSessionOutputs) {
        error = "The solution result declared " + std::to_string (outputCount) + " outputs, over the " +
                std::to_string (MaxSessionOutputs) + " limit.";
        return false;
    }

    if (!ReadString (bytes, size, offset, MaxSessionTextBytes, "the definition identity", decoded.definitionIdentity,
                     error))
        return false;
    if (!ReadString (bytes, size, offset, MaxSessionTextBytes, "the input snapshot hash", decoded.inputSnapshotHash,
                     error))
        return false;

    decoded.outputs.reserve (outputCount);
    for (uint32_t index = 0; index < outputCount; ++index) {
        SessionOutputValue output;
        if (!ReadString (bytes, size, offset, MaxSessionTextBytes, "an output id", output.id, error))
            return false;
        if (!ReadString (bytes, size, offset, MaxSessionTextBytes, "an output type", output.type, error))
            return false;
        if (!ReadString (bytes, size, offset, MaxSessionTextBytes, "an output tree path", output.path, error))
            return false;
        if (!ReadString (bytes, size, offset, MaxSessionTextBytes, "an output value", output.value, error))
            return false;
        decoded.outputs.push_back (std::move (output));
    }

    if (!ReadDiagnostics (bytes, size, offset, diagnosticCount, decoded.diagnostics, error))
        return false;
    if (!AtEnd (offset, size, "solution result", error))
        return false;

    message = std::move (decoded);
    return true;
}

std::vector<uint8_t> EncodeSolutionFailedPayload (const SolutionFailedPayload& message)
{
    std::vector<uint8_t> payload;
    AppendEnvelope (payload, message.envelope);
    AppendUInt32 (payload, (uint32_t) message.failure);
    AppendUInt32 (payload, message.elapsedMs);
    AppendUInt32 (payload, (uint32_t) message.diagnostics.size ());
    AppendString (payload, message.message);
    AppendDiagnostics (payload, message.diagnostics);
    return payload;
}

bool DecodeSolutionFailedPayload (const uint8_t* bytes, size_t size, SolutionFailedPayload& message, std::string& error)
{
    size_t offset = 0;
    SolutionFailedPayload decoded;
    if (!ReadEnvelope (bytes, size, offset, decoded.envelope, error))
        return false;
    if (offset + 12 > size) {
        error = "The solution failure ended inside its fixed header.";
        return false;
    }

    const uint32_t failure = ReadUInt32 (bytes + offset);
    decoded.elapsedMs = ReadUInt32 (bytes + offset + 4);
    const uint32_t diagnosticCount = ReadUInt32 (bytes + offset + 8);
    offset += 12;

    if (!KnownFailureCode (failure)) {
        error = "The solution failure carried an unknown failure code " + std::to_string (failure) + ".";
        return false;
    }
    // A failure that reports None is a message with no content: the panel would
    // show a category-free error and the log would record one.
    if ((FailureCode) failure == FailureCode::None) {
        error = "The solution failure carried no failure code.";
        return false;
    }

    if (!ReadString (bytes, size, offset, MaxSessionTextBytes, "the failure message", decoded.message, error))
        return false;
    if (!ReadDiagnostics (bytes, size, offset, diagnosticCount, decoded.diagnostics, error))
        return false;
    if (!AtEnd (offset, size, "solution failure", error))
        return false;

    decoded.failure = (FailureCode) failure;
    message = std::move (decoded);
    return true;
}

std::vector<uint8_t> EncodeGetDiagnosticsPayload (const GetDiagnosticsPayload& message)
{
    return EncodeEnvelopeOnly (message.envelope);
}

bool DecodeGetDiagnosticsPayload (const uint8_t* bytes, size_t size, GetDiagnosticsPayload& message, std::string& error)
{
    return DecodeEnvelopeOnly (bytes, size, "diagnostics request", message.envelope, error);
}

std::vector<uint8_t> EncodeDiagnosticsResultPayload (const DiagnosticsResultPayload& message)
{
    std::vector<uint8_t> payload;
    payload.reserve (SessionEnvelopeSize + 4 + message.report.size ());
    AppendEnvelope (payload, message.envelope);
    AppendString (payload, message.report);
    return payload;
}

bool DecodeDiagnosticsResultPayload (const uint8_t* bytes, size_t size, DiagnosticsResultPayload& message,
                                     std::string& error)
{
    size_t offset = 0;
    if (!ReadEnvelope (bytes, size, offset, message.envelope, error))
        return false;
    if (!ReadString (bytes, size, offset, MaxSchemaBytes, "the diagnostics report", message.report, error))
        return false;
    return AtEnd (offset, size, "diagnostics result", error);
}

const char* DescribeSessionState (SessionState state)
{
    switch (state) {
        case SessionState::Empty:
            return "empty";
        case SessionState::Loading:
            return "loading";
        case SessionState::Loaded:
            return "loaded";
        case SessionState::Solving:
            return "solving";
        case SessionState::Published:
            return "published";
        case SessionState::Invalid:
            return "invalid";
        case SessionState::Cancelling:
            return "cancelling";
        case SessionState::Reloading:
            return "reloading";
        case SessionState::HostRestartRequired:
            return "host-restart-required";
    }
    return "unknown";
}

const char* DescribeSessionMode (SessionMode mode)
{
    switch (mode) {
        case SessionMode::Headless:
            return "headless";
        case SessionMode::Authoring:
            return "authoring";
    }
    return "unknown";
}

// ⚠️ ONE SENTENCE PER CATEGORY, AND IT IS THE ONE THE PANEL SHOWS. §11: "The
// panel shows one concise status plus a diagnostics action." Anything longer
// belongs in the diagnostics block, and anything that names an internal symbol
// belongs there too.
const char* DescribeFailureCode (FailureCode failure)
{
    switch (failure) {
        case FailureCode::None:
            return "no failure";
        case FailureCode::RuntimeMissing:
            return "Rhino 8 was not found on this machine";
        case FailureCode::LicenceUnavailable:
            return "Rhino is installed but no licence is available";
        case FailureCode::RuntimeStartFailed:
            return "Rhino would not start";
        case FailureCode::DefinitionInvalid:
            return "the definition could not be read";
        case FailureCode::DependencyMissing:
            return "the definition needs a Grasshopper package that is not installed";
        case FailureCode::ContractInvalid:
            return "the definition's Tapioca inputs or outputs are not usable";
        case FailureCode::InputInvalid:
            return "an input value was rejected";
        case FailureCode::SolveFailed:
            return "the solution failed";
        case FailureCode::SolveTimedOut:
            return "the solution ran past its time limit";
        case FailureCode::Cancelled:
            return "the solution was cancelled";
        case FailureCode::HostDisconnected:
            return "the Grasshopper host disconnected";
        case FailureCode::HostCrashed:
            return "the Grasshopper host stopped unexpectedly";
        case FailureCode::PreviewInvalid:
            return "the preview this solution produced could not be read";
        case FailureCode::CommitInvalid:
            return "the commit this solution produced did not validate";
        case FailureCode::StaleRevision:
            return "a newer request replaced this one";
        case FailureCode::AccessDenied:
            return "that is not allowed in this session mode";
        case FailureCode::CapacityExceeded:
            return "the request was over a size or rate limit";
        case FailureCode::ProtocolMismatch:
            return "the add-on and the Grasshopper host speak different protocols";
        case FailureCode::SessionUnknown:
            return "the Grasshopper host has no such session";
    }
    return "an unrecognised failure";
}

const char* DescribeDiagnosticLevel (DiagnosticLevel level)
{
    switch (level) {
        case DiagnosticLevel::Error:
            return "error";
        case DiagnosticLevel::Warning:
            return "warning";
        case DiagnosticLevel::Remark:
            return "remark";
    }
    return "unknown";
}

} // namespace protocol
} // namespace grasshopper
} // namespace evp
