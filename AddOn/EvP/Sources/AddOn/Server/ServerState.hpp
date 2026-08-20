#ifndef GEOMETRYSERVER_SERVERSTATE_HPP
#define GEOMETRYSERVER_SERVERSTATE_HPP

#include <atomic>
#include <cstdint>

// Shared, lock-free state that HTTP worker threads may read WITHOUT touching
// ACAPI. The main thread (menu/notification handlers) is the only writer.
// This is the plan's design: /health and queries read cached state; ACAPI is
// never called from a worker thread.
namespace geomsrv {

class ServerState {
public:
    static ServerState& Get ()
    {
        static ServerState instance;
        return instance;
    }

    std::atomic<bool>     modelOpen     { false };  // set by project open/close events
    std::atomic<uint64_t> snapshotId    { 0 };      // bumped on each BuildSnapshot
    std::atomic<bool>     serverRunning { false };  // HTTP data plane up?
    std::atomic<int>      port          { 0 };      // its bound port

private:
    ServerState () = default;
};

} // namespace geomsrv

#endif
