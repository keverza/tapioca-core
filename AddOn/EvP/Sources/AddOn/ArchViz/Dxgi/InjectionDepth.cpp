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

ID3D11Device* g_device = nullptr;
ID3D11DepthStencilView* g_sceneView = nullptr;

// The private copy, and the description it was built for.
ID3D11Texture2D* g_privateTexture = nullptr;
ID3D11DepthStencilView* g_privateView = nullptr;
D3D11_TEXTURE2D_DESC g_privateDesc = {};
DXGI_FORMAT g_privateViewFormat = DXGI_FORMAT_UNKNOWN;
bool g_privateReady = false;

ID3D11DepthStencilState* g_testOnly = nullptr;
ID3D11DepthStencilState* g_testAndWrite = nullptr;

std::atomic<uint32_t> g_mode { uint32_t (Mode::Off) };

// ⚠️ AT PRESENT BY DEFAULT, WHICH IS THE BEHAVIOUR RUN FORTY-EIGHT
// SHIPPED AND THE ONE IT FALSIFIED. The default stays put so the sweep's first
// window reproduces the known-bad reading rather than quietly fixing it; a
// comparison needs its control.
std::atomic<uint32_t> g_source { uint32_t (Source::AtPresent) };
Stats g_stats;
Provenance g_provenance;

// Per-frame bookkeeping for the classification.
uint64_t g_frameGeneration = 0;
uint32_t g_drawIndexThisFrame = 0;
bool g_sawOpaqueThisFrame = false;
bool g_transitionedThisFrame = false;
bool g_frameOpen = false;

// ⚠️ WHICH FRAME THE SNAPSHOT IN THE PRIVATE TEXTURE BELONGS TO. A
// snapshot from the previous frame is a stale camera's depth, and binding it
// would occlude this frame's overlay with last frame's walls -- exactly the kind
// of one-frame error that looks like a transform bug in a picture.
uint64_t g_capturedGeneration = 0;
bool g_haveCapture = false;

template <typename T> void ReleaseAndNull (T*& object)
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

// ⚠️ ASKED OF THE CONTEXT, NOT TRACKED THROUGH A NEW HOOKED SLOT.
// `OMSetBlendState` and `OMSetDepthStencilState` are not patched, and patching
// them would change the build's pinned patch profile and force every pinned
// build to be re-pinned -- a high price for a fact two COM calls answer directly.
// `GetDesc` on a state object is a struct copy; at roughly a dozen scene draws a
// frame this is not a hot path, so nothing is cached and nothing can go stale.
bool BlendEnabledNow (ID3D11DeviceContext* context)
{
    ID3D11BlendState* state = nullptr;
    FLOAT factor[4] = {};
    UINT mask = 0;
    context->OMGetBlendState (&state, factor, &mask);
    bool enabled = false;
    if (state != nullptr) {
        D3D11_BLEND_DESC desc = {};
        state->GetDesc (&desc);
        // Independent blend means each target decides; target 0 is the one we
        // share with Archicad, so it is the one that matters.
        enabled = desc.RenderTarget[0].BlendEnable != FALSE;
        state->Release ();
    }
    return enabled; // no state bound at all is D3D11's default: no blending
}

void DepthBehaviourNow (ID3D11DeviceContext* context, bool& writesDepth, bool& testsDepth)
{
    ID3D11DepthStencilState* state = nullptr;
    UINT stencilRef = 0;
    context->OMGetDepthStencilState (&state, &stencilRef);
    // D3D11's default state: depth test on, writes on.
    writesDepth = true;
    testsDepth = true;
    if (state != nullptr) {
        D3D11_DEPTH_STENCIL_DESC desc = {};
        state->GetDesc (&desc);
        testsDepth = desc.DepthEnable != FALSE;
        writesDepth = testsDepth && desc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
        state->Release ();
    }
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

    if (g_privateReady && desc.Width == g_privateDesc.Width && desc.Height == g_privateDesc.Height &&
        desc.Format == g_privateDesc.Format && desc.SampleDesc.Count == g_privateDesc.SampleDesc.Count &&
        desc.ArraySize == g_privateDesc.ArraySize && desc.MipLevels == g_privateDesc.MipLevels) {
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
    ++g_stats.sourceDescAccepted;
    if (FAILED (g_device->CreateTexture2D (&ours, nullptr, &g_privateTexture))) {
        Fail ("the private depth texture could not be created");
        return false;
    }
    ++g_stats.textureCreated;

    D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc = {};
    viewDesc.Format = DepthViewFormat (desc.Format);
    viewDesc.ViewDimension =
        desc.SampleDesc.Count > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED (g_device->CreateDepthStencilView (g_privateTexture, &viewDesc, &g_privateView))) {
        Fail ("the private depth view could not be created");
        ReleasePrivate ();
        return false;
    }

    ++g_stats.viewCreated;
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

} // namespace

void SetMode (Mode mode)
{
    g_mode.store (uint32_t (mode), std::memory_order_release);
}

Mode GetMode ()
{
    return Mode (g_mode.load (std::memory_order_acquire));
}

void SetSource (Source source)
{
    g_source.store (uint32_t (source), std::memory_order_release);
}

Source GetSource ()
{
    return Source (g_source.load (std::memory_order_acquire));
}

Provenance GetProvenance ()
{
    return g_provenance;
}

void ResetProvenance ()
{
    g_provenance = Provenance {};
    g_frameOpen = false;
    g_drawIndexThisFrame = 0;
    g_sawOpaqueThisFrame = false;
    g_transitionedThisFrame = false;
    // ⚠️ THE CAPTURE ITSELF IS NOT DROPPED. Resetting the counters
    // between sweep windows must not throw away the snapshot the next window is
    // about to be judged on.
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

void CaptureBoundSceneView (ID3D11DeviceContext* context)
{
    if (context == nullptr)
        return;
    ID3D11RenderTargetView* boundTarget = nullptr;
    ID3D11DepthStencilView* boundDepth = nullptr;
    context->OMGetRenderTargets (1, &boundTarget, &boundDepth);
    ++g_stats.captureAttempts;
    if (boundDepth != nullptr) {
        RetainSceneView (boundDepth);
        boundDepth->Release ();
    }
    else {
        // Expected at Present, where Archicad binds the back buffer alone. Counted
        // rather than ignored: this returning silently every frame is what let the
        // header claim a fix that had not landed.
        ++g_stats.captureNoDepth;
    }
    if (boundTarget != nullptr)
        boundTarget->Release ();
}

// ⚠️ ONE WHOLE-RESOURCE COPY, GPU TO GPU, AT WHATEVER MOMENT THE
// SOURCE NAMES. It carries Archicad's depth into our texture so the building
// still occludes, and from there every write is ours. What changed in run
// forty-nine is only WHEN this is called.
bool Capture (ID3D11DeviceContext* context, uint64_t modelGeneration)
{
    if (context == nullptr || g_sceneView == nullptr)
        return false;
    if (!EnsureStates (context))
        return false;

    ID3D11Resource* sourceResource = nullptr;
    g_sceneView->GetResource (&sourceResource);
    if (sourceResource == nullptr) {
        ++g_stats.copyRefused;
        Fail ("the scene depth view has no resource");
        return false;
    }
    ID3D11Texture2D* source = nullptr;
    if (FAILED (sourceResource->QueryInterface (__uuidof (ID3D11Texture2D), (void**) &source)) || source == nullptr) {
        ++g_stats.copyRefused;
        Fail ("the scene depth resource is not a 2D texture");
        ReleaseAndNull (sourceResource);
        return false;
    }
    ++g_stats.sourceResourceAcquired;

    bool ok = false;
    if (EnsurePrivate (context, source)) {
        context->CopyResource (g_privateTexture, source);
        ++g_stats.copies;
        ++g_stats.copyIssued;
        g_capturedGeneration = modelGeneration;
        g_haveCapture = true;
        ok = true;
    }
    ReleaseAndNull (source);
    ReleaseAndNull (sourceResource);
    return ok;
}

void OnDraw (ID3D11DeviceContext* context, uint64_t boundDepthStencil, uint64_t modelGeneration)
{
    if (context == nullptr || g_sceneView == nullptr)
        return;
    // Only draws into the depth buffer the model was rendered with can put
    // anything into it that could occlude us.
    if (boundDepthStencil != uint64_t (uintptr_t (g_sceneView)))
        return;

    if (!g_frameOpen || modelGeneration != g_frameGeneration) {
        if (g_frameOpen) {
            if (!g_sawOpaqueThisFrame)
                ++g_provenance.framesWithoutOpaque;
            else if (!g_transitionedThisFrame)
                ++g_provenance.framesWithoutTransition;
        }
        g_frameGeneration = modelGeneration;
        g_frameOpen = true;
        g_drawIndexThisFrame = 0;
        g_sawOpaqueThisFrame = false;
        g_transitionedThisFrame = false;
    }

    ++g_drawIndexThisFrame;
    ++g_provenance.sceneDraws;
    g_provenance.lastSceneDrawIndex = g_drawIndexThisFrame;

    const bool blended = BlendEnabledNow (context);
    bool writesDepth = true;
    bool testsDepth = true;
    DepthBehaviourNow (context, writesDepth, testsDepth);
    if (!testsDepth)
        ++g_provenance.depthTestOffDraws;
    if (!writesDepth)
        ++g_provenance.noDepthWriteDraws;
    if (blended)
        ++g_provenance.blendedDraws;

    // ⚠️ OPAQUE MEANS UNBLENDED **AND** DEPTH-WRITING. A blended draw
    // that writes depth is the exact thing that ruins this buffer -- it leaves a
    // near value where you can see straight through -- and a draw that writes no
    // depth cannot have contributed to what is in it at all.
    const bool opaque = !blended && writesDepth;
    if (opaque) {
        ++g_provenance.opaqueDraws;
        g_sawOpaqueThisFrame = true;
        g_provenance.lastOpaqueDrawIndex = g_drawIndexThisFrame;
        if (g_transitionedThisFrame)
            ++g_provenance.opaqueAfterTransition;
        return;
    }

    if (!g_sawOpaqueThisFrame || g_transitionedThisFrame)
        return;

    // The boundary: opaque geometry has finished and this draw is the first that
    // is not. Everything in the buffer right now was put there by the model.
    g_transitionedThisFrame = true;
    ++g_provenance.transitions;
    if (GetSource () == Source::AfterOpaque && GetMode () == Mode::PrivateCopy && Capture (context, modelGeneration))
        ++g_provenance.capturedAfterOpaque;
}

void OnScenePassEnd (ID3D11DeviceContext* context, uint64_t modelGeneration)
{
    if (GetSource () != Source::AfterTransparent || GetMode () != Mode::PrivateCopy)
        return;
    if (Capture (context, modelGeneration))
        ++g_provenance.capturedAfterTransparent;
}

ID3D11DepthStencilView* SceneView ()
{
    return g_sceneView;
}

ID3D11DepthStencilView* PrepareForInjection (ID3D11DeviceContext* context)
{
    const Mode mode = GetMode ();
    if (mode == Mode::Off || context == nullptr)
        return nullptr;
    ++g_stats.preparations;
    if (g_sceneView == nullptr) {
        ++g_stats.noSceneView;
        Fail ("no scene depth view has been seen yet");
        return nullptr;
    }
    ++g_stats.sourceViewAcquired;
    if (!EnsureStates (context))
        return nullptr;
    if (mode == Mode::SceneReadOnly) {
        ++g_stats.viewBound;
        return g_sceneView;
    }

    // ---- PrivateCopy -------------------------------------------------------
    if (GetSource () == Source::AtPresent) {
        if (!Capture (context, g_frameGeneration))
            return nullptr;
        ++g_provenance.capturedAtPresent;
    }
    else if (!g_haveCapture) {
        // ⚠️ NOTHING HAS BEEN CAPTURED YET, SO THERE IS NOTHING TO
        // BIND. Refusing is the only honest answer: binding an uninitialised
        // depth texture would reject every ghost pixel and read as "the overlay
        // is broken" when it means "the chosen moment never arrived".
        ++g_provenance.servedStale;
        Fail ("the chosen depth moment has not occurred yet");
        return nullptr;
    }
    if (g_privateView == nullptr)
        return nullptr;
    ++g_stats.viewBound;
    return g_privateView;
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
    g_provenance = Provenance {};
    g_frameOpen = false;
    g_haveCapture = false;
    g_capturedGeneration = 0;
}

Stats GetStats ()
{
    return g_stats;
}

} // namespace depth
} // namespace injection
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
