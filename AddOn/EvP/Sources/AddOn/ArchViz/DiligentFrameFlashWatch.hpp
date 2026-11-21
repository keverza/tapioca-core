#pragma once

// ArchViz/DiligentFrameFlashWatch -- measures each finished scene frame and logs the frames that
// flash (#14 white-frame diagnostic; FrameFlash.hpp has the rule and why).
//
// Per frame: a 4x4 grid of 8x8-pixel patches is copied from the target the scene drew into, into
// one of three small staging textures behind a fence -- never a wait; a slot still in flight skips
// the frame. Each slot keeps what the frame did (FrameFlashState), so a flash is logged with the
// frame's own state, a frame or two after it was drawn. 8-bit targets only; anything else, or a
// failed resource, turns the watch off with one log line.
//
// ⚠️ RENDER THREAD ONLY, and owned by the scene: released with it, before its device.

#include <cstddef>
#include <cstdint>
#include <memory>

namespace Diligent {
struct IRenderDevice;
struct IDeviceContext;
struct ITextureView;
} // namespace Diligent

namespace geomsrv {
namespace archviz {

// What the frame did, recorded when it is sampled.
struct FrameFlashState {
    uint64_t frame = 0;
    uint64_t picksIssued = 0; // DiligentPickBuffer requests so far (PickRequestsIssued)
    size_t drawCalls = 0;
    size_t meshCommits = 0;
    int stillPhase = 0; // TemporalPhase
    bool idle = false;  // presented the still image without drawing the scene
    bool taa = false;   // TAA resolved this frame
};

class DiligentFrameFlashWatch {
  public:
    DiligentFrameFlashWatch ();
    ~DiligentFrameFlashWatch ();
    DiligentFrameFlashWatch (const DiligentFrameFlashWatch&) = delete;
    DiligentFrameFlashWatch& operator= (const DiligentFrameFlashWatch&) = delete;

    // After the scene's draw. `target` and `depth` are bound again afterwards (the copy moves the
    // target out of its render-target state).
    void Sample (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, Diligent::ITextureView* target,
                 Diligent::ITextureView* depth, const FrameFlashState& state);
    void Shutdown ();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace archviz
} // namespace geomsrv
