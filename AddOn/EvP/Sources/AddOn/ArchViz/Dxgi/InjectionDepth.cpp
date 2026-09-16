// ArchViz/Dxgi/InjectionDepth -- see the header. Every rule about this file is
// in that header's comments; this is the mechanism.

#include "ArchViz/Dxgi/InjectionDepth.hpp"

#include <d3d11_1.h>

#include <atomic>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace injection {
namespace depth {

namespace {

ID3D11Device*            g_device = nullptr;
ID3D11DepthStencilView*  g_sceneView = nullptr;

// The private copy, and the description it was built for.
ID3D11Texture2D*         g_privateTexture = nullptr;
ID3D11DepthStencilView*  g_privateView = nullptr;
D3D11_TEXTURE2D_DESC     g_privateDesc = {};
DXGI_FORMAT              g_privateViewFormat = DXGI_FORMAT_UNKNOWN;
bool                     g_privateReady = false;

ID3D11DepthStencilState* g_testOnly = nullptr;
ID3D11DepthStencilState* g_testAndWrite = nullptr;

std::atomic<uint32_t> g_mode {uint32_t (Mode::Off)};
Stats g_stats;

template <typename T>
void ReleaseAndNull (T*& object)
{
    if (object != nullptr) {
        object->Release ();
        object = nullptr;
    }
}

void Fail (const char* what)
{
    strncpy_s (g_stats.lastError, sizeof (g_stats.lastError), what, _TRUNCATE);
}

bool EnsureStates (ID3D11DeviceContext* context)
{
    if (g_testOnly != nullptr && g_testAndWrite != nullptr)
        return true;
    if (g_device == nullptr) {
        context->GetDevice (&g_device);
        if (g_device == nullptr)
            return false;
    }

    // ⚠️ `LESS_EQUAL`, AND EQUAL DEPTHS ARE KEPT. A ghost surface coincident
    // with a wall should be visible on it rather than z-fighting away, and Proof
    // B measured this comparison against Archicad's own buffer rather than
    // assuming it.
    D3D11_DEPTH_STENCIL_DESC desc = {};
    desc.DepthEnable = TRUE;
    desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    bool ok = SUCCEEDED (g_device->CreateDepthStencilState (&desc, &g_testOnly));

    // ⚠️ WRITES ARE ON ONLY FOR THE PRIVATE COPY, and that is the entire reason
    // the copy exists: our own primitives can occlude each other without a
    // single write reaching Archicad's depth buffer.
    desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ok = ok && SUCCEEDED (g_device->CreateDepthStencilState (&desc, &g_testAndWrite));
    if (!ok)
        Fail ("a depth-stencil state could not be created");
    return ok;
}

// The DSV format that matches a (possibly typeless) depth texture format.
DXGI_FORMAT DepthViewFormat (DXGI_FORMAT resource)
{
    switch (resource) {
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
            return DXGI_FORMAT_D32_FLOAT;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
            return DXGI_FORMAT_D16_UNORM;
        default:
            return resource;
    }
}

void ReleasePrivate ()
{
    ReleaseAndNull (g_privateView);
    ReleaseAndNull (g_privateTexture);
    g_privateReady = false;
    g_stats.privateReady = false;
}

// ⚠️ THE COPY IS REBUILT WHENEVER THE SOURCE DESCRIPTION CHANGES, which is what
// makes a window resize safe: the old texture is dropped rather than copied into
// at the wrong size, and `rebuilds` says how often that happened.
bool EnsurePrivate (ID3D11DeviceContext* context, ID3D11Texture2D* source)
{
    D3D11_TEXTURE2D_DESC desc = {};
    source->GetDesc (&desc);

    if (g_privateReady &&
        desc.Width == g_privateDesc.Width && desc.Height == g_privateDesc.Height &&
        desc.Format == g_privateDesc.Format &&
        desc.SampleDesc.Count == g_privateDesc.SampleDesc.Count &&
        desc.ArraySize == g_privateDesc.ArraySize &&
        desc.MipLevels == g_privateDesc.MipLevels) {
        return true;
    }

    ReleasePrivate ();
    if (!EnsureStates (context))
        return false;
    ++g_stats.rebuilds;

    // ⚠️ THE SAME DESCRIPTION, BECAUSE `CopyResource` REQUIRES IT. Identical
    // dimensions, format, sample count, array size and mip levels -- only the
    // bind flags are ours, and they are the minimum that lets us render into it.
    D3D11_TEXTURE2D_DESC ours = desc;
    ours.Usage = D3D11_USAGE_DEFAULT;
    ours.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ours.CPUAccessFlags = 0;
    ours.MiscFlags = 0;
    if (FAILED (g_device->CreateTexture2D (&ours, nullptr, &g_privateTexture))) {
        Fail ("the private depth texture could not be created");
        return false;
    }

    D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc = {};
    viewDesc.Format = DepthViewFormat (desc.Format);
    viewDesc.ViewDimension = desc.SampleDesc.Count > 1
            ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED (g_device->CreateDepthStencilView (g_privateTexture, &viewDesc,
                                                  &g_privateView))) {
        Fail ("the private depth view could not be created");
        ReleasePrivate ();
        return false;
    }

    g_privateDesc = desc;
    g_privateViewFormat = viewDesc.Format;
    g_privateReady = true;
    g_stats.privateReady = true;
    g_stats.width = desc.Width;
    g_stats.height = desc.Height;
    g_stats.format = uint32_t (desc.Format);
    g_stats.sampleCount = desc.SampleDesc.Count;
    return true;
}

}   // namespace

void SetMode (Mode mode)
{
    g_mode.store (uint32_t (mode), std::memory_order_release);
}

Mode GetMode ()
{
    return Mode (g_mode.load (std::memory_order_acquire));
}

void RetainSceneView (ID3D11DepthStencilView* view)
{
    if (g_sceneView == view)
        return;
    ReleaseAndNull (g_sceneView);
    g_sceneView = view;
    if (g_sceneView != nullptr)
        g_sceneView->AddRef ();
}

ID3D11DepthStencilView* PrepareForInjection (ID3D11DeviceContext* context)
{
    const Mode mode = GetMode ();
    if (mode == Mode::Off || context == nullptr)
        return nullptr;
    ++g_stats.preparations;
    if (g_sceneView == nullptr) {
        ++g_stats.noSceneView;
        return nullptr;
    }
    if (!EnsureStates (context))
        return nullptr;
    if (mode == Mode::SceneReadOnly)
        return g_sceneView;

    // ---- PrivateCopy -------------------------------------------------------
    ID3D11Resource* sourceResource = nullptr;
    g_sceneView->GetResource (&sourceResource);
    if (sourceResource == nullptr) {
        ++g_stats.copyRefused;
        return nullptr;
    }
    ID3D11Texture2D* source = nullptr;
    if (FAILED (sourceResource->QueryInterface (__uuidof (ID3D11Texture2D),
                                                (void**) &source)) ||
        source == nullptr) {
        ++g_stats.copyRefused;
        ReleaseAndNull (sourceResource);
        return nullptr;
    }
    ID3D11DepthStencilView* result = nullptr;
    if (EnsurePrivate (context, source)) {
        // ⚠️ ONE WHOLE-RESOURCE COPY PER INJECTED FRAME, GPU TO GPU. It carries
        // Archicad's depth into our texture so the building still occludes, and
        // from here every write is ours.
        context->CopyResource (g_privateTexture, source);
        ++g_stats.copies;
        result = g_privateView;
    }
    ReleaseAndNull (source);
    ReleaseAndNull (sourceResource);
    return result;
}

ID3D11DepthStencilState* StateForMode (ID3D11DeviceContext* context)
{
    if (!EnsureStates (context))
        return nullptr;
    return GetMode () == Mode::PrivateCopy ? g_testAndWrite : g_testOnly;
}

void Shutdown ()
{
    ReleasePrivate ();
    ReleaseAndNull (g_testAndWrite);
    ReleaseAndNull (g_testOnly);
    ReleaseAndNull (g_sceneView);
    ReleaseAndNull (g_device);
    g_privateDesc = D3D11_TEXTURE2D_DESC {};
    g_stats = Stats {};
}

Stats GetStats ()
{
    return g_stats;
}

}   // namespace depth
}   // namespace injection
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv
