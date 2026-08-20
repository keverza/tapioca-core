// ArchViz/Dxgi/SharedOverlaySurface -- see the header for why the two devices
// share a texture rather than a device. OUR render thread.

#include "ArchViz/Dxgi/SharedOverlaySurface.hpp"

#include "ArchViz/ArchVizLog.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <atomic>
#include <mutex>

namespace geomsrv {
namespace archviz {
namespace dxgi {

namespace {

ID3D11Texture2D*  g_surface = nullptr;
IDXGIKeyedMutex*  g_mutex = nullptr;
HANDLE            g_handle = nullptr;
uint32_t          g_width = 0;
uint32_t          g_height = 0;
std::atomic<uint64_t> g_generation {0};

// ⚠️ A MUTEX, NOT ATOMICS, AND IT IS NEVER HELD ACROSS A GPU CALL. A pose is
// five values that only mean anything together -- a torn read mixes one frame's
// centre with another's zoom, which is a warp to somewhere neither camera was.
// The critical sections here are half a dozen assignments.
std::mutex g_poseMutex;
SharedOverlayPose g_framePose;    // what is IN the shared texture
SharedOverlayPose g_latestPose;   // where Archicad is NOW, from the camera tick

// ⚠️ THE TWO KEYS ARE A PROTOCOL, NOT LABELS. 0 means "the producer may write",
// 1 means "the host may read". Each side releases with the other's key, so the
// texture is never read while it is half-copied -- which on a 3000x1800 surface
// would be a visible horizontal tear through the overlay, appearing only under
// load and looking exactly like a sync bug in the camera.
constexpr UINT64 kProducerKey = 0;
constexpr UINT64 kConsumerKey = 1;

void ReleaseAll ()
{
    if (g_mutex != nullptr) {
        g_mutex->Release ();
        g_mutex = nullptr;
    }
    if (g_surface != nullptr) {
        g_surface->Release ();
        g_surface = nullptr;
    }
    // ⚠️ THE HANDLE IS OURS TO CLOSE. CreateSharedHandle returns an NT handle
    // that outlives the texture; leaking one per resize is a handle leak in
    // Archicad's process, which shows up as nothing at all until it shows up as
    // everything at once.
    if (g_handle != nullptr) {
        CloseHandle (g_handle);
        g_handle = nullptr;
    }
    g_width = 0;
    g_height = 0;
}

}   // namespace

bool EnsureSharedOverlaySurface (ID3D11Device* device, uint32_t width, uint32_t height,
                                 std::string& error)
{
    if (device == nullptr || width == 0 || height == 0) {
        error = "no device or a zero-sized surface";
        return false;
    }
    if (g_surface != nullptr && g_width == width && g_height == height)
        return true;

    ReleaseAll ();

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    // ⚠️ THE COMPOSITION CHAIN'S OWN FORMAT. A mismatch here would make
    // CopyResource fail outright rather than convert -- D3D11 copies bits, it
    // does not translate formats.
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    // ⚠️ NTHANDLE + KEYEDMUTEX, NOT THE LEGACY `SHARED` FLAG. The legacy flag
    // gives an untracked handle with no synchronisation at all, which works
    // until the two devices happen to touch the texture in the same instant.
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                     D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    HRESULT hr = device->CreateTexture2D (&desc, nullptr, &g_surface);
    if (FAILED (hr) || g_surface == nullptr) {
        error = "CreateTexture2D for the shared overlay surface failed (0x" +
                std::to_string ((unsigned) hr) + ")";
        ReleaseAll ();
        return false;
    }

    hr = g_surface->QueryInterface (__uuidof (IDXGIKeyedMutex), (void**) &g_mutex);
    if (FAILED (hr) || g_mutex == nullptr) {
        error = "the shared surface has no keyed mutex (0x" + std::to_string ((unsigned) hr) + ")";
        ReleaseAll ();
        return false;
    }

    IDXGIResource1* resource = nullptr;
    hr = g_surface->QueryInterface (__uuidof (IDXGIResource1), (void**) &resource);
    if (SUCCEEDED (hr) && resource != nullptr) {
        hr = resource->CreateSharedHandle (nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &g_handle);
        resource->Release ();
    }
    if (FAILED (hr) || g_handle == nullptr) {
        error = "CreateSharedHandle for the overlay surface failed (0x" +
                std::to_string ((unsigned) hr) + ")";
        ReleaseAll ();
        return false;
    }

    g_width = width;
    g_height = height;
    // ⚠️ THE GENERATION RESTARTS AT ZERO ON A RESIZE, and the host must treat a
    // generation going BACKWARDS as "a new surface, reopen it". Reusing the
    // counter across surfaces would have the host blitting a handle it had
    // already closed.
    g_generation.store (0, std::memory_order_release);
    ArchVizLog ("shared overlay surface: " + std::to_string (width) + "x" +
                std::to_string (height) + " ready for the host compositor");
    return true;
}

void PublishSharedOverlayFrame (ID3D11DeviceContext* context, ID3D11Texture2D* frame,
                                const SharedOverlayPose& pose)
{
    if (context == nullptr || frame == nullptr || g_surface == nullptr || g_mutex == nullptr)
        return;

    // Zero timeout -- see the header. Missing a turn repeats one overlay frame;
    // waiting would tie our render thread to Archicad's.
    if (g_mutex->AcquireSync (kProducerKey, 0) != S_OK)
        return;
    context->CopyResource (g_surface, frame);
    {
        // ⚠️ INSIDE THE MUTEX, BEFORE IT IS RELEASED. The pose describes the
        // pixels that were just copied; publishing it afterwards leaves a window
        // in which the host has the new image and the old pose, and reprojects
        // it through a difference that was already applied.
        std::lock_guard<std::mutex> lock (g_poseMutex);
        g_framePose = pose;
    }
    g_mutex->ReleaseSync (kConsumerKey);
    g_generation.fetch_add (1, std::memory_order_release);
}

SharedOverlayPose SharedOverlayFramePose ()
{
    std::lock_guard<std::mutex> lock (g_poseMutex);
    return g_framePose;
}

void SetLatestPlanPose (const SharedOverlayPose& pose)
{
    std::lock_guard<std::mutex> lock (g_poseMutex);
    g_latestPose = pose;
}

SharedOverlayPose LatestPlanPose ()
{
    std::lock_guard<std::mutex> lock (g_poseMutex);
    return g_latestPose;
}

void DestroySharedOverlaySurface ()
{
    ReleaseAll ();
    g_generation.store (0, std::memory_order_release);
}

uint64_t SharedOverlayHandle ()
{
    return uint64_t (uintptr_t (g_handle));
}

uint64_t SharedOverlayGeneration ()
{
    return g_generation.load (std::memory_order_acquire);
}

uint32_t SharedOverlayWidth ()  { return g_width; }
uint32_t SharedOverlayHeight () { return g_height; }

}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv
