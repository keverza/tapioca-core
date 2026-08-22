#include "ArchViz/D3D12FeasibilityProbe.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/DiligentViewportSupport.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dcomp.h>
#include <dxgi1_4.h>

#include <CommandQueueD3D12.h>
#include <DeviceContextD3D12.h>
#include <EngineFactoryD3D12.h>
#include <RefCntAutoPtr.hpp>
#include <RenderDeviceD3D12.h>
#include <SwapChainD3D12.h>
#include <Texture.h>
#include <TextureView.h>

#include <chrono>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <vector>

namespace geomsrv::archviz {
namespace {

using Diligent::RefCntAutoPtr;

constexpr auto kStepDelay = std::chrono::milliseconds (500);

std::string HrText (HRESULT hr)
{
    char text[32] = {};
    std::snprintf (text, sizeof (text), "0x%08lX", static_cast<unsigned long> (hr));
    return text;
}

template <class T> void ReleaseCom (T*& object)
{
    if (object != nullptr) {
        object->Release ();
        object = nullptr;
    }
}

std::string Narrow (const wchar_t* text)
{
    if (text == nullptr || *text == L'\0')
        return {};
    const int size = ::WideCharToMultiByte (CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return {};
    std::vector<char> utf8 (static_cast<size_t> (size));
    ::WideCharToMultiByte (CP_UTF8, 0, text, -1, utf8.data (), size, nullptr, nullptr);
    return std::string (utf8.data ());
}

struct HardwarePreflight {
    bool succeeded = false;
    HRESULT result = E_FAIL;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    std::string adapter;
    std::string runtime;
};

HardwarePreflight ProbeHardwareDevice ()
{
    HardwarePreflight result;
    HMODULE d3d12 = ::LoadLibraryExW (L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (d3d12 == nullptr) {
        result.result = HRESULT_FROM_WIN32 (::GetLastError ());
        return result;
    }

    wchar_t runtimePath[MAX_PATH] = {};
    ::GetModuleFileNameW (d3d12, runtimePath, MAX_PATH);
    result.runtime = Narrow (runtimePath);
    const auto createDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE> (::GetProcAddress (d3d12, "D3D12CreateDevice"));
    if (createDevice == nullptr) {
        result.result = HRESULT_FROM_WIN32 (::GetLastError ());
        ::FreeLibrary (d3d12);
        return result;
    }

    IDXGIFactory4* factory = nullptr;
    HRESULT hr = ::CreateDXGIFactory1 (__uuidof (IDXGIFactory4), reinterpret_cast<void**> (&factory));
    IDXGIAdapter1* hardware = nullptr;
    if (SUCCEEDED (hr)) {
        for (UINT index = 0; factory->EnumAdapters1 (index, &hardware) != DXGI_ERROR_NOT_FOUND; ++index) {
            DXGI_ADAPTER_DESC1 desc = {};
            hardware->GetDesc1 (&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                SUCCEEDED (createDevice (hardware, D3D_FEATURE_LEVEL_11_0, __uuidof (ID3D12Device), nullptr))) {
                result.adapter = Narrow (desc.Description);
                break;
            }
            ReleaseCom (hardware);
        }
    }

    if (hardware == nullptr) {
        result.result = FAILED (hr) ? hr : DXGI_ERROR_UNSUPPORTED;
    }
    else {
        constexpr D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                                 D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        for (const D3D_FEATURE_LEVEL level : levels) {
            ID3D12Device* device = nullptr;
            hr = createDevice (hardware, level, __uuidof (ID3D12Device), reinterpret_cast<void**> (&device));
            ArchVizLog ("[RE51.D1] hardware D3D12CreateDevice adapter='" + result.adapter + "' level=0x" + [&] {
                char text[16] = {};
                std::snprintf (text, sizeof (text), "%04X", uint32_t (level));
                return std::string (text);
            }() + " result=" + HrText (hr));
            ReleaseCom (device);
            result.result = hr;
            result.featureLevel = level;
            if (SUCCEEDED (hr)) {
                result.succeeded = true;
                break;
            }
        }
    }

    ReleaseCom (hardware);
    ReleaseCom (factory);
    ::FreeLibrary (d3d12);
    return result;
}

} // namespace

D3D12FeasibilityProbe& D3D12FeasibilityProbe::Get ()
{
    static D3D12FeasibilityProbe probe;
    return probe;
}

D3D12FeasibilityProbe::~D3D12FeasibilityProbe ()
{
    Stop ();
}

bool D3D12FeasibilityProbe::Start (void* childHwnd, uint32_t childWidth, uint32_t childHeight, void* overlayHwnd,
                                   uint32_t overlayWidth, uint32_t overlayHeight,
                                   const std::string& overlayPreflightError, std::string& error)
{
    if (childHwnd == nullptr || ::IsWindow (static_cast<HWND> (childHwnd)) == FALSE || childWidth == 0 ||
        childHeight == 0) {
        error = "the DG child HWND or its client size is invalid; D3D12 was not called";
        SetRefusal (error);
        return false;
    }
    if (attempted_.exchange (true)) {
        error = "RE51.D1 was already attempted in this process; restart Archicad before retrying";
        SetRefusal (error);
        return false;
    }

    if (worker_.joinable ())
        worker_.join ();
    stopRequested_.store (false);
    {
        std::lock_guard<std::mutex> lock (mutex_);
        stats_ = {};
        stats_.attempted = true;
        stats_.running = true;
        stats_.stage = "starting";
    }
    try {
        worker_ = std::thread (&D3D12FeasibilityProbe::Run, this, childHwnd, childWidth, childHeight, overlayHwnd,
                               overlayWidth, overlayHeight, overlayPreflightError);
    }
    catch (const std::exception& ex) {
        error = std::string ("could not start the D3D12 probe worker: ") + ex.what ();
        SetRefusal (error);
        return false;
    }
    return true;
}

void D3D12FeasibilityProbe::Stop ()
{
    stopRequested_.store (true);
    if (worker_.joinable ())
        worker_.join ();
    std::lock_guard<std::mutex> lock (mutex_);
    stats_.running = false;
}

D3D12FeasibilityProbeStats D3D12FeasibilityProbe::Stats () const
{
    std::lock_guard<std::mutex> lock (mutex_);
    return stats_;
}

void D3D12FeasibilityProbe::SetRefusal (const std::string& error)
{
    {
        std::lock_guard<std::mutex> lock (mutex_);
        // A duplicate request must not make the original worker look stopped or
        // erase the retained result of the one process-wide attempt.
        if (!stats_.attempted) {
            stats_.running = false;
            stats_.stage = "refused";
            stats_.error = error;
        }
    }
    ArchVizLog ("[RE51.D1] REFUSED: " + error);
}

void D3D12FeasibilityProbe::Run (void* childHwnd, uint32_t childWidth, uint32_t childHeight, void* overlayHwnd,
                                 uint32_t overlayWidth, uint32_t overlayHeight, std::string overlayPreflightError)
{
    RefCntAutoPtr<Diligent::IRenderDevice> device;
    RefCntAutoPtr<Diligent::IDeviceContext> context;
    RefCntAutoPtr<Diligent::ISwapChain> childSwapChain;
    RefCntAutoPtr<Diligent::IRenderDeviceD3D12> deviceD3D12;
    RefCntAutoPtr<Diligent::IDeviceContextD3D12> contextD3D12;

    IDXGIFactory2* dxgiFactory = nullptr;
    IDXGISwapChain1* compositionSwapChain = nullptr;
    IDXGISwapChain3* compositionSwapChain3 = nullptr;
    IDCompositionDevice* compositionDevice = nullptr;
    IDCompositionTarget* compositionTarget = nullptr;
    IDCompositionVisual* compositionVisual = nullptr;

    auto setStage = [this] (const char* stage) {
        std::lock_guard<std::mutex> lock (mutex_);
        stats_.stage = stage;
        ArchVizLog ("[RE51.D1] stage: " + std::string (stage));
    };
    auto finish = [this] (bool clean) {
        std::lock_guard<std::mutex> lock (mutex_);
        stats_.cleanTeardown = clean;
        stats_.cancelled = stopRequested_.load ();
        stats_.completed = true;
        stats_.running = false;
        stats_.stage = clean ? "complete" : "teardown failed";
    };

    bool cleanTeardown = false;
    try {
        InstallDiligentDebugCallback ();
        setStage ("device");
        {
            std::lock_guard<std::mutex> lock (mutex_);
            stats_.deviceAttempted = true;
        }
        ArchVizLog ("[RE51.D1] D3D12 device creation begin");
        const HardwarePreflight preflight = ProbeHardwareDevice ();
        {
            std::lock_guard<std::mutex> lock (mutex_);
            stats_.hardwarePreflightSucceeded = preflight.succeeded;
            stats_.hardwareCreateResult = uint32_t (preflight.result);
            stats_.hardwareFeatureLevel = uint32_t (preflight.featureLevel);
            stats_.d3d12Runtime = preflight.runtime;
            stats_.adapter = preflight.adapter;
        }
        ArchVizLog ("[RE51.D1] hardware preflight " + std::string (preflight.succeeded ? "PASS" : "FAIL") +
                    ": adapter='" + preflight.adapter + "' runtime='" + preflight.runtime + "' result=" +
                    HrText (preflight.result));
        if (!preflight.succeeded)
            throw std::runtime_error ("hardware D3D12CreateDevice failed " + HrText (preflight.result) +
                                      "; WARP presentation was not attempted");

        Diligent::EngineD3D12CreateInfo createInfo;
        createInfo.Features.RayTracing = Diligent::DEVICE_FEATURE_STATE_OPTIONAL;
        Diligent::IEngineFactoryD3D12* factory = Diligent::GetEngineFactoryD3D12 ();
        if (factory == nullptr)
            throw std::runtime_error ("GetEngineFactoryD3D12 returned null");
        factory->CreateDeviceAndContextsD3D12 (createInfo, &device, &context);
        if (device == nullptr || context == nullptr)
            throw std::runtime_error ("CreateDeviceAndContextsD3D12 returned an incomplete device/context pair");

        deviceD3D12 = RefCntAutoPtr<Diligent::IRenderDeviceD3D12> (device, Diligent::IID_RenderDeviceD3D12);
        contextD3D12 = RefCntAutoPtr<Diligent::IDeviceContextD3D12> (context, Diligent::IID_DeviceContextD3D12);
        if (deviceD3D12 == nullptr || contextD3D12 == nullptr)
            throw std::runtime_error ("the created objects do not expose the D3D12 interop interfaces");

        const auto& deviceInfo = device->GetDeviceInfo ();
        const auto& adapterInfo = device->GetAdapterInfo ();
        const auto& rt = adapterInfo.RayTracing;
        if (adapterInfo.Type == Diligent::ADAPTER_TYPE_SOFTWARE)
            throw std::runtime_error ("Diligent returned a software D3D12 adapter; WARP is not a D1 success");
        {
            std::lock_guard<std::mutex> lock (mutex_);
            stats_.deviceSucceeded = true;
            stats_.adapter = adapterInfo.Description;
            stats_.rayTracingFeature = uint32_t (deviceInfo.Features.RayTracing);
            stats_.rayTracingCaps = uint32_t (rt.CapFlags);
            stats_.rayTracingStandalone = (rt.CapFlags & Diligent::RAY_TRACING_CAP_FLAG_STANDALONE_SHADERS) != 0;
            stats_.rayTracingInline = (rt.CapFlags & Diligent::RAY_TRACING_CAP_FLAG_INLINE_RAY_TRACING) != 0;
            stats_.rayTracingIndirect = (rt.CapFlags & Diligent::RAY_TRACING_CAP_FLAG_INDIRECT_RAY_TRACING) != 0;
            stats_.maxRecursionDepth = rt.MaxRecursionDepth;
            stats_.maxRayGenThreads = rt.MaxRayGenThreads;
        }
        ArchVizLog ("[RE51.D1] DEVICE PASS: adapter='" + std::string (adapterInfo.Description) +
                    "' RT feature=" + std::to_string (uint32_t (deviceInfo.Features.RayTracing)) + " caps=0x" + [&rt] {
                        char text[16] = {};
                        std::snprintf (text, sizeof (text), "%02X", uint32_t (rt.CapFlags));
                        return std::string (text);
                    }());

        if (!stopRequested_.load ()) {
            setStage ("child HWND");
            {
                std::lock_guard<std::mutex> lock (mutex_);
                stats_.childAttempted = true;
            }
            try {
                Diligent::SwapChainDesc desc;
                desc.Width = childWidth;
                desc.Height = childHeight;
                const Diligent::Win32NativeWindow window { static_cast<HWND> (childHwnd) };
                factory->CreateSwapChainD3D12 (device, context, desc, Diligent::FullScreenModeDesc {}, window,
                                               &childSwapChain);
                if (childSwapChain == nullptr)
                    throw std::runtime_error ("CreateSwapChainD3D12 returned no swap chain");

                RefCntAutoPtr<Diligent::ISwapChainD3D12> nativeSwapChain { childSwapChain,
                                                                           Diligent::IID_SwapChainD3D12 };
                for (int frame = 0; frame < 8 && !stopRequested_.load (); ++frame) {
                    Diligent::ITextureView* rtv = childSwapChain->GetCurrentBackBufferRTV ();
                    if (rtv == nullptr)
                        throw std::runtime_error ("the D3D12 child swap chain returned no back-buffer RTV");
                    const float green[4] = { 0.05f, 0.85f, 0.15f, 1.0f };
                    const float magenta[4] = { 0.85f, 0.05f, 0.65f, 1.0f };
                    context->SetRenderTargets (1, &rtv, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                    context->ClearRenderTarget (rtv, (frame & 1) != 0 ? green : magenta,
                                                Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                    childSwapChain->Present (1);
                    context->FinishFrame ();
                    HRESULT presentCheck = E_NOINTERFACE;
                    UINT presentCount = 0;
                    if (nativeSwapChain != nullptr && nativeSwapChain->GetDXGISwapChain () != nullptr &&
                        deviceD3D12->GetD3D12Device () != nullptr) {
                        const HRESULT countResult = nativeSwapChain->GetDXGISwapChain ()->GetLastPresentCount (&presentCount);
                        presentCheck = FAILED (countResult) ? countResult
                                                            : deviceD3D12->GetD3D12Device ()->GetDeviceRemovedReason ();
                    }
                    {
                        std::lock_guard<std::mutex> lock (mutex_);
                        stats_.childPresents = presentCount;
                        stats_.childLastPresentResult = uint32_t (presentCheck);
                    }
                    std::this_thread::sleep_for (kStepDelay);
                }
                {
                    std::lock_guard<std::mutex> lock (mutex_);
                    stats_.childSucceeded =
                        stats_.childPresents > 0 && SUCCEEDED (HRESULT (stats_.childLastPresentResult));
                    if (!stats_.childSucceeded)
                        stats_.childError = "DXGI did not retain present progress on a healthy D3D12 device (" +
                                            HrText (HRESULT (stats_.childLastPresentResult)) + ")";
                }
                const D3D12FeasibilityProbeStats childStats = Stats ();
                ArchVizLog ("[RE51.D1] CHILD " + std::string (childStats.childSucceeded ? "API PASS" : "FAIL") +
                            ": presents=" + std::to_string (childStats.childPresents) +
                            " device/check=" + HrText (HRESULT (childStats.childLastPresentResult)));
            }
            catch (const std::exception& ex) {
                std::lock_guard<std::mutex> lock (mutex_);
                stats_.childError = ex.what ();
                ArchVizLog ("[RE51.D1] CHILD FAIL: " + stats_.childError);
            }
            context->SetRenderTargets (0, nullptr, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE);
            context->Flush ();
            childSwapChain.Release ();
        }

        if (!stopRequested_.load ()) {
            setStage ("overlay setup");
            {
                std::lock_guard<std::mutex> lock (mutex_);
                stats_.overlayAttempted = true;
            }
            try {
                if (overlayHwnd == nullptr || ::IsWindow (static_cast<HWND> (overlayHwnd)) == FALSE ||
                    overlayWidth == 0 || overlayHeight == 0)
                    throw std::runtime_error (overlayPreflightError.empty ()
                                                  ? "the overlay HWND or its client size is invalid"
                                                  : overlayPreflightError);

                HRESULT hr =
                    ::CreateDXGIFactory2 (0, __uuidof (IDXGIFactory2), reinterpret_cast<void**> (&dxgiFactory));
                if (FAILED (hr) || dxgiFactory == nullptr)
                    throw std::runtime_error ("CreateDXGIFactory2 failed " + HrText (hr));

                DXGI_SWAP_CHAIN_DESC1 desc = {};
                desc.Width = overlayWidth;
                desc.Height = overlayHeight;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                desc.BufferCount = 2;
                desc.Scaling = DXGI_SCALING_STRETCH;
                desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
                desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

                Diligent::ICommandQueue* queueBase = context->LockCommandQueue ();
                RefCntAutoPtr<Diligent::ICommandQueueD3D12> queue { queueBase, Diligent::IID_CommandQueueD3D12 };
                ID3D12CommandQueue* nativeQueue = queue != nullptr ? queue->GetD3D12CommandQueue () : nullptr;
                if (nativeQueue == nullptr) {
                    context->UnlockCommandQueue ();
                    throw std::runtime_error ("the immediate context exposed no D3D12 command queue");
                }
                hr = dxgiFactory->CreateSwapChainForComposition (nativeQueue, &desc, nullptr, &compositionSwapChain);
                context->UnlockCommandQueue ();
                if (FAILED (hr) || compositionSwapChain == nullptr)
                    throw std::runtime_error ("CreateSwapChainForComposition failed " + HrText (hr));
                ArchVizLog ("[RE51.D1] OVERLAY: CreateSwapChainForComposition PASS");

                hr = ::DCompositionCreateDevice (nullptr, __uuidof (IDCompositionDevice),
                                                 reinterpret_cast<void**> (&compositionDevice));
                if (SUCCEEDED (hr))
                    hr = compositionDevice->CreateTargetForHwnd (static_cast<HWND> (overlayHwnd), TRUE,
                                                                 &compositionTarget);
                if (SUCCEEDED (hr))
                    hr = compositionDevice->CreateVisual (&compositionVisual);
                if (SUCCEEDED (hr))
                    hr = compositionVisual->SetContent (compositionSwapChain);
                if (SUCCEEDED (hr))
                    hr = compositionTarget->SetRoot (compositionVisual);
                if (SUCCEEDED (hr))
                    hr = compositionDevice->Commit ();
                if (FAILED (hr))
                    throw std::runtime_error ("building the DirectComposition visual tree failed " + HrText (hr));
                ArchVizLog ("[RE51.D1] OVERLAY: DirectComposition Commit PASS");

                hr = compositionSwapChain->QueryInterface (__uuidof (IDXGISwapChain3),
                                                           reinterpret_cast<void**> (&compositionSwapChain3));
                if (FAILED (hr) || compositionSwapChain3 == nullptr)
                    throw std::runtime_error ("IDXGISwapChain3 unavailable " + HrText (hr));

                const float transparent[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                const float halfOrange[4] = { 0.5f, 0.25f, 0.0f, 0.5f };
                const float opaqueOrange[4] = { 1.0f, 0.5f, 0.0f, 1.0f };
                for (int frame = 0; frame < 16 && !stopRequested_.load (); ++frame) {
                    setStage (frame < 4 ? "overlay transparent" : frame < 12 ? "overlay half alpha" : "overlay opaque");
                    ID3D12Resource* backBuffer = nullptr;
                    const UINT index = compositionSwapChain3->GetCurrentBackBufferIndex ();
                    hr = compositionSwapChain->GetBuffer (index, __uuidof (ID3D12Resource),
                                                          reinterpret_cast<void**> (&backBuffer));
                    if (FAILED (hr) || backBuffer == nullptr)
                        throw std::runtime_error ("composition GetBuffer failed " + HrText (hr));

                    RefCntAutoPtr<Diligent::ITexture> texture;
                    deviceD3D12->CreateTextureFromD3DResource (backBuffer, Diligent::RESOURCE_STATE_PRESENT, &texture);
                    backBuffer->Release ();
                    if (texture == nullptr)
                        throw std::runtime_error ("CreateTextureFromD3DResource returned no texture");
                    Diligent::ITextureView* rtv = texture->GetDefaultView (Diligent::TEXTURE_VIEW_RENDER_TARGET);
                    if (rtv == nullptr)
                        throw std::runtime_error ("the wrapped composition buffer has no RTV");
                    const float* color = frame < 4 ? transparent : frame < 12 ? halfOrange : opaqueOrange;
                    context->SetRenderTargets (1, &rtv, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                    context->ClearRenderTarget (rtv, color, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                    contextD3D12->TransitionTextureState (texture, D3D12_RESOURCE_STATE_PRESENT);
                    context->Flush ();
                    hr = compositionSwapChain->Present (1, 0);
                    context->FinishFrame ();
                    {
                        std::lock_guard<std::mutex> lock (mutex_);
                        ++stats_.overlayPresents;
                        stats_.overlayLastPresentResult = uint32_t (hr);
                    }
                    if (FAILED (hr))
                        throw std::runtime_error ("composition Present failed " + HrText (hr));
                    std::this_thread::sleep_for (kStepDelay);
                }
                {
                    std::lock_guard<std::mutex> lock (mutex_);
                    stats_.overlaySucceeded =
                        stats_.overlayPresents > 0 && SUCCEEDED (HRESULT (stats_.overlayLastPresentResult));
                }
                const D3D12FeasibilityProbeStats overlayStats = Stats ();
                ArchVizLog ("[RE51.D1] OVERLAY API PASS: presents=" + std::to_string (overlayStats.overlayPresents) +
                            " last=" + HrText (HRESULT (overlayStats.overlayLastPresentResult)));
            }
            catch (const std::exception& ex) {
                std::lock_guard<std::mutex> lock (mutex_);
                stats_.overlayError = ex.what ();
                ArchVizLog ("[RE51.D1] OVERLAY FAIL: " + stats_.overlayError);
            }
        }
    }
    catch (const std::exception& ex) {
        const std::string message = ex.what ();
        std::lock_guard<std::mutex> lock (mutex_);
        stats_.error = message;
        if (!stats_.deviceSucceeded)
            stats_.deviceError = message;
        else if (stats_.childAttempted && !stats_.childSucceeded && stats_.childError.empty ())
            stats_.childError = message;
        else if (stats_.overlayAttempted && !stats_.overlaySucceeded && stats_.overlayError.empty ())
            stats_.overlayError = message;
        ArchVizLog ("[RE51.D1] FAIL at '" + stats_.stage + "': " + message);
    }
    catch (...) {
        std::lock_guard<std::mutex> lock (mutex_);
        stats_.error = "unknown exception";
        ArchVizLog ("[RE51.D1] FAIL at '" + stats_.stage + "': unknown exception");
    }

    setStage ("teardown");
    if (context != nullptr) {
        context->SetRenderTargets (0, nullptr, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE);
        context->Flush ();
    }
    cleanTeardown = true;
    if (compositionDevice != nullptr && compositionTarget != nullptr) {
        HRESULT hr = compositionTarget->SetRoot (nullptr);
        if (SUCCEEDED (hr))
            hr = compositionDevice->Commit ();
        if (SUCCEEDED (hr))
            hr = compositionDevice->WaitForCommitCompletion ();
        if (FAILED (hr)) {
            cleanTeardown = false;
            std::lock_guard<std::mutex> lock (mutex_);
            stats_.error = "DirectComposition teardown failed " + HrText (hr);
        }
    }
    ReleaseCom (compositionVisual);
    ReleaseCom (compositionTarget);
    ReleaseCom (compositionDevice);
    ReleaseCom (compositionSwapChain3);
    ReleaseCom (compositionSwapChain);
    ReleaseCom (dxgiFactory);
    childSwapChain.Release ();
    contextD3D12.Release ();
    deviceD3D12.Release ();
    context.Release ();
    device.Release ();
    finish (cleanTeardown);
    ArchVizLog (std::string ("[RE51.D1] teardown ") + (cleanTeardown ? "PASS" : "FAIL"));
}

} // namespace geomsrv::archviz
