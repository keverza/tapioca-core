#ifndef EVP_ARCHVIZ_DXGI_SHAREDOVERLAYSURFACE_HPP
#define EVP_ARCHVIZ_DXGI_SHAREDOVERLAYSURFACE_HPP

// The overlay's finished frame, made visible to ARCHICAD'S D3D11 device
// (PLAT-RE79, phase 4). Producer half; HostComposite is the consumer.
//
// WHY A SHARED TEXTURE AND NOT A SHARED DEVICE. The obvious route is to attach
// Diligent to Archicad's own D3D11 device and render straight into its back
// buffer -- one copy fewer, and the camera could in principle be applied inside
// the Present detour. It was rejected: it puts our pipeline states, our shaders
// and our resource lifetimes on a device we do not own and cannot reset, on a
// context Archicad drives from its own thread, where a device-removed event or a
// state leak becomes Archicad's crash rather than our black frame. It also
// deletes the fallback -- there would be no composition path left to switch back
// to, and `mode=legacy` is the thing that makes every rung of this ladder
// reversible.
//
// What this does instead: the overlay renders exactly as it does today, into its
// own composition chain on its own device, and the finished frame is COPIED into
// a texture the two devices share. Archicad's Present detour blits that texture
// over the frame it is already about to show. The overlay path is untouched and
// still works with the hook off.
//
// ⚠️ THE PIXELS ARE PREMULTIPLIED, and every consumer has to know it. The
// composition chain is premultiplied by construction (PLAT-RE37), so the copy is
// too, and the host must blend with ONE / INV_SRC_ALPHA. Blending it as
// straight alpha darkens every edge in the overlay and looks like a shader bug.
//
// ⚠️ THE KEYED MUTEX IS NEVER WAITED ON, BY EITHER SIDE. Both acquire with a
// zero timeout and skip their turn when the other holds it. A producer that
// blocked would stall our render thread on Archicad's frame rate; a consumer
// that blocked would stall ARCHICAD's render thread on ours, which is a hang in
// somebody else's application caused by a diagnostic of ours. Skipping costs a
// repeated frame and nothing else.
//
// ⚠️ RENDER THREAD (OURS) ONLY, except Handle()/Generation(), which the host
// side reads.

#include <cstdint>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace geomsrv {
namespace archviz {
namespace dxgi {

// Create or resize the shared texture. Idempotent for an unchanged size.
// False with `error` filled leaves nothing behind.
bool EnsureSharedOverlaySurface (ID3D11Device* device, uint32_t width, uint32_t height,
                                 std::string& error);

// The plan pose a shared frame was DRAWN with, so the host can reproject it.
//
// ⚠️ WITHOUT THIS THE COMPOSITED IMAGE IS SIMPLY LATE. Blitting a finished frame
// into Archicad's own present fixes WHEN the pixels land and nothing about what
// they contain: the frame was drawn from a camera sampled one poll earlier. A
// plan overlay is a parallel projection, so the correction is exactly a 2D
// affine warp -- but only if the pose it was drawn with travels with it.
struct SharedOverlayPose {
    bool   valid = false;      // false for a perspective frame: no plan warp
    double centreX = 0.0;
    double centreY = 0.0;
    double halfHeightMetres = 0.0;
    double rotationRadians = 0.0;
};

// Copy this frame's overlay into the shared texture and hand it to the host,
// with the pose it was drawn with. Call AFTER the frame is rendered and BEFORE
// it is presented. No-op when the surface does not exist or the host currently
// holds the mutex.
void PublishSharedOverlayFrame (ID3D11DeviceContext* context, ID3D11Texture2D* frame,
                                const SharedOverlayPose& pose);

// The pose of the frame currently IN the shared texture. Host side, render
// thread: published before the mutex is released, so a reader that has acquired
// the texture is reading the pose that belongs to it.
SharedOverlayPose SharedOverlayFramePose ();

// The newest plan pose Archicad is known to be at -- what the overlay SHOULD be
// showing right now. Published by the main-thread camera tick, read by the
// Present detour, which cannot call ACAPI itself.
//
// ⚠️ IT IS THE PREDICTED POSE, THE SAME ONE THE RENDERER IS GIVEN. Reprojecting
// onto the raw observation would hand back the poll interval this whole ladder
// has spent itself removing.
void SetLatestPlanPose (const SharedOverlayPose& pose);
SharedOverlayPose LatestPlanPose ();

// Release everything. Call before the device goes.
void DestroySharedOverlaySurface ();

// The NT handle the host opens, as an integer so it can cross a plain interface.
// 0 when there is no surface.
uint64_t SharedOverlayHandle ();

// Bumped once per published frame. The host uses it to tell "no new frame yet"
// from "the producer has stopped", which look identical from one sample.
uint64_t SharedOverlayGeneration ();

uint32_t SharedOverlayWidth ();
uint32_t SharedOverlayHeight ();

}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv

#endif
