#ifndef EVP_ARCHVIZ_D3D12FEASIBILITYPROBE_HPP
#define EVP_ARCHVIZ_D3D12FEASIBILITYPROBE_HPP

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace geomsrv::archviz {

struct D3D12FeasibilityProbeStats {
    bool attempted = false;
    bool running = false;
    bool completed = false;
    bool cancelled = false;
    bool cleanTeardown = false;
    std::string stage;
    std::string error;

    bool deviceAttempted = false;
    bool deviceSucceeded = false;
    std::string deviceError;
    std::string adapter;
    bool hardwarePreflightSucceeded = false;
    uint32_t hardwareCreateResult = 0;
    uint32_t hardwareFeatureLevel = 0;
    std::string d3d12Runtime;

    bool childAttempted = false;
    bool childSucceeded = false;
    uint64_t childPresents = 0;
    uint32_t childLastPresentResult = 0;
    std::string childError;

    bool overlayAttempted = false;
    bool overlaySucceeded = false;
    uint64_t overlayPresents = 0;
    uint32_t overlayLastPresentResult = 0;
    std::string overlayError;

    uint32_t rayTracingFeature = 0;
    uint32_t rayTracingCaps = 0;
    bool rayTracingStandalone = false;
    bool rayTracingInline = false;
    bool rayTracingIndirect = false;
    uint32_t maxRecursionDepth = 0;
    uint32_t maxRayGenThreads = 0;
};

// RE51.D1 only. This owns a D3D12 device and its two test presentation paths;
// it never reaches into the production D3D11 viewport or target.
class D3D12FeasibilityProbe final {
  public:
    static D3D12FeasibilityProbe& Get ();

    bool Start (void* childHwnd, uint32_t childWidth, uint32_t childHeight, void* overlayHwnd, uint32_t overlayWidth,
                uint32_t overlayHeight, const std::string& overlayPreflightError, std::string& error);
    void Stop ();
    D3D12FeasibilityProbeStats Stats () const;
    void SetRefusal (const std::string& error);

  private:
    D3D12FeasibilityProbe () = default;
    ~D3D12FeasibilityProbe ();
    void Run (void* childHwnd, uint32_t childWidth, uint32_t childHeight, void* overlayHwnd, uint32_t overlayWidth,
              uint32_t overlayHeight, std::string overlayPreflightError);

    std::atomic<bool> attempted_ { false };
    std::atomic<bool> stopRequested_ { false };
    std::thread worker_;
    mutable std::mutex mutex_;
    D3D12FeasibilityProbeStats stats_;
};

} // namespace geomsrv::archviz

#endif
