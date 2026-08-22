#ifndef EVP_ARCHVIZ_DILIGENTPICKBUFFER_HPP
#define EVP_ARCHVIZ_DILIGENTPICKBUFFER_HPP

// ArchViz/DiligentPickBuffer — which element is under the cursor, answered by
// the GPU.
//
// ⚠️ IT IS AN ID G-BUFFER NOW, NOT A SNIPERSCOPE, AND THAT IS THE FIX FOR
// PLAT-RE136. The old shape rendered the ids through a SECOND camera built
// around the cursor — a narrow frustum (or, after PLAT-RE62, a sliding parallel
// box) aimed down the view ray into an 8x8 target. That put TWO descriptions of
// one camera in the code, and every one of the reported picking faults was the
// two disagreeing:
//
//   * the aim camera's field of view had to be re-derived from the main one, and
//     a constant there sampled a 50-pixel box around the cursor (PLAT-RE130);
//   * the parallel view needed a different aim ENTIRELY, so picking was simply
//     switched OFF in an axonometric projection and a click did nothing at all;
//   * the aim was computed against a viewport size that was arrived at
//     separately from the swap chain's, so at 125% display scaling the error was
//     zero at the centre and grew towards the edges (PLAT-RE139);
//   * anything the main camera gains that the aim does not — a roll, a rebased
//     origin, a near-plane change — silently reintroduces the same class of bug.
//
// So the ids are now rendered ONCE PER PICK AT THE VIEWPORT'S OWN RESOLUTION,
// WITH THE DISPLAYED IMAGE'S NOMINAL VIEW-PROJECTION. A temporal effect may
// jitter one geometry sample, but its accumulated output and picking share this
// unjittered pixel grid. There is nothing left to aim: pixel (x, y) of the id
// buffer IS pixel (x, y) of the picture the user clicked on, by construction, in
// every projection and at every DPI. The readback is then a small box copied out
// around the cursor.
//
// The cost is one extra geometry pass at full resolution on the frames a pick is
// wanted — depth and a flat colour, no shading, no shadow lookup — against the
// old pass's identical draw-call count into 8x8. The caller throttles hover picks
// so that is not every frame.
//
// ⚠️ THE PICK TARGET IS RGBA8_UNORM AND MUST NEVER BE _SRGB. The swap chain's
// render-target view is sRGB, so the hardware encodes colours on write; an id is
// not a colour, and an sRGB target would store 188 for a byte of 128 and resolve
// the click to a completely different, entirely valid element. The same trap
// that PLAT-RE24 decoded for the clear check, in the one place where it silently
// selects the wrong thing instead of looking wrong.
//
// ⚠️ AND THE READBACK IS RGBA, NOT BGRA. bgfx handed back the BACK BUFFER's
// native D3D11 order, which is why the old `PickBuffer::Poll` reassembled the id
// from p[2],p[1],p[0]. This target is ours and is declared RGBA8_UNORM, so the
// mapped bytes really are R,G,B,A. Copying bgfx's swizzle across would swap the
// id's high and low bytes -- again landing on a different valid element.
//
// ⚠️ THE ANSWER ARRIVES LATE, AND THAT IS NOT A BUG. The GPU is pipelined, so
// the copy issued this frame is not finished this frame. `Poll` refuses to map
// until `kReadbackDelayFrames` presents have gone by; reading earlier returns
// the PREVIOUS pick, i.e. every click selecting what the last one was over.
//
// ⚠️ RENDER THREAD ONLY. It owns Diligent resources and uses the immediate
// context.
//
// ⚠️ THE HEADER IS Diligent-FREE on purpose, like DiligentScene.hpp: the two
// formats are handed to the scene so its pick pipeline records them, and the
// scene's header must not pull Diligent's in.

#include <cstdint>
#include <memory>
#include <string>

namespace Diligent {
struct IRenderDevice;
struct IDeviceContext;
}   // namespace Diligent

namespace geomsrv {
namespace archviz {

// The formats the pick pass renders into, as raw `Diligent::TEXTURE_FORMAT`
// values. The scene's pick pipeline state records them, so a mismatch fails at
// pipeline creation rather than at draw time.
uint32_t PickColorFormat ();
uint32_t PickDepthFormat ();

class DiligentPickBuffer final {
public:
    DiligentPickBuffer ();
    ~DiligentPickBuffer ();
    DiligentPickBuffer (const DiligentPickBuffer&) = delete;
    DiligentPickBuffer& operator= (const DiligentPickBuffer&) = delete;

    // Creates the CPU-side readback texture, which never changes size. The id
    // target itself is created by `EnsureSize`, because it tracks the viewport.
    //
    // `error` carries the reason on false. A viewport with no picking is worth
    // far more than no viewport, so the caller reports and carries on.
    bool Init (Diligent::IRenderDevice* device, std::string& error);
    void Shutdown ();

    // True once Init succeeded. ⚠️ NOT the same as having a target to draw into
    // -- see `EnsureSize`, which is what a pick actually needs.
    bool IsReady () const;

    // (Re)create the id target at the viewport's pixel size. Cheap and a no-op
    // when the size has not changed, so the caller may simply call it before
    // every pick rather than tracking resizes itself.
    //
    // ⚠️ NEVER WHILE THE TARGET IS BOUND, and never between Begin and Request:
    // it releases the textures the context is holding. Call it first, or not at
    // all this frame.
    bool EnsureSize (Diligent::IRenderDevice* device, uint32_t width, uint32_t height,
                     std::string& error);

    // Bind the id target and clear it. ⚠️ CLEARED TO ZERO, and that is what
    // makes "the user clicked the sky" representable: id 0 is reserved for
    // nothing. Sets the viewport explicitly to the id target's full size.
    void Begin (Diligent::IDeviceContext* context);

    // Unbind. ⚠️ IT LEAVES NOTHING BOUND, exactly like DiligentShadowMap::End --
    // the caller binds its own targets next. Separate from `Request` because
    // D3D11 refuses to copy a resource that is still bound for output.
    void End (Diligent::IDeviceContext* context);

    // Copy the block around viewport pixel (px, py) to the staging texture.
    // False when the cursor is outside the target, in which case NOTHING is
    // pending and the caller has not lost a request it thinks is in flight.
    //
    // `tag` is carried through to the matching Poll and is opaque here. The
    // viewport uses it to say whether a request was a CLICK or a HOVER: the two
    // are the same GPU work over the same target, and the answer means entirely
    // different things -- one changes the selection, the other only draws the
    // hover outline and fills in a callout. Keeping them one request rather than
    // two buffers is what stops a hover in flight from delaying a click behind a
    // second readback.
    bool Request (Diligent::IDeviceContext* context, int32_t px, int32_t py,
                  uint64_t currentFrame, uint32_t tag);

    // True once the pixels for the last Request are readable, with the winning
    // id in `outId` (0 = nothing was under the cursor) and the Request's own
    // `tag` in `outTag`. Call once per frame, with the same monotonically
    // increasing frame counter Request was given.
    bool Poll (Diligent::IDeviceContext* context, uint64_t currentFrame, uint32_t& outId,
               uint32_t& outTag);

    bool HasPendingRequest () const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}   // namespace archviz
}   // namespace geomsrv

#endif
