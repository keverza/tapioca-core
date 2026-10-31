#ifndef EVP_RAYWORKER_RAYWORKERPROTOCOL_HPP
#define EVP_RAYWORKER_RAYWORKERPROTOCOL_HPP

// RayWorker/RayWorkerProtocol -- the control wire between the add-on and the
// out-of-process GPU ray worker (raycast plan §3.5, E7).
//
// ⚠️ DevKit-free, Win32-free and CUDA-free on purpose: framing is where a
// transport goes wrong silently, and every rule here is about bytes, so it is
// the half that is tested offline. The worker executable compiles this same
// file; the two halves ship together and share one version number.
//
// Rules carried over from the add-on's existing worker protocol:
// - fixed-size little-endian header, spelled out byte by byte (never a packed
//   struct copy, whose layout depends on the compiler);
// - the version is compared first and a mismatch is refused, naming both;
// - every length is checked BEFORE anything is allocated;
// - no text parsing on the control path: strings are length-prefixed UTF-8.
//
// Geometry and results never travel here; they go through shared-memory
// segments (RayWorkerPacket.hpp) that these messages name, with an epoch and a
// checksum so a stale or partial segment is never published.

#include "RayWorker/RayWorkerReason.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace evp::rayworker::protocol {

// v1: the first wire.
// v2: packet layout 2 (word-lane checksum); a v1 worker is refused at Hello
// rather than failing every run on a checksum it computes differently.
constexpr uint32_t Version = 2;

// 5 x uint32 little-endian: protocolVersion, messageType, requestId,
// correlationId, payloadBytes.
constexpr uint32_t HeaderSize = 20;

// Control messages are small; anything large is a shared-memory segment.
constexpr uint32_t MaxPayloadBytes = 64u * 1024u;

// Segment names and device names are short; a corrupt length is refused.
constexpr uint32_t MaxTextBytes = 1024;

// Worker capability bits (Hello).
constexpr uint32_t CapabilityHardwareRayTracing = 1u << 0;
constexpr uint32_t CapabilityDoublePrecision = 1u << 1;

enum class MessageType : uint32_t {
    // worker -> host, first on a fresh connection: who it is and what it can do.
    Hello = 1,
    // host -> worker: accepted capabilities, or a refusal.
    HelloAck = 2,
    // worker -> host, on its own cadence; its ABSENCE is the liveness signal.
    Heartbeat = 3,
    // host -> worker: build the scene in a segment (geometry changed).
    UploadScene = 4,
    // worker -> host: the scene of that epoch is resident.
    SceneReady = 5,
    // host -> worker: trace a request segment against a resident scene.
    RunStudy = 6,
    // worker -> host: a result segment is complete.
    RunResult = 7,
    // worker -> host: a correlated request failed, with a reason code.
    Failed = 8,
    // host -> worker: abandon a run (cooperative; the guarantee is a kill).
    Cancel = 9,
    // host -> worker: exit cleanly.
    Shutdown = 10,
    // worker -> host: one log line, stamped by the host on arrival.
    Log = 11,
};

struct Header {
    uint32_t protocolVersion = 0;
    MessageType messageType = MessageType::Hello;
    uint32_t requestId = 0;
    uint32_t correlationId = 0;
    uint32_t payloadBytes = 0;
};

// The worker's capability report. `reason` is not None when the worker runs
// but cannot serve GPU work (old driver, no RT device): it still connects so
// the host can say why instead of guessing from a dead process.
struct HelloPayload {
    uint32_t processId = 0;
    uint32_t capabilities = 0;
    GpuReason reason = GpuReason::None;
    uint32_t driverVersion = 0;     // e.g. 55299 for 552.99
    uint32_t runtimeVersion = 0;    // ray-tracing runtime, e.g. 80000
    uint32_t computeCapability = 0; // major * 10 + minor
    uint64_t deviceMemoryBytes = 0;
    std::string deviceName; // UTF-8
};

struct HelloAckPayload {
    uint32_t capabilities = 0; // granted
    std::string refusal;       // empty on acceptance
};

// A shared-memory segment named by a control message.
struct SegmentRef {
    std::string name; // UTF-8, platform object name
    uint64_t bytes = 0;
    uint64_t checksum = 0; // of the payload after the packet header
};

struct UploadScenePayload {
    uint64_t sceneEpoch = 0;
    SegmentRef scene;
};

struct SceneReadyPayload {
    uint64_t sceneEpoch = 0;
    uint32_t buildMicroseconds = 0;
    uint64_t deviceBytes = 0;
};

struct RunStudyPayload {
    uint64_t runGeneration = 0;
    uint64_t sceneEpoch = 0;
    SegmentRef request;
    SegmentRef result; // `bytes` is the capacity; checksum unused
};

// Timings of the complete study (decision 20), microseconds.
struct RunTimings {
    uint32_t transfer = 0;
    uint32_t trace = 0;
    uint32_t readback = 0;
};

struct RunResultPayload {
    uint64_t runGeneration = 0;
    uint64_t sceneEpoch = 0;
    uint64_t resultBytes = 0;
    uint64_t checksum = 0;
    RunTimings timings;
};

struct FailedPayload {
    GpuReason reason = GpuReason::WorkerFailed;
    std::string message; // UTF-8
};

struct CancelPayload {
    uint64_t runGeneration = 0;
};

std::vector<uint8_t> EncodeHeader (MessageType type, uint32_t requestId, uint32_t correlationId, uint32_t payloadBytes);
bool DecodeHeader (const uint8_t* bytes, size_t size, Header& header, std::string& error);

// One whole frame: header followed by payload.
std::vector<uint8_t> EncodeFrame (MessageType type, uint32_t requestId, uint32_t correlationId,
                                  const std::vector<uint8_t>& payload);

std::vector<uint8_t> Encode (const HelloPayload& value);
std::vector<uint8_t> Encode (const HelloAckPayload& value);
std::vector<uint8_t> Encode (const UploadScenePayload& value);
std::vector<uint8_t> Encode (const SceneReadyPayload& value);
std::vector<uint8_t> Encode (const RunStudyPayload& value);
std::vector<uint8_t> Encode (const RunResultPayload& value);
std::vector<uint8_t> Encode (const FailedPayload& value);
std::vector<uint8_t> Encode (const CancelPayload& value);
std::vector<uint8_t> EncodeText (const std::string& utf8);

// Each refuses a short, long or malformed payload with a reason; trailing
// bytes are an error (a length that does not match is not "forward compatible",
// it is a different message).
bool Decode (const uint8_t* bytes, size_t size, HelloPayload& value, std::string& error);
bool Decode (const uint8_t* bytes, size_t size, HelloAckPayload& value, std::string& error);
bool Decode (const uint8_t* bytes, size_t size, UploadScenePayload& value, std::string& error);
bool Decode (const uint8_t* bytes, size_t size, SceneReadyPayload& value, std::string& error);
bool Decode (const uint8_t* bytes, size_t size, RunStudyPayload& value, std::string& error);
bool Decode (const uint8_t* bytes, size_t size, RunResultPayload& value, std::string& error);
bool Decode (const uint8_t* bytes, size_t size, FailedPayload& value, std::string& error);
bool Decode (const uint8_t* bytes, size_t size, CancelPayload& value, std::string& error);
bool DecodeText (const uint8_t* bytes, size_t size, std::string& utf8, std::string& error);

bool AcceptsVersion (uint32_t workerVersion, std::string& refusal);
const char* DescribeMessageType (MessageType type);

} // namespace evp::rayworker::protocol

#endif
