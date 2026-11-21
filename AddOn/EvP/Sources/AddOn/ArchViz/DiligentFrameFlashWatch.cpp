// ArchViz/DiligentFrameFlashWatch -- see the header.

#include "ArchViz/DiligentFrameFlashWatch.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/FrameFlash.hpp"

#include <windows.h>
#include <d3d11.h> // Must precede any Diligent D3D11 interop header (Probe 1a).

#include <DeviceContext.h>
#include <Fence.h>
#include <RefCntAutoPtr.hpp>
#include <RenderDevice.h>
#include <Texture.h>
#include <TextureView.h>

#include <chrono>
#include <string>

namespace geomsrv {
namespace archviz {

namespace {

constexpr uint32_t kGrid = 4;  // patches across and down
constexpr uint32_t kPatch = 8; // pixels a side
constexpr uint32_t kSide = kGrid * kPatch;
constexpr int kSlots = 3;
constexpr int kLinesPerSecond = 10;

bool EightBit (Diligent::TEXTURE_FORMAT f)
{
    return f == Diligent::TEX_FORMAT_RGBA8_UNORM || f == Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB ||
           f == Diligent::TEX_FORMAT_BGRA8_UNORM || f == Diligent::TEX_FORMAT_BGRA8_UNORM_SRGB;
}

} // namespace

struct DiligentFrameFlashWatch::Impl {
    bool off = false;
    Diligent::TEXTURE_FORMAT format = Diligent::TEX_FORMAT_UNKNOWN;
    Diligent::RefCntAutoPtr<Diligent::ITexture> staging[kSlots];
    Diligent::RefCntAutoPtr<Diligent::IFence> fence; // D3D12; D3D11's map says "not yet" itself
    uint64_t fenceValue = 0;
    uint64_t slotFence[kSlots] = {};
    bool slotPending[kSlots] = {};
    FrameFlashState slotState[kSlots];
    int nextSlot = 0;
    FrameFlashDetector detector;
    // The states of the frames held off the baseline, for the report (at most kMaxFlashFrames + 1).
    FrameFlashState held[FrameFlashDetector::kMaxFlashFrames + 1];
    uint32_t heldCount = 0;
    int64_t logSecond = -1;
    int logLines = 0;

    void TurnOff (const std::string& why)
    {
        off = true;
        ArchVizLog ("Diligent frame flash watch: off -- " + why);
    }

    bool Create (Diligent::IRenderDevice* device, Diligent::TEXTURE_FORMAT targetFormat)
    {
        format = targetFormat;
        for (auto& s : staging) {
            Diligent::TextureDesc d;
            d.Name = "ArchViz frame flash readback";
            d.Type = Diligent::RESOURCE_DIM_TEX_2D;
            d.Width = kSide;
            d.Height = kSide;
            d.MipLevels = 1;
            d.Format = targetFormat;
            d.BindFlags = Diligent::BIND_NONE;
            d.Usage = Diligent::USAGE_STAGING;
            d.CPUAccessFlags = Diligent::CPU_ACCESS_READ;
            device->CreateTexture (d, nullptr, &s);
            if (s == nullptr)
                return false;
        }
        if (device->GetDeviceInfo ().Type == Diligent::RENDER_DEVICE_TYPE_D3D12) {
            Diligent::FenceDesc fd;
            fd.Name = "ArchViz frame flash readback fence";
            fd.Type = Diligent::FENCE_TYPE_CPU_WAIT_ONLY;
            device->CreateFence (fd, &fence);
            if (fence == nullptr)
                return false;
        }
        ArchVizLog ("Diligent frame flash watch: on (" + std::to_string (kGrid * kGrid) + " patches of " +
                    std::to_string (kPatch) + " px; a frame " + std::to_string (int (FrameFlashDetector::kJump)) +
                    "+ levels off its neighbours for at most " + std::to_string (FrameFlashDetector::kMaxFlashFrames) +
                    " frames is logged)");
        return true;
    }

    static std::string Describe (const FrameFlashState& s)
    {
        return "frame " + std::to_string (s.frame) + " picks " + std::to_string (s.picksIssued) + " draws " +
               std::to_string (s.drawCalls) + " commits " + std::to_string (s.meshCommits) + " still " +
               std::to_string (s.stillPhase) + (s.idle ? " idle" : "") + (s.taa ? " taa" : "");
    }

    void Report (const FrameFlashDetector::Result& r, const FrameFlashState& after)
    {
        const int64_t second =
            std::chrono::duration_cast<std::chrono::seconds> (std::chrono::steady_clock::now ().time_since_epoch ())
                .count ();
        if (second != logSecond) {
            logSecond = second;
            logLines = 0;
        }
        if (++logLines > kLinesPerSecond)
            return;
        std::string line = "Diligent frame flash: " + std::to_string (r.frames) + " frame(s) at level " +
                           std::to_string (int (r.peak)) + " against " + std::to_string (int (r.baseline)) + " --";
        for (uint32_t k = 0; k < heldCount && k < r.frames; ++k)
            line += " [" + Describe (held[k]) + "]";
        line += "; then [" + Describe (after) + "]";
        ArchVizLog (line);
    }

    // Every slot the GPU has finished, oldest first, through the detector.
    void Drain (Diligent::IDeviceContext* context)
    {
        for (int k = 0; k < kSlots; ++k) {
            const int slot = (nextSlot + k) % kSlots;
            if (!slotPending[slot])
                continue;
            if (fence != nullptr && fence->GetCompletedValue () < slotFence[slot])
                break;
            Diligent::MappedTextureSubresource mapped;
            context->MapTextureSubresource (staging[slot], 0, 0, Diligent::MAP_READ, Diligent::MAP_FLAG_DO_NOT_WAIT,
                                            nullptr, mapped);
            if (mapped.pData == nullptr)
                break; // D3D11: not finished yet
            // ⚠️ THE ROW STRIDE IS NOT width*4: a staging texture is padded to the driver's alignment.
            const uint8_t* base = static_cast<const uint8_t*> (mapped.pData);
            uint64_t sum = 0;
            for (uint32_t y = 0; y < kSide; ++y) {
                const uint8_t* row = base + size_t (y) * size_t (mapped.Stride);
                for (uint32_t x = 0; x < kSide; ++x)
                    sum += uint32_t (row[x * 4]) + row[x * 4 + 1] + row[x * 4 + 2];
            }
            context->UnmapTextureSubresource (staging[slot], 0, 0);
            slotPending[slot] = false;
            const float level = float (sum) / float (3 * kSide * kSide);
            const FrameFlashState& state = slotState[slot];
            const uint32_t heldBefore = detector.held;
            const FrameFlashDetector::Result r = detector.Add (state.frame, level);
            if (r.flash)
                Report (r, state);
            if (detector.held > heldBefore && detector.held <= FrameFlashDetector::kMaxFlashFrames + 1)
                held[detector.held - 1] = state;
            heldCount = detector.held;
        }
    }
};

DiligentFrameFlashWatch::DiligentFrameFlashWatch () : impl_ (std::make_unique<Impl> ())
{
}

DiligentFrameFlashWatch::~DiligentFrameFlashWatch () = default;

void DiligentFrameFlashWatch::Shutdown ()
{
    impl_ = std::make_unique<Impl> ();
}

void DiligentFrameFlashWatch::Sample (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                                      Diligent::ITextureView* target, Diligent::ITextureView* depth,
                                      const FrameFlashState& state)
{
    Impl& m = *impl_;
    if (m.off || device == nullptr || context == nullptr || target == nullptr)
        return;
    Diligent::ITexture* texture = target->GetTexture ();
    const Diligent::TextureDesc& desc = texture->GetDesc ();
    if (desc.Width < kSide || desc.Height < kSide)
        return;
    if (m.format != desc.Format) {
        if (m.format != Diligent::TEX_FORMAT_UNKNOWN) {
            m.TurnOff ("the target's format changed");
            return;
        }
        if (!EightBit (desc.Format) || desc.SampleCount != 1) {
            m.TurnOff ("the scene's target is not a single-sampled 8-bit one");
            return;
        }
        if (!m.Create (device, desc.Format)) {
            m.TurnOff ("its readback textures could not be created");
            return;
        }
    }
    m.Drain (context);

    const int slot = m.nextSlot;
    if (m.slotPending[slot])
        return; // every slot still in flight: skip this frame rather than wait
    // Unbound first, so Diligent does not unbind the target itself (and log it every frame).
    context->SetRenderTargets (0, nullptr, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE);
    for (uint32_t gy = 0; gy < kGrid; ++gy)
        for (uint32_t gx = 0; gx < kGrid; ++gx) {
            Diligent::Box box;
            box.MinX = (2 * gx + 1) * desc.Width / (2 * kGrid) - kPatch / 2;
            box.MinY = (2 * gy + 1) * desc.Height / (2 * kGrid) - kPatch / 2;
            box.MaxX = box.MinX + kPatch;
            box.MaxY = box.MinY + kPatch;
            box.MinZ = 0;
            box.MaxZ = 1;
            Diligent::CopyTextureAttribs copy;
            copy.pSrcTexture = texture;
            copy.pSrcBox = &box;
            copy.SrcTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
            copy.pDstTexture = m.staging[slot];
            copy.DstX = gx * kPatch;
            copy.DstY = gy * kPatch;
            copy.DstTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
            context->CopyTexture (copy);
        }
    if (m.fence != nullptr)
        context->EnqueueSignal (m.fence, ++m.fenceValue);
    m.slotFence[slot] = m.fenceValue;
    m.slotPending[slot] = true;
    m.slotState[slot] = state;
    m.nextSlot = (slot + 1) % kSlots;
    // BOUND AGAIN: what follows the scene draws into the same targets.
    context->SetRenderTargets (1, &target, depth, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->SetViewports (1, nullptr, 0, 0);
}

} // namespace archviz
} // namespace geomsrv
