#ifndef GEOMETRYSERVER_MESHSTORE_HPP
#define GEOMETRYSERVER_MESHSTORE_HPP

#include "Mesh.hpp"

#include <memory>
#include <mutex>

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
        current = std::move (snap);
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
    }

    size_t Bytes () const
    {
        std::lock_guard<std::mutex> lock (mtx);
        return current ? current->Bytes () : 0;
    }

private:
    MeshStore () = default;

    mutable std::mutex              mtx;
    std::shared_ptr<const Snapshot> current;
};

} // namespace geomsrv

#endif
