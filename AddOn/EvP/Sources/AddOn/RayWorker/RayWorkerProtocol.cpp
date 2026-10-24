#include "RayWorker/RayWorkerProtocol.hpp"

namespace evp::rayworker::protocol {

namespace {

// Little-endian, byte by byte: the layout must not depend on either compiler.
class Writer {
  public:
    void U32 (uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8)
            bytes_.push_back (static_cast<uint8_t> ((value >> shift) & 0xFFu));
    }
    void U64 (uint64_t value)
    {
        for (int shift = 0; shift < 64; shift += 8)
            bytes_.push_back (static_cast<uint8_t> ((value >> shift) & 0xFFu));
    }
    void Text (const std::string& text)
    {
        U32 (static_cast<uint32_t> (text.size ()));
        bytes_.insert (bytes_.end (), text.begin (), text.end ());
    }
    void Segment (const SegmentRef& segment)
    {
        Text (segment.name);
        U64 (segment.bytes);
        U64 (segment.checksum);
    }
    std::vector<uint8_t> Take ()
    {
        return std::move (bytes_);
    }

  private:
    std::vector<uint8_t> bytes_;
};

// Bounds-checked reads; the first failure sticks and names what was short.
class Reader {
  public:
    Reader (const uint8_t* bytes, size_t size, std::string& error) : bytes_ (bytes), size_ (size), error_ (error)
    {
        if (bytes_ == nullptr && size_ != 0)
            Fail ("the payload pointer was null");
    }
    uint32_t U32 (const char* what)
    {
        uint32_t value = 0;
        if (Need (4, what))
            for (int i = 0; i < 4; ++i)
                value |= static_cast<uint32_t> (bytes_[at_++]) << (8 * i);
        return value;
    }
    uint64_t U64 (const char* what)
    {
        uint64_t value = 0;
        if (Need (8, what))
            for (int i = 0; i < 8; ++i)
                value |= static_cast<uint64_t> (bytes_[at_++]) << (8 * i);
        return value;
    }
    std::string Text (const char* what)
    {
        const uint32_t length = U32 (what);
        if (!ok_)
            return std::string ();
        // Checked before the allocation.
        if (length > MaxTextBytes) {
            Fail (std::string (what) + " claimed " + std::to_string (length) + " bytes, over the " +
                  std::to_string (MaxTextBytes) + "-byte limit");
            return std::string ();
        }
        if (!Need (length, what))
            return std::string ();
        std::string text (reinterpret_cast<const char*> (bytes_ + at_), length);
        at_ += length;
        if (text.find ('\0') != std::string::npos)
            Fail (std::string (what) + " contained a NUL byte");
        return text;
    }
    SegmentRef Segment (const char* what)
    {
        SegmentRef segment;
        segment.name = Text (what);
        segment.bytes = U64 (what);
        segment.checksum = U64 (what);
        if (ok_ && segment.name.empty ())
            Fail (std::string (what) + " named no segment");
        return segment;
    }
    GpuReason Reason ()
    {
        const uint32_t value = U32 ("reason");
        if (ok_ && !IsKnownGpuReason (value))
            Fail ("the reason code " + std::to_string (value) + " is not one this add-on knows");
        return static_cast<GpuReason> (value);
    }
    bool Finish (const char* message)
    {
        if (ok_ && at_ != size_)
            Fail (std::string (message) + " had " + std::to_string (size_ - at_) + " trailing byte(s)");
        return ok_;
    }

  private:
    bool Need (size_t count, const char* what)
    {
        if (!ok_)
            return false;
        if (size_ - at_ < count) {
            Fail (std::string ("the payload was short reading ") + what);
            return false;
        }
        return true;
    }
    void Fail (const std::string& message)
    {
        if (ok_)
            error_ = message;
        ok_ = false;
    }

    const uint8_t* bytes_;
    size_t size_;
    size_t at_ = 0;
    bool ok_ = true;
    std::string& error_;
};

bool KnownMessageType (uint32_t value)
{
    return value >= static_cast<uint32_t> (MessageType::Hello) && value <= static_cast<uint32_t> (MessageType::Log);
}

} // namespace

std::vector<uint8_t> EncodeHeader (MessageType type, uint32_t requestId, uint32_t correlationId, uint32_t payloadBytes)
{
    Writer w;
    w.U32 (Version);
    w.U32 (static_cast<uint32_t> (type));
    w.U32 (requestId);
    w.U32 (correlationId);
    w.U32 (payloadBytes);
    return w.Take ();
}

bool DecodeHeader (const uint8_t* bytes, size_t size, Header& header, std::string& error)
{
    if (bytes == nullptr || size < HeaderSize) {
        error = "The message header was short.";
        return false;
    }
    Reader r (bytes, HeaderSize, error);
    const uint32_t version = r.U32 ("version");
    // The version first: another version may use a type this build never knew.
    if (!AcceptsVersion (version, error))
        return false;
    const uint32_t type = r.U32 ("type");
    if (!KnownMessageType (type)) {
        error = "The message type " + std::to_string (type) + " is not one this add-on knows.";
        return false;
    }
    header.protocolVersion = version;
    header.messageType = static_cast<MessageType> (type);
    header.requestId = r.U32 ("requestId");
    header.correlationId = r.U32 ("correlationId");
    header.payloadBytes = r.U32 ("payloadBytes");
    if (header.payloadBytes > MaxPayloadBytes) {
        error = "The message payload claimed " + std::to_string (header.payloadBytes) + " bytes, over the " +
                std::to_string (MaxPayloadBytes) + "-byte limit.";
        return false;
    }
    return true;
}

std::vector<uint8_t> EncodeFrame (MessageType type, uint32_t requestId, uint32_t correlationId,
                                  const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> frame = EncodeHeader (type, requestId, correlationId, static_cast<uint32_t> (payload.size ()));
    frame.insert (frame.end (), payload.begin (), payload.end ());
    return frame;
}

std::vector<uint8_t> Encode (const HelloPayload& v)
{
    Writer w;
    w.U32 (v.processId);
    w.U32 (v.capabilities);
    w.U32 (static_cast<uint32_t> (v.reason));
    w.U32 (v.driverVersion);
    w.U32 (v.runtimeVersion);
    w.U32 (v.computeCapability);
    w.U64 (v.deviceMemoryBytes);
    w.Text (v.deviceName);
    return w.Take ();
}

bool Decode (const uint8_t* bytes, size_t size, HelloPayload& v, std::string& error)
{
    Reader r (bytes, size, error);
    v.processId = r.U32 ("processId");
    v.capabilities = r.U32 ("capabilities");
    v.reason = r.Reason ();
    v.driverVersion = r.U32 ("driverVersion");
    v.runtimeVersion = r.U32 ("runtimeVersion");
    v.computeCapability = r.U32 ("computeCapability");
    v.deviceMemoryBytes = r.U64 ("deviceMemoryBytes");
    v.deviceName = r.Text ("deviceName");
    return r.Finish ("Hello");
}

std::vector<uint8_t> Encode (const HelloAckPayload& v)
{
    Writer w;
    w.U32 (v.capabilities);
    w.Text (v.refusal);
    return w.Take ();
}

bool Decode (const uint8_t* bytes, size_t size, HelloAckPayload& v, std::string& error)
{
    Reader r (bytes, size, error);
    v.capabilities = r.U32 ("capabilities");
    v.refusal = r.Text ("refusal");
    return r.Finish ("HelloAck");
}

std::vector<uint8_t> Encode (const UploadScenePayload& v)
{
    Writer w;
    w.U64 (v.sceneEpoch);
    w.Segment (v.scene);
    return w.Take ();
}

bool Decode (const uint8_t* bytes, size_t size, UploadScenePayload& v, std::string& error)
{
    Reader r (bytes, size, error);
    v.sceneEpoch = r.U64 ("sceneEpoch");
    v.scene = r.Segment ("scene segment");
    return r.Finish ("UploadScene");
}

std::vector<uint8_t> Encode (const SceneReadyPayload& v)
{
    Writer w;
    w.U64 (v.sceneEpoch);
    w.U32 (v.buildMicroseconds);
    w.U64 (v.deviceBytes);
    return w.Take ();
}

bool Decode (const uint8_t* bytes, size_t size, SceneReadyPayload& v, std::string& error)
{
    Reader r (bytes, size, error);
    v.sceneEpoch = r.U64 ("sceneEpoch");
    v.buildMicroseconds = r.U32 ("buildMicroseconds");
    v.deviceBytes = r.U64 ("deviceBytes");
    return r.Finish ("SceneReady");
}

std::vector<uint8_t> Encode (const RunStudyPayload& v)
{
    Writer w;
    w.U64 (v.runGeneration);
    w.U64 (v.sceneEpoch);
    w.Segment (v.request);
    w.Segment (v.result);
    return w.Take ();
}

bool Decode (const uint8_t* bytes, size_t size, RunStudyPayload& v, std::string& error)
{
    Reader r (bytes, size, error);
    v.runGeneration = r.U64 ("runGeneration");
    v.sceneEpoch = r.U64 ("sceneEpoch");
    v.request = r.Segment ("request segment");
    v.result = r.Segment ("result segment");
    return r.Finish ("RunStudy");
}

std::vector<uint8_t> Encode (const RunResultPayload& v)
{
    Writer w;
    w.U64 (v.runGeneration);
    w.U64 (v.sceneEpoch);
    w.U64 (v.resultBytes);
    w.U64 (v.checksum);
    w.U32 (v.timings.transfer);
    w.U32 (v.timings.trace);
    w.U32 (v.timings.readback);
    return w.Take ();
}

bool Decode (const uint8_t* bytes, size_t size, RunResultPayload& v, std::string& error)
{
    Reader r (bytes, size, error);
    v.runGeneration = r.U64 ("runGeneration");
    v.sceneEpoch = r.U64 ("sceneEpoch");
    v.resultBytes = r.U64 ("resultBytes");
    v.checksum = r.U64 ("checksum");
    v.timings.transfer = r.U32 ("transfer");
    v.timings.trace = r.U32 ("trace");
    v.timings.readback = r.U32 ("readback");
    return r.Finish ("RunResult");
}

std::vector<uint8_t> Encode (const FailedPayload& v)
{
    Writer w;
    w.U32 (static_cast<uint32_t> (v.reason));
    w.Text (v.message);
    return w.Take ();
}

bool Decode (const uint8_t* bytes, size_t size, FailedPayload& v, std::string& error)
{
    Reader r (bytes, size, error);
    v.reason = r.Reason ();
    v.message = r.Text ("message");
    return r.Finish ("Failed");
}

std::vector<uint8_t> Encode (const CancelPayload& v)
{
    Writer w;
    w.U64 (v.runGeneration);
    return w.Take ();
}

bool Decode (const uint8_t* bytes, size_t size, CancelPayload& v, std::string& error)
{
    Reader r (bytes, size, error);
    v.runGeneration = r.U64 ("runGeneration");
    return r.Finish ("Cancel");
}

std::vector<uint8_t> EncodeText (const std::string& utf8)
{
    Writer w;
    w.Text (utf8);
    return w.Take ();
}

bool DecodeText (const uint8_t* bytes, size_t size, std::string& utf8, std::string& error)
{
    Reader r (bytes, size, error);
    utf8 = r.Text ("text");
    return r.Finish ("Log");
}

bool AcceptsVersion (uint32_t workerVersion, std::string& refusal)
{
    if (workerVersion == Version)
        return true;
    refusal = "The GPU worker speaks protocol version " + std::to_string (workerVersion) + " but this add-on speaks " +
              std::to_string (Version) + "; reinstall the add-on and its worker together.";
    return false;
}

const char* DescribeMessageType (MessageType type)
{
    switch (type) {
        case MessageType::Hello:
            return "Hello";
        case MessageType::HelloAck:
            return "HelloAck";
        case MessageType::Heartbeat:
            return "Heartbeat";
        case MessageType::UploadScene:
            return "UploadScene";
        case MessageType::SceneReady:
            return "SceneReady";
        case MessageType::RunStudy:
            return "RunStudy";
        case MessageType::RunResult:
            return "RunResult";
        case MessageType::Failed:
            return "Failed";
        case MessageType::Cancel:
            return "Cancel";
        case MessageType::Shutdown:
            return "Shutdown";
        case MessageType::Log:
            return "Log";
    }
    return "Unknown";
}

} // namespace evp::rayworker::protocol
