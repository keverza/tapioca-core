#ifndef EVP_ARCHVIZ_DXGI_GUESTGPU_HPP
#define EVP_ARCHVIZ_DXGI_GUESTGPU_HPP

// ArchViz/Dxgi/GuestGpu -- the Diligent objects the overlays' guests draw with, and
// the draw itself: pipelines built from GuestShaderSources.hpp, the content of
// ArchViz/OverlayScene.hpp uploaded as immutable buffers, the atlas pages as
// textures, and the passes that put them on Archicad's frame.
//
// ⚠️ PLUMBING, NOT A RENDERER (§12). This knows how to build a pipeline from a
// source and draw prepared arrays; it does not know where the source's projection
// comes from. The plan guest hands it the plan's source and frame constants, the
// 3D guest the camera-composed source and its own -- the two never meet here.
//
// ⚠️ THE PASS TABLE, ONCE. 3D with a depth view: an item hidden behind the building
// is drawn "near" (LESS_EQUAL); faded or dashed, "near" then "behind" (GREATER);
// shown, "over" (untested). Without a depth view -- the host occluder not ready --
// everything is drawn "all": untested and whole, which is the reference
// wireframe's own rule for the same case. The plan is always "over" (finding 13).
//
// ⚠️ NOTHING HERE IS BOUND BY HAND. Every binding goes through Diligent after the
// guest's `BeginDraw`, and the caller's ScopedPipelineState puts Archicad's back.
// The slots Diligent can reach are within what the guard saves: one vertex buffer,
// two constant buffers plus the camera's two, one SRV and one sampler per stage.
//
// THREAD: the owning guest's.

#include "ArchViz/OverlayScene.hpp"

#include <windows.h>
#include <d3d11_1.h> // Must precede Diligent's D3D11 interop headers (Probe 1a).
#include <Buffer.h>
#include <DeviceContext.h>
#include <PipelineState.h>
#include <RefCntAutoPtr.hpp>
#include <RenderDevice.h>
#include <ShaderResourceBinding.h>
#include <Texture.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace guestgpu {

enum class Kind : uint8_t { Plan = 0, Scene = 1 };

// The passes, as GuestShaderSources.hpp numbers them.
constexpr uint32_t kNear = 0;
constexpr uint32_t kBehind = 1;
constexpr uint32_t kOver = 2;
constexpr uint32_t kAll = 3;
constexpr uint32_t kDepthModes = 3; // near, behind, over; "all" draws with "over"'s state

// `cbuffer GuestDraw`, byte for byte.
struct DrawConstants {
    float mode[4];
    float ramp[4];
    float iso[4];
    float isoColour[4];
    float stopAt[16];
    float stopColour[16][4];
    float atlas[4];
};
static_assert (sizeof (DrawConstants) == 400, "GuestDraw is 25 float4s");

struct Pipelines {
    Kind kind = Kind::Plan;
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> fill[kDepthModes][2]; // [depth][cull back]
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> line[kDepthModes];
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> glyph[kDepthModes];
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> fillSrb;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> lineSrb;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> frame; // GuestPlanFrame or GuestSceneFrame
    Diligent::RefCntAutoPtr<Diligent::IBuffer> draw;  // GuestDraw
    uint32_t frameBytes = 0;
    bool ready = false;
};

// Compiles `source` -- the plan's, or the 3D body composed for one camera slot --
// and builds every pipeline the kind uses. The plan builds "over" only.
bool Build (Diligent::IRenderDevice* device, Kind kind, const std::string& source, Pipelines& out, std::string& error);

// One atlas page on the GPU, kept across content changes by the page's id.
struct Page {
    uint64_t id = 0;
    Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
    float invWidth = 0.0f;
    float invHeight = 0.0f;
};

struct Content {
    Diligent::RefCntAutoPtr<Diligent::IBuffer> fills;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> lines;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> glyphs;
    uint32_t fillStride = 0;
    uint32_t lineStride = 0;
    uint32_t glyphStride = 0;
    uint32_t lineCount = 0;
    std::vector<overlayscene::FillDraw> fillDraws;
    std::vector<overlayscene::GlyphDraw> glyphDraws;
    std::vector<uint32_t> pageSlot; // GlyphDraw::page -> index into the page cache
    uint64_t generation = 0;

    bool Empty () const
    {
        return fillDraws.empty () && lineCount == 0 && glyphDraws.empty ();
    }
};

struct Arrays {
    const void* fills = nullptr;
    size_t fillCount = 0;
    size_t fillStride = 0;
    const void* lines = nullptr;
    size_t lineCount = 0;
    size_t lineStride = 0;
    const void* glyphs = nullptr;
    size_t glyphCount = 0;
    size_t glyphStride = 0;
};

// Replaces `out` with the new content's buffers; new atlas pages join `pages`.
bool Upload (Diligent::IRenderDevice* device, const Pipelines& pipelines, std::vector<Page>& pages,
             const Arrays& arrays, const std::vector<overlayscene::FillDraw>& fillDraws,
             const std::vector<overlayscene::GlyphDraw>& glyphDraws,
             const std::vector<std::shared_ptr<const overlaytext::Page>>& atlas, Content& out, std::string& error);

// 3D: the camera copies, wrapped, on every binding that reads them.
void BindCamera (Pipelines& pipelines, std::vector<Page>& pages, Diligent::IBuffer* view,
                 Diligent::IBuffer* projection);

struct DrawStats {
    uint32_t drawCalls = 0;
    uint32_t mapFailures = 0;
};

// Draws the content: fills, then lines, then glyphs, each in its passes. Call after
// the guest's `BeginDraw`, inside the caller's ScopedPipelineState. `frame` is the
// frame cbuffer's bytes (`pipelines.frameBytes` of them).
void Draw (Diligent::IDeviceContext* context, const Pipelines& pipelines, const std::vector<Page>& pages,
           const Content& content, const void* frame, bool haveDepth, float dpiScale, DrawStats& stats);

} // namespace guestgpu
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
