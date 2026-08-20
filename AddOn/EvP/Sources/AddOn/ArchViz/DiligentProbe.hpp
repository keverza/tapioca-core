#ifndef EVP_ARCHVIZ_DILIGENTPROBE_HPP
#define EVP_ARCHVIZ_DILIGENTPROBE_HPP

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace geomsrv::archviz {

struct DiligentProbeStats {
    bool attempted = false;
    bool running = false;
    bool succeeded = false;
    std::string error;
};

// Probe 1c only: create and release a Diligent D3D11 IRenderDevice in the
// Archicad process after a real DG child HWND has been validated. No swap chain,
// draw, or retry belongs here.
class DiligentProbe {
public:
    static DiligentProbe& Get ();
    bool Start (void* hwnd);
    void Stop ();
    DiligentProbeStats Stats () const;

private:
    DiligentProbe () = default;
    ~DiligentProbe ();
    void Run (void* hwnd);

    std::atomic<bool> attempted_ {false};
    std::atomic<bool> running_ {false};
    std::thread worker_;
    mutable std::mutex mutex_;
    DiligentProbeStats stats_;
};

} // namespace geomsrv::archviz

#endif
