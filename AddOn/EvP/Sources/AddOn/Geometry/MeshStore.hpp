#ifndef GEOMETRYSERVER_MESHSTORE_HPP
#define GEOMETRYSERVER_MESHSTORE_HPP

#include "Mesh.hpp"

#include <memory>
#include <mutex>
#include <atomic>

// Holds the current immutable snapshot. The main thread publishes a new
// snapshot after extraction; worker threads read it. Snapshots are immutable
// once published, so readers hold a shared_ptr and never see partial state.
namespace geomsrv {

class MeshStore {
  public:
    static MeshStore& Get ()
    {
        static MeshStore instance;
        return instance;
    }

    // Main thread: replace the current snapshot atomically.
    void Publish (std::shared_ptr<const Snapshot> snap)
    {
        std::lock_guard<std::mutex> lock (mtx);
        if (snap != nullptr && snap->completeModel && snap->captureStamp != 0)
            shared = snap;
        current = std::move (snap);
    }

    // Viewer worker: publish only a completed immutable pass. It does not replace
    // a caller's selection/filtered snapshot or its metadata as a side effect.
    void PublishShared (std::shared_ptr<const Snapshot> snap)
    {
        std::lock_guard<std::mutex> lock (mtx);
        if (snap != nullptr && snap->completeModel && snap->captureStamp != 0)
            shared = std::move (snap);
    }
    std::shared_ptr<const Snapshot> Shared () const
    {
        std::lock_guard<std::mutex> lock (mtx);
        return shared;
    }
    uint64_t NextId ()
    {
        std::lock_guard<std::mutex> lock (mtx);
        return nextId++;
    }
    uint64_t CaptureStamp () const
    {
        return captureStamp.load ();
    }
    void BumpCaptureStamp ()
    {
        ++captureStamp;
    }
    void SetCaptureActive (bool active)
    {
        captureActive.store (active);
        ++captureStamp;
    }
    bool CaptureActive () const
    {
        return captureActive.load ();
    }

    // Any thread: get the current snapshot (may be null before first build).
    std::shared_ptr<const Snapshot> Current () const
    {
        std::lock_guard<std::mutex> lock (mtx);
        return current;
    }

    // Any thread: drop the snapshot and give the memory back. Safe to call while
    // queries are in flight — they hold their own shared_ptr, so they finish on
    // the old snapshot and the memory is freed when the last reader lets go.
    void Release ()
    {
        std::lock_guard<std::mutex> lock (mtx);
        current.reset ();
        shared.reset ();
        ++captureStamp;
    }

    size_t Bytes () const
    {
        std::lock_guard<std::mutex> lock (mtx);
        return (current ? current->Bytes () : 0) + (shared && shared != current ? shared->Bytes () : 0);
    }

  private:
    MeshStore () = default;

    mutable std::mutex mtx;
    std::shared_ptr<const Snapshot> current;
    std::shared_ptr<const Snapshot> shared;
    uint64_t nextId = 1;
    std::atomic<uint64_t> captureStamp { 1 };
    std::atomic<bool> captureActive { false };
};

} // namespace geomsrv

#endif
