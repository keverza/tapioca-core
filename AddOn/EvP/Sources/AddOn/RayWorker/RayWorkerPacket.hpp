#ifndef EVP_RAYWORKER_RAYWORKERPACKET_HPP
#define EVP_RAYWORKER_RAYWORKERPACKET_HPP

// RayWorker/RayWorkerPacket -- what a shared-memory segment holds: the scene
// (geometry, uploaded once per snapshot), a study request (samples and sun
// directions) and its result (the per-sample, per-step lit bitset).
//
// ⚠️ A SEGMENT IS NEVER TRUSTED BECAUSE A MESSAGE NAMED IT. Every packet carries
// a magic, a layout version, its kind, an epoch (the scene it belongs to), a
// sequence (the run generation for requests and results), its payload length
// and a checksum of the payload. The reader checks all of them, plus every
// internal count and index, before handing anything back -- a stale, partial
// or foreign segment is refused with a reason, never published (raycast plan
// §3.5: "stale or partial payloads can never be published").
//
// Byte buffers only: the platform mapping is the transport's business, so this
// layout is tested offline and compiled unchanged by the worker.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv {
struct Snapshot;
}

namespace evp::rayworker::packet {

constexpr uint32_t Magic = 0x50575254u; // "TRWP" little-endian
constexpr uint32_t LayoutVersion = 2;   // 2: word-lane checksum
constexpr size_t HeaderBytes = 64;

enum class Kind : uint32_t { Scene = 1, Request = 2, Result = 3 };

// The geometry, flattened: one vertex array, one triangle array and the
// owning snapshot mesh of every triangle (for masked first-hit queries).
struct Scene {
    uint32_t meshCount = 0;
    std::vector<double> vertices;       // xyz, world metres
    std::vector<uint32_t> triangles;    // 3 global vertex indices per triangle
    std::vector<uint32_t> triangleMesh; // one snapshot mesh index per triangle
};

// One study: samples and the sun directions of the steps to trace.
struct Request {
    double tmin = 0.001;
    double tmax = 0.0;              // <= 0 unbounded
    uint64_t totalSteps = 0;        // the day's step count (result layout)
    std::vector<double> positions;  // xyz per sample
    std::vector<double> normals;    // xyz per sample, or empty (always facing)
    std::vector<uint32_t> steps;    // step indices to trace, ascending
    std::vector<double> directions; // xyz per entry of `steps`
};

// The lit bitset in the accumulator's own layout: sample-major, 64-bit words,
// bit `step % 64` of word `sample * wordsPerSample + step / 64`.
struct Result {
    uint64_t sampleCount = 0;
    uint64_t stepCount = 0;
    std::vector<uint64_t> bits;
    std::vector<uint8_t> resolved; // one per step: 1 when traced
};

// The payload checksum: four FNV-1a lanes over 64-bit words (layout 2).
uint64_t Checksum (const uint8_t* bytes, size_t size);

// Flatten a snapshot (mesh order, local indices rebased).
Scene SceneFromSnapshot (const geomsrv::Snapshot& snapshot);

std::vector<uint8_t> Write (uint64_t epoch, const Scene& scene);
std::vector<uint8_t> Write (uint64_t epoch, uint64_t sequence, const Request& request);
std::vector<uint8_t> Write (uint64_t epoch, uint64_t sequence, const Result& result);

// The payload checksum stored in a written packet (what a control message
// carries beside the segment name).
uint64_t StoredChecksum (const std::vector<uint8_t>& packet);

// Read and validate. `checksum` is the one the control message carried; it
// must equal both the stored and the recomputed checksum.
bool Read (const uint8_t* bytes, size_t size, uint64_t epoch, uint64_t checksum, Scene& scene, std::string& error);
bool Read (const uint8_t* bytes, size_t size, uint64_t epoch, uint64_t sequence, uint64_t checksum, Request& request,
           std::string& error);
bool Read (const uint8_t* bytes, size_t size, uint64_t epoch, uint64_t sequence, uint64_t checksum, Result& result,
           std::string& error);

} // namespace evp::rayworker::packet

#endif
