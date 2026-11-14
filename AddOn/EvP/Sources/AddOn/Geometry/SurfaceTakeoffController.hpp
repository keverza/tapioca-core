#ifndef EVP_NATIVECOMMANDS_SURFACETAKEOFFCONTROLLER_HPP
#define EVP_NATIVECOMMANDS_SURFACETAKEOFFCONTROLLER_HPP

#include "Geometry/MeshStore.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace geomsrv::surfacetakeoff {

enum class ControllerState { Empty, Pending, Current, Stale, Refused, Cancelled };
const char* ControllerStateName (ControllerState state);

struct ControllerOutcome {
    bool ok = false;
    std::shared_ptr<const void> payload;
    std::string error;
};

struct ControllerRequest {
    std::shared_ptr<const Snapshot> snapshot;
    std::function<ControllerOutcome (const std::atomic<bool>&)> execute;
};

struct ControllerResult {
    uint64_t generation = 0, snapshotId = 0, captureStamp = 0;
    std::shared_ptr<const Snapshot> snapshot;
    std::shared_ptr<const void> payload;
};

struct ControllerView {
    ControllerState state = ControllerState::Empty;
    uint64_t generation = 0;
    std::optional<ControllerResult> lastGood;
    std::string error;
};

// Latest-request-wins native mailbox. SDK capture is owned by the caller;
// computation touches only owned inputs. Poll is the publication boundary and
// rechecks document/capture identity before accepting a completed result.
class SurfaceTakeoffController final {
  public:
    SurfaceTakeoffController () = default;
    ~SurfaceTakeoffController ();
    SurfaceTakeoffController (const SurfaceTakeoffController&) = delete;
    SurfaceTakeoffController& operator= (const SurfaceTakeoffController&) = delete;

    uint64_t Submit (ControllerRequest request, std::string& error); // assigned generation, zero on refusal
    ControllerView Poll ();
    void Cancel ();
    void Clear ();
    void OnProjectClosed ();
    void Shutdown ();

  private:
    struct Ticket {
        ControllerRequest request;
        uint64_t generation = 0;
        std::atomic<bool> cancelled { false };
    };
    void ThreadMain ();
    ControllerView ViewLocked () const;
    static bool Current (const std::shared_ptr<const Snapshot>& snapshot);

    mutable std::mutex mutex_;
    std::condition_variable workAvailable_;
    std::thread thread_;
    std::shared_ptr<Ticket> pending_, running_, completed_;
    ControllerOutcome completion_;
    bool ready_ = false, stopping_ = false;
    uint64_t generation_ = 0;
    std::optional<ControllerResult> lastGood_;
    ControllerState terminal_ = ControllerState::Empty;
    std::string error_;
};

SurfaceTakeoffController& SharedSurfaceTakeoffController ();

} // namespace geomsrv::surfacetakeoff

#endif
