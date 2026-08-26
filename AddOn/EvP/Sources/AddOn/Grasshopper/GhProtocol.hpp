#ifndef EVP_GRASSHOPPER_GHPROTOCOL_HPP
#define EVP_GRASSHOPPER_GHPROTOCOL_HPP

// The wire between Archicad and Tapioca.GhWorker.exe, and nothing else.
//
// Deliberately DevKit-free, Win32-free and CLR-free — only <cstdint>, <string>
// and <vector> — for the same reason HostState.hpp is: this is the half of the
// bridge that can be exercised offline. Framing is where a transport goes wrong
// silently (a length read as a type, a payload cap that is checked after the
// allocation, a version accepted because nobody compared it), and every one of
// those is a rule about bytes rather than about pipes.
//
// ⚠️ NO JSON, NO XML AND NO TEXT PARSING ON THE CONTROL PATH.
// HANDOFF-GrasshopperInsideArchicad.md, "The bridge": control, events and small
// results have a sub-millisecond budget, which a text protocol cannot hold. The
// header is fixed-size little-endian POD. The only UTF-8 in here is inside a
// payload whose length the header already stated — it is carried, never parsed
// to find the end of a message.
//
// ⚠️ THE VERSION IS NEGOTIATED, NOT ADAPTED TO. A worker whose protocolVersion
// this host does not know is REFUSED with a message that names both numbers.
// Two halves that ship together and are versioned together have exactly one
// legitimate mismatch — a stale worker left beside an upgraded add-on — and
// "rebuild and redeploy both halves" is the only useful thing to say about it.

#include <cstdint>
#include <string>
#include <vector>

namespace evp {
namespace grasshopper {
namespace protocol {

// Bumped whenever the meaning of ANY message below changes. Both halves carry
// their own copy (see Sources/GhWorker/BridgeProtocol.cs) and the handshake
// compares them; neither is allowed to infer the other's.
constexpr uint32_t Version = 1;

// 5 x uint32, little-endian: protocolVersion, messageType, requestId,
// correlationId, payloadBytes.
constexpr uint32_t HeaderSize = 20;

// A ceiling that is checked BEFORE anything is allocated. Small results travel
// here; geometry does not (it goes through shared memory, referenced by handle),
// so this is generous rather than a design budget.
constexpr uint32_t MaxPayloadBytes = 16u * 1024u * 1024u;

// Long enough for any command name the dispatcher knows and short enough that a
// corrupt length is refused instead of allocated.
constexpr uint32_t MaxCommandBytes = 1024;

enum class MessageType : uint32_t {
    // worker -> host, first message on a fresh connection. Payload: HelloPayload.
    Hello = 1,
    // host -> worker, the answer to Hello. Payload: UTF-8, empty on acceptance
    // and the refusal reason otherwise.
    HelloAck = 2,
    // worker -> host, unsolicited, on the worker's own cadence. No payload. Its
    // ABSENCE is the signal: GhWorkerHost's liveness deadline is measured from
    // the last one seen.
    Heartbeat = 3,
    // worker -> host, correlated. Payload: ApiRequest (command + params).
    ApiRequest = 4,
    // host -> worker, correlationId = the request's requestId. Payload: UTF-8
    // envelope, exactly the {"ok":...} shape ApiDispatcher already produces.
    ApiResponse = 5,
    // host -> worker control. No payload.
    ShowEditor = 6,
    HideEditor = 7,
    // host -> worker, the COOPERATIVE half of cancellation and shutdown. The
    // guaranteed half is TerminateProcess and lives in GhWorkerHost.
    Shutdown = 8,
    // worker -> host, correlated to whatever it answers. Payload: Ack (status +
    // UTF-8 message).
    Ack = 9,
    // worker -> host, unsolicited. Payload: UTF-8 line for grasshopper.log. One
    // log file, two processes: the host stamps pid and restart generation on
    // arrival, so the interleaving is recorded rather than reconstructed.
    Log = 10,
};

// Mirrors TapiocaGhStatus's surviving cases. The transport carries them; it does
// not interpret them.
enum class AckStatus : uint32_t {
    Ok = 0,
    Failed = 1,
    NotReady = 2,
};

struct Header {
    uint32_t protocolVersion = 0;
    MessageType messageType = MessageType::Hello;
    uint32_t requestId = 0;
    uint32_t correlationId = 0;
    uint32_t payloadBytes = 0;
};

struct HelloPayload {
    uint32_t processId = 0;
    // Reserved for the capability bits P3 needs (shared-memory geometry,
    // selection events). Zero today, and a worker that sets an unknown bit is
    // not refused for it — the handshake gates the VERSION, and capabilities are
    // additive within one.
    uint32_t capabilities = 0;
};

struct ApiRequestPayload {
    std::string command;    // UTF-8
    std::string parameters; // UTF-8 JSON, may be empty
};

struct AckPayload {
    AckStatus status = AckStatus::Ok;
    std::string message; // UTF-8
};

std::vector<uint8_t> EncodeHeader (MessageType type, uint32_t requestId, uint32_t correlationId, uint32_t payloadBytes);

// Refuses a wrong version, an unknown message type and an oversized payload,
// each with its own `error`, and never partially fills `header` on failure.
bool DecodeHeader (const uint8_t* bytes, size_t size, Header& header, std::string& error);

std::vector<uint8_t> EncodeHelloPayload (const HelloPayload& hello);
bool DecodeHelloPayload (const uint8_t* bytes, size_t size, HelloPayload& hello, std::string& error);

std::vector<uint8_t> EncodeApiRequestPayload (const ApiRequestPayload& request);
bool DecodeApiRequestPayload (const uint8_t* bytes, size_t size, ApiRequestPayload& request, std::string& error);

std::vector<uint8_t> EncodeAckPayload (const AckPayload& ack);
bool DecodeAckPayload (const uint8_t* bytes, size_t size, AckPayload& ack, std::string& error);

// UTF-8 payloads (HelloAck, ApiResponse, Log). Encoding is a copy; decoding
// exists so that an embedded NUL is REJECTED rather than silently truncating the
// string on the way into whatever reads it.
std::vector<uint8_t> EncodeTextPayload (const std::string& utf8);
bool DecodeTextPayload (const uint8_t* bytes, size_t size, std::string& utf8, std::string& error);

// The handshake verdict, with the message a user has to act on. Kept here rather
// than in the host so that the refusal wording is covered by the offline tests.
bool AcceptsVersion (uint32_t workerVersion, std::string& refusal);

const char* DescribeMessageType (MessageType type);

} // namespace protocol
} // namespace grasshopper
} // namespace evp

#endif
