#include "ArchViz/DiligentPickBuffer.hpp"

#include "ArchViz/PickVote.hpp"

#include <windows.h>
#include <d3d11.h>   // Must precede any Diligent D3D11 interop header (Probe 1a).
#include <DeviceContext.h>
#include <GraphicsTypes.h>
#include <RefCntAutoPtr.hpp>
#include <RenderDevice.h>
#include <Texture.h>
#include <TextureView.h>

namespace geomsrv {
namespace archviz {

namespace {

using Diligent::RefCntAutoPtr;

// ⚠️ SMALL, BUT NOT ONE PIXEL, AND IT IS NOW HONESTLY IN PIXELS. A single pixel
// makes picking unusably precise: the user must hit a wall exactly, and a click
// one pixel off a thin railing selects the wall behind it. This is the edge of
// the box READ BACK around the cursor out of a full-resolution id buffer, so 8
// really is 8 screen pixels -- at every zoom, in both projections, at every DPI.
//
// The old shape could only claim that. Its 8x8 was the whole render target of a
// second camera, so what those 64 texels covered depended on how that camera's
// field of view had been derived, and a constant there once made them span fifty
// screen pixels (PLAT-RE130).
constexpr uint32_t kReadbackSize = 8;

// How many presents must go by before the staging texture is mapped.
//
// ⚠️ TWO, AND THEN A BLOCKING MAP -- deliberately, rather than polling with
// MAP_FLAG_DO_NOT_WAIT. By the time two vsynced Presents have happened the copy
// finished long ago, so the map returns immediately and the code has no
// "it was not ready, try again" state to get wrong. A `DO_NOT_WAIT` poll would
// buy back at most a frame of latency on a click, and it buys it by adding the
// one path that is hard to test without a GPU.
constexpr uint64_t kReadbackDelayFrames = 2;

}   // namespace

uint32_t PickColorFormat () { return uint32_t (Diligent::TEX_FORMAT_RGBA8_UNORM); }
uint32_t PickDepthFormat () { return uint32_t (Diligent::TEX_FORMAT_D32_FLOAT); }

struct DiligentPickBuffer::Impl {
    RefCntAutoPtr<Diligent::ITexture> color;
    RefCntAutoPtr<Diligent::ITexture> depth;
    RefCntAutoPtr<Diligent::ITexture> staging;
    Diligent::ITextureView* rtv = nullptr;   // owned by `color`
    Diligent::ITextureView* dsv = nullptr;   // owned by `depth`

    // The id target's current size, which tracks the viewport. 0 until the first
    // EnsureSize -- `ready` says Init succeeded, not that there is a target.
    uint32_t width = 0;
    uint32_t height = 0;

    bool ready = false;
    bool pending = false;
    uint64_t readyAtFrame = 0;
    uint32_t tag = 0;

    // The readback box that is in flight, and WHERE THE CURSOR SITS INSIDE IT.
    // ⚠️ THE CURSOR IS NOT AT THE BOX'S MIDDLE NEAR AN EDGE. The box is clamped
    // to the target, so a click three pixels from the left yields a box whose
    // middle is elsewhere; the vote is told the real offset rather than
    // assuming. See PickVote.hpp -- assuming is what "picking drifts near the
    // edges" would be.
    uint32_t boxWidth = 0;
    uint32_t boxHeight = 0;
    float centreX = 0.0f;
    float centreY = 0.0f;

    void ReleaseTarget ()
    {
        rtv = nullptr;
        dsv = nullptr;
        depth.Release ();
        color.Release ();
        width = 0;
        height = 0;
    }
};

DiligentPickBuffer::DiligentPickBuffer () : impl_ (std::make_unique<Impl> ()) {}
DiligentPickBuffer::~DiligentPickBuffer () { Shutdown (); }

bool DiligentPickBuffer::IsReady () const { return impl_ != nullptr && impl_->ready; }
bool DiligentPickBuffer::HasPendingRequest () const
{
    return impl_ != nullptr && impl_->pending;
}

bool DiligentPickBuffer::Init (Diligent::IRenderDevice* device, std::string& error)
{
    if (device == nullptr) {
        error = "DiligentPickBuffer::Init got no render device";
        return false;
    }
    if (impl_->ready)
        return true;

    // ⚠️ THE STAGING TEXTURE IS THE ONLY THING SIZED HERE, and it never changes
    // size: it holds the small box copied out around the cursor, not the id
    // buffer. The id buffer follows the viewport and is EnsureSize's job.
    //
    // ⚠️ BIND_NONE ON A STAGING TEXTURE. Diligent rejects a staging resource
    // that also declares a bind flag, and the message names the flag rather than
    // this line -- so the two are spelled out together here.
    Diligent::TextureDesc sd;
    sd.Name = "ArchViz pick readback";
    sd.Type = Diligent::RESOURCE_DIM_TEX_2D;
    sd.Width = kReadbackSize;
    sd.Height = kReadbackSize;
    sd.MipLevels = 1;
    // ⚠️ UNORM, NEVER _SRGB, and it must MATCH the id target's format or the
    // copy is rejected. See the header for what an sRGB round trip does to an id.
    sd.Format = static_cast<Diligent::TEXTURE_FORMAT> (PickColorFormat ());
    sd.BindFlags = Diligent::BIND_NONE;
    sd.Usage = Diligent::USAGE_STAGING;
    sd.CPUAccessFlags = Diligent::CPU_ACCESS_READ;
    device->CreateTexture (sd, nullptr, &impl_->staging);

    if (impl_->staging == nullptr) {
        error = "Diligent CreateTexture failed for the pick readback staging texture";
        Shutdown ();
        return false;
    }

    impl_->ready = true;
    return true;
}

void DiligentPickBuffer::Shutdown ()
{
    if (impl_ == nullptr)
        return;
    impl_->ReleaseTarget ();
    impl_->staging.Release ();
    impl_->ready = false;
    impl_->pending = false;
}

bool DiligentPickBuffer::EnsureSize (Diligent::IRenderDevice* device, uint32_t width,
                                     uint32_t height, std::string& error)
{
    if (!impl_->ready) {
        error = "DiligentPickBuffer::EnsureSize before a successful Init";
        return false;
    }
    if (device == nullptr) {
        error = "DiligentPickBuffer::EnsureSize got no render device";
        return false;
    }
    if (width == 0 || height == 0) {
        error = "DiligentPickBuffer::EnsureSize got a zero-sized viewport";
        return false;
    }
    if (impl_->color != nullptr && impl_->width == width && impl_->height == height)
        return true;

    // ⚠️ RELEASED BEFORE THE NEW ONES ARE CREATED, not after. Two full-resolution
    // RGBA8 + D32 pairs alive at once is ~12 MB at 1080p for no reason, and on a
    // resize storm the peak is what fails.
    impl_->ReleaseTarget ();

    Diligent::TextureDesc cd;
    cd.Name = "ArchViz pick ids";
    cd.Type = Diligent::RESOURCE_DIM_TEX_2D;
    cd.Width = width;
    cd.Height = height;
    cd.MipLevels = 1;
    // ⚠️ UNORM, NEVER _SRGB. See the header: an sRGB target encodes on write and
    // the id comes back as a different, entirely valid element.
    cd.Format = static_cast<Diligent::TEXTURE_FORMAT> (PickColorFormat ());
    cd.BindFlags = Diligent::BIND_RENDER_TARGET;
    cd.Usage = Diligent::USAGE_DEFAULT;
    device->CreateTexture (cd, nullptr, &impl_->color);

    Diligent::TextureDesc dd;
    dd.Name = "ArchViz pick depth";
    dd.Type = Diligent::RESOURCE_DIM_TEX_2D;
    dd.Width = width;
    dd.Height = height;
    dd.MipLevels = 1;
    dd.Format = static_cast<Diligent::TEXTURE_FORMAT> (PickDepthFormat ());
    dd.BindFlags = Diligent::BIND_DEPTH_STENCIL;
    dd.Usage = Diligent::USAGE_DEFAULT;
    device->CreateTexture (dd, nullptr, &impl_->depth);

    if (impl_->color == nullptr || impl_->depth == nullptr) {
        error = "Diligent CreateTexture failed for the pick id target or its depth buffer";
        impl_->ReleaseTarget ();
        return false;
    }

    impl_->rtv = impl_->color->GetDefaultView (Diligent::TEXTURE_VIEW_RENDER_TARGET);
    impl_->dsv = impl_->depth->GetDefaultView (Diligent::TEXTURE_VIEW_DEPTH_STENCIL);
    if (impl_->rtv == nullptr || impl_->dsv == nullptr) {
        error = "the pick id target has no render-target view or no depth view";
        impl_->ReleaseTarget ();
        return false;
    }

    impl_->width = width;
    impl_->height = height;
    return true;
}

void DiligentPickBuffer::Begin (Diligent::IDeviceContext* context)
{
    if (context == nullptr || !impl_->ready || impl_->color == nullptr)
        return;

    Diligent::ITextureView* rtv = impl_->rtv;
    context->SetRenderTargets (1, &rtv, impl_->dsv,
                               Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // ⚠️ CLEARED TO ZERO. Id 0 is "nothing", which is what makes a click on the
    // sky representable rather than resolving to whichever element happened to
    // be numbered 0.
    const float zero[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    context->ClearRenderTarget (rtv, zero,
                                Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->ClearDepthStencil (impl_->dsv, Diligent::CLEAR_DEPTH_FLAG, 1.0f, 0,
                                Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // ⚠️ AN EXPLICIT VIEWPORT, for the same reason DiligentShadowMap::Begin has
    // one: `SetViewports (1, nullptr, 0, 0)` takes the dimensions of the bound
    // render target as Diligent LAST RECORDED THEM, which is the swap chain's
    // size -- and this target only usually agrees with it. Stating it here means
    // the id pass rasterises over exactly the pixels the picture occupies, which
    // is the entire premise of the G-buffer: pixel (x, y) here IS pixel (x, y)
    // on screen.
    Diligent::Viewport viewport;
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = float (impl_->width);
    viewport.Height = float (impl_->height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context->SetViewports (1, &viewport, impl_->width, impl_->height);
}

void DiligentPickBuffer::End (Diligent::IDeviceContext* context)
{
    if (context == nullptr || !impl_->ready)
        return;

    // ⚠️ UNBIND BEFORE THE COPY IN Request. D3D11 refuses to copy a resource that
    // is still bound for output, and the refusal is a validation message rather
    // than a failure -- the staging texture simply keeps its previous contents,
    // so the click resolves to whatever the LAST click was over. The same trap
    // the first PLAT-RE22 readback fell into, and the reason this is its own call
    // rather than the first line of Request: the caller has to put its own
    // targets back afterwards either way, so the seam is where it can see it.
    context->SetRenderTargets (0, nullptr, nullptr,
                               Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

bool DiligentPickBuffer::Request (Diligent::IDeviceContext* context, int32_t px, int32_t py,
                                  uint64_t currentFrame, uint32_t tag)
{
    if (context == nullptr || !impl_->ready || impl_->color == nullptr || impl_->pending)
        return false;

    // Where to read, and where the cursor sits in what is read. ⚠️ THE
    // ARITHMETIC IS IN ArchViz/PickVote, not here, for the reason the vote is:
    // it decides clicks, it is pure, and it is wrong only near the frame's edges
    // -- so it is covered by the offline suite rather than by a human clicking
    // near the corner of a viewport.
    //
    // ⚠️ AN INVALID PLAN MEANS FALSE, WITH NOTHING PENDING -- the caller must be
    // able to tell "no request was made" from "a request is in flight", or a
    // click that landed outside would leave it waiting for a readback that never
    // arrives.
    const PickReadback plan =
        PlanPickReadback (px, py, impl_->width, impl_->height, kReadbackSize);
    if (!plan.valid)
        return false;

    Diligent::Box srcBox;
    srcBox.MinX = plan.minX;
    srcBox.MaxX = plan.minX + plan.width;
    srcBox.MinY = plan.minY;
    srcBox.MaxY = plan.minY + plan.height;
    srcBox.MinZ = 0;
    srcBox.MaxZ = 1;

    Diligent::CopyTextureAttribs copy;
    copy.pSrcTexture = impl_->color;
    copy.pSrcBox = &srcBox;
    copy.SrcTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    copy.pDstTexture = impl_->staging;
    copy.DstX = 0;
    copy.DstY = 0;
    copy.DstTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    context->CopyTexture (copy);

    impl_->pending = true;
    impl_->readyAtFrame = currentFrame + kReadbackDelayFrames;
    impl_->tag = tag;
    impl_->boxWidth = plan.width;
    impl_->boxHeight = plan.height;
    impl_->centreX = plan.centreX;
    impl_->centreY = plan.centreY;
    return true;
}

bool DiligentPickBuffer::Poll (Diligent::IDeviceContext* context, uint64_t currentFrame,
                               uint32_t& outId, uint32_t& outTag)
{
    if (context == nullptr || !impl_->ready || !impl_->pending ||
        currentFrame < impl_->readyAtFrame)
        return false;

    impl_->pending = false;
    outTag = impl_->tag;

    Diligent::MappedTextureSubresource mapped;
    context->MapTextureSubresource (impl_->staging, 0, 0, Diligent::MAP_READ,
                                    Diligent::MAP_FLAG_NONE, nullptr, mapped);
    if (mapped.pData == nullptr)
        return false;

    // ⚠️ THE ROW STRIDE IS NOT width*4. A staging texture is padded to the
    // driver's alignment, so an 8-pixel row can be 32 bytes or 256; walking it
    // as if it were tight reads the row below and votes on garbage.
    const uint8_t* base = static_cast<const uint8_t*> (mapped.pData);
    uint32_t ids[kReadbackSize * kReadbackSize] = {};
    for (uint32_t y = 0; y < impl_->boxHeight; ++y) {
        const uint8_t* row = base + size_t (y) * size_t (mapped.Stride);
        for (uint32_t x = 0; x < impl_->boxWidth; ++x) {
            const uint8_t* p = row + size_t (x) * 4;
            // ⚠️ RGBA, NOT bgfx's BGRA -- the target is ours and is declared
            // RGBA8_UNORM. See the header.
            ids[y * impl_->boxWidth + x] =
                (uint32_t (p[0]) << 16) | (uint32_t (p[1]) << 8) | uint32_t (p[2]);
        }
    }
    context->UnmapTextureSubresource (impl_->staging, 0, 0);

    // Which id the user meant. ⚠️ THE RULE LIVES IN ArchViz/PickVote, which has
    // no GPU dependency and is covered by the offline suite -- it has been wrong
    // three times and each correction previously cost a build/sync/restart round
    // trip to a human clicking on a building.
    //
    // ⚠️ ResolvePickIdAt, NOT ResolvePickId: the cursor is only at the box's
    // middle when the box did not have to be clamped to an edge.
    outId = ResolvePickIdAt (ids, impl_->boxWidth, impl_->boxHeight, impl_->centreX,
                             impl_->centreY);
    return true;
}

}   // namespace archviz
}   // namespace geomsrv
