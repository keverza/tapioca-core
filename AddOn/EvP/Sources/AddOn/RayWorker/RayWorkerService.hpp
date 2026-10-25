#ifndef EVP_RAYWORKER_RAYWORKERSERVICE_HPP
#define EVP_RAYWORKER_RAYWORKERSERVICE_HPP

// RayWorker/RayWorkerService -- the worker's side of the wire: frames in,
// frames out, over a segment store and a ray backend. The worker executable
// wraps it with a named pipe and file mappings and plugs in the hardware
// ray-tracing backend; tests wrap it with an in-memory store and the CPU
// reference backend. Everything that decides what a reply is lives here, so
// the protocol is exercised end to end without a GPU.

#include "RayWorker/RayWorkerPacket.hpp"
#include "RayWorker/RayWorkerProtocol.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace evp::rayworker {

// Named byte segments: shared memory in the product, a map in tests.
class SegmentStore {
  public:
    virtual ~SegmentStore () = default;
    virtual bool Put (const std::string& name, std::vector<uint8_t> bytes) = 0;
    virtual bool Get (const std::string& name, std::vector<uint8_t>& bytes) const = 0;
};

class MemorySegmentStore final : public SegmentStore {
  public:
    bool Put (const std::string& name, std::vector<uint8_t> bytes) override;
    bool Get (const std::string& name, std::vector<uint8_t>& bytes) const override;
    // Tests: corrupt or replace what a name holds.
    std::map<std::string, std::vector<uint8_t>>& Segments ()
    {
        return segments_;
    }

  private:
    std::map<std::string, std::vector<uint8_t>> segments_;
};

// What does the tracing. The hardware backend implements this in the worker;
// the CPU reference backend implements it for tests and diagnostics.
class RayBackend {
  public:
    virtual ~RayBackend () = default;
    // The capability report sent in Hello (reason None when it can serve).
    virtual protocol::HelloPayload Describe () const = 0;
    virtual GpuReason BuildScene (const packet::Scene& scene, protocol::SceneReadyPayload& ready,
                                  std::string& error) = 0;
    // Trace `request` against the resident scene into `result`.
    virtual GpuReason Trace (const packet::Request& request, packet::Result& result, protocol::RunTimings& timings,
                             const std::function<bool ()>& isCancelled, std::string& error) = 0;
};

// The CPU reference backend: the sun study's own accumulator over the C++
// query index, so a GPU backend has an exact oracle behind the same wire.
// Defined in RayWorkerCpuReference.cpp, which links the geometry engine; a
// worker executable links the service without it.
std::unique_ptr<RayBackend> MakeCpuReferenceBackend ();

class RayWorkerService final {
  public:
    RayWorkerService (std::unique_ptr<RayBackend> backend, SegmentStore& segments);

    // The first frame the worker sends.
    std::vector<uint8_t> Hello ();
    // Handle one frame; returns the frames to send back (possibly none).
    std::vector<std::vector<uint8_t>> OnFrame (const std::vector<uint8_t>& frame);
    bool ShutdownRequested () const
    {
        return shutdown_;
    }
    uint64_t ResidentScene () const
    {
        return residentScene_;
    }

  private:
    std::vector<uint8_t> Reply (protocol::MessageType type, uint32_t correlation, const std::vector<uint8_t>& payload);
    std::vector<uint8_t> Fail (uint32_t correlation, GpuReason reason, const std::string& message);

    std::unique_ptr<RayBackend> backend_;
    SegmentStore& segments_;
    uint32_t nextRequestId_ = 1;
    uint64_t residentScene_ = 0;
    uint64_t cancelledRun_ = 0;
    bool accepted_ = false;
    bool shutdown_ = false;
};

} // namespace evp::rayworker

#endif
