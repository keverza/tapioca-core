#include "Fixture.hpp"
#include "Reload.hpp"
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include "EngineFactoryD3D11.h"
#include "RenderDevice.h"
#include "DeviceContext.h"
#include "SwapChain.h"
#include "TextureView.h"
#include "RefCntAutoPtr.hpp"
#include "ImGuiImplDiligent.hpp"
#include "ImGuiDiligentRenderer.hpp"
#include <shellapi.h>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler (HWND, UINT, WPARAM, LPARAM);

namespace tapioca::playground {
namespace fs = std::filesystem;
namespace {
LRESULT CALLBACK WindowProc (HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (ImGui::GetCurrentContext () && ImGui_ImplWin32_WndProcHandler (window, message, wparam, lparam))
        return 1;
    if (message == WM_DESTROY) {
        PostQuitMessage (0);
        return 0;
    }
    if (message == WM_DPICHANGED) {
        const auto* bounds = reinterpret_cast<const RECT*> (lparam);
        SetWindowPos (window, nullptr, bounds->left, bounds->top, bounds->right - bounds->left,
                      bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    if (message == WM_SYSCOMMAND && (wparam & 0xFFF0) == SC_KEYMENU)
        return 0;
    return DefWindowProcW (window, message, wparam, lparam);
}
std::string InitialState (const fs::path& folder)
{
    json::JsonArray fixtures;
    std::vector<fs::path> paths;
    for (const auto& entry : fs::directory_iterator (folder))
        if (entry.path ().extension () == ".json")
            paths.push_back (entry.path ());
    std::sort (paths.begin (), paths.end ());
    for (const auto& path : paths) {
        const auto parsed = json::Parse (ReadText (path));
        FixtureModel model;
        std::string error;
        if (!parsed.ok || !DecodeFixture (parsed.value, model, error))
            throw std::runtime_error ("Invalid fixture " + path.filename ().string () + ": " + parsed.error + error);
        fixtures.push_back (parsed.value);
    }
    if (fixtures.empty ())
        throw std::runtime_error ("No JSON fixtures found.");
    return json::Write (Value::Object ({ { "version", Value::Integer (1) },
                                         { "selected", Value::Integer (0) },
                                         { "fixtures", Value::Array (std::move (fixtures)) },
                                         { "events", Value::Array ({}) } }),
                        0);
}
struct Window {
    HWND handle = nullptr;
    HINSTANCE instance = GetModuleHandleW (nullptr);
    Window ()
    {
        ImGui_ImplWin32_EnableDpiAwareness ();
        WNDCLASSEXW wc { sizeof (WNDCLASSEXW) };
        wc.style = CS_CLASSDC;
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW (nullptr, IDC_ARROW);
        wc.lpszClassName = L"TapiocaUIPlayground";
        if (!RegisterClassExW (&wc))
            throw std::runtime_error ("Cannot register playground window.");
        handle = CreateWindowExW (0, wc.lpszClassName, L"Tapioca UI Playground", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                  CW_USEDEFAULT, 1500, 950, nullptr, nullptr, instance, nullptr);
        if (!handle)
            throw std::runtime_error ("Cannot create playground window.");
    }
    ~Window ()
    {
        if (IsWindow (handle))
            DestroyWindow (handle);
        UnregisterClassW (L"TapiocaUIPlayground", instance);
    }
};
struct Graphics {
    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> context;
    Diligent::RefCntAutoPtr<Diligent::ISwapChain> swapChain;
    ImGuiContext* imgui = nullptr;
    std::unique_ptr<Diligent::ImGuiDiligentRenderer> renderer;
    std::vector<char> fontBytes;
    bool platformReady = false;
    ~Graphics ()
    {
        renderer.reset ();
        if (platformReady)
            ImGui_ImplWin32_Shutdown ();
        if (imgui)
            ImGui::DestroyContext (imgui);
    }
    void Init (HWND window)
    {
        auto* factory = Diligent::GetEngineFactoryD3D11 ();
        Diligent::EngineD3D11CreateInfo engine;
        factory->CreateDeviceAndContextsD3D11 (engine, &device, &context);
        if (!device || !context)
            throw std::runtime_error ("Diligent D3D11 device creation failed.");
        RECT client {};
        GetClientRect (window, &client);
        Diligent::SwapChainDesc desc;
        desc.Width = client.right;
        desc.Height = client.bottom;
        desc.ColorBufferFormat = Diligent::TEX_FORMAT_RGBA8_UNORM;
        desc.DepthBufferFormat = Diligent::TEX_FORMAT_UNKNOWN;
        factory->CreateSwapChainD3D11 (device, context, desc, Diligent::FullScreenModeDesc {},
                                       Diligent::Win32NativeWindow { window }, &swapChain);
        if (!swapChain)
            throw std::runtime_error ("Diligent swapchain creation failed.");
        IMGUI_CHECKVERSION ();
        imgui = ImGui::CreateContext ();
        ImGuiIO& io = ImGui::GetIO ();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        const auto bytes = ReadText (TAPIOCA_FONT);
        fontBytes.assign (bytes.begin (), bytes.end ());
        if (fontBytes.empty ())
            throw std::runtime_error ("Missing bundled Noto Sans font; provision reference dependencies.");
        ImFontConfig font;
        font.FontDataOwnedByAtlas = false;
        font.PixelSnapH = true;
        font.OversampleH = font.OversampleV = 1;
        io.Fonts->AddFontFromMemoryTTF (fontBytes.data (), int (fontBytes.size ()), 16, &font);
        platformReady = ImGui_ImplWin32_Init (window);
        if (!platformReady)
            throw std::runtime_error ("ImGui Win32 input initialization failed.");
        Diligent::ImGuiDiligentCreateInfo info;
        info.pDevice = device;
        info.BackBufferFmt = desc.ColorBufferFormat;
        info.DepthBufferFmt = desc.DepthBufferFormat;
        renderer = std::make_unique<Diligent::ImGuiDiligentRenderer> (info);
    }
};
int Run ()
{
    int argc = 0;
    auto** argv = CommandLineToArgvW (GetCommandLineW (), &argc);
    int smokeFrames = 0, smokeReloads = 0;
    bool fresh = false, verification = false;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring (argv[i]) == L"--fresh")
            fresh = true;
        if (std::wstring (argv[i]) == L"--verification")
            verification = true;
        if (std::wstring (argv[i]) == L"--smoke-frames" && i + 1 < argc)
            smokeFrames = _wtoi (argv[++i]);
        if (std::wstring (argv[i]) == L"--smoke-reloads" && i + 1 < argc)
            smokeReloads = _wtoi (argv[++i]);
    }
    LocalFree (argv);
    wchar_t executable[32768] {};
    GetModuleFileNameW (nullptr, executable, DWORD (std::size (executable)));
    const fs::path bin = fs::path (executable).parent_path ();
    const fs::path uiDll = bin / "TapiocaUIPlaygroundUI.dll";
    const fs::path stateFile = bin / "workspace.json";
    std::string state = InitialState (fs::path (TAPIOCA_PLAYGROUND_SOURCE) / "fixtures");
    const auto original = state;
    if (!fresh && fs::exists (stateFile))
        state = ReadText (stateFile);
    Window window;
    Graphics graphics;
    graphics.Init (window.handle);
    Module module;
    std::string error, reloadStatus;
    if (!module.Swap (uiDll, state, error)) {
        state = original;
        if (!module.Swap (uiDll, state, error))
            throw std::runtime_error (error);
        reloadStatus = "Saved workspace rejected; loaded fixtures from disk.\n";
    }
    ReloadLoop reload (TAPIOCA_PLAYGROUND_SOURCE, TAPIOCA_PLAYGROUND_BUILD, TAPIOCA_CMAKE, bin.filename ().wstring ());
    ShowWindow (window.handle, SW_SHOWDEFAULT);
    UpdateWindow (window.handle);
    bool running = true, requestBuild = false;
    int frames = 0;
    std::ofstream diagnostic (bin / "playground-session.log", std::ios::trunc);
    diagnostic << "Session PID: " << GetCurrentProcessId () << '\n' << std::flush;
    std::string lastBuildStatus;
    while (running) {
        MSG message;
        while (PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT)
                running = false;
            TranslateMessage (&message);
            DispatchMessageW (&message);
        }
        if (!running)
            break;
        // Previous frame's draw data has been submitted before any unload occurs.
        const bool rebuilt = reload.Tick (requestBuild);
        requestBuild = false;
        if (lastBuildStatus != reload.Status ()) {
            lastBuildStatus = reload.Status ();
            diagnostic << "Build status: " << lastBuildStatus << '\n' << std::flush;
        }
        if (rebuilt ||
            (smokeReloads > 0 && frames > 0 && frames % 2 == 0 && int (module.Generation ()) <= smokeReloads)) {
            const auto start = std::chrono::steady_clock::now ();
            if (module.Swap (uiDll, state, error)) {
                const auto ms =
                    std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - start).count ();
                reloadStatus = "Active UI generation " + std::to_string (module.Generation ()) + "; swap " +
                               std::to_string (ms) + " ms.\n";
                if (rebuilt)
                    reloadStatus += "Watcher-to-reload " + std::to_string (reload.LastLatencyMs () + ms) + " ms.\n";
                diagnostic << reloadStatus << std::flush;
                if (verification) {
                    const auto folder = bin / "verification" / std::to_string (GetCurrentProcessId ());
                    fs::create_directories (folder);
                    std::ofstream (folder / ("generation-" + std::to_string (module.Generation ()) + ".json")) << state;
                }
            }
            else {
                reloadStatus = error + "\n";
                diagnostic << reloadStatus << std::flush;
            }
        }
        if (IsIconic (window.handle)) {
            WaitMessage ();
            continue;
        }
        RECT bounds {};
        GetClientRect (window.handle, &bounds);
        const auto width = uint32_t (bounds.right), height = uint32_t (bounds.bottom);
        if (!width || !height)
            continue;
        if (graphics.swapChain->GetDesc ().Width != width || graphics.swapChain->GetDesc ().Height != height)
            graphics.swapChain->Resize (width, height);
        graphics.renderer->NewFrame (width, height, Diligent::SURFACE_TRANSFORM_IDENTITY);
        ImGui_ImplWin32_NewFrame ();
        ImGui::NewFrame ();
        ImGui::PushFont (nullptr, 13.0f * float (GetDpiForWindow (window.handle)) / 96);
        const auto status = reloadStatus + reload.Status ();
        const TapiocaUiFrame frame { float (width), float (height), float (GetDpiForWindow (window.handle)) / 96,
                                     status.c_str () };
        TapiocaUiResult result {};
        const bool drawn = module.Draw (frame, result);
        if (result.stateJson) {
            const auto parsed = json::Parse (result.stateJson);
            if (drawn && parsed.ok)
                state = result.stateJson;
            std::free (result.stateJson);
        }
        requestBuild = result.requestBuild != 0;
        ImGui::PopFont ();
        if (!drawn)
            throw std::runtime_error ("UI Draw failed; state snapshot retained. See playground-session.log.");
        ImGui::Render ();
        auto* target = graphics.swapChain->GetCurrentBackBufferRTV ();
        graphics.context->SetRenderTargets (1, &target, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        const float clear[] { 0.12f, 0.12f, 0.12f, 1 };
        graphics.context->ClearRenderTarget (target, clear, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        graphics.renderer->RenderDrawData (graphics.context, ImGui::GetDrawData ());
        graphics.swapChain->Present (1);
        if (frames == 0 && ImGui::GetDrawData ()->TotalVtxCount <= 0)
            throw std::runtime_error ("No UI vertices drawn.");
        ++frames;
        if (verification && frames % 60 == 0)
            diagnostic << "Frames rendered: " << frames << '\n' << std::flush;
        if (smokeFrames > 0 && frames >= smokeFrames)
            running = false;
    }
    std::ofstream (stateFile, std::ios::binary) << state;
    diagnostic << "Frames: " << frames << "; generation: " << module.Generation () << "; clean shutdown\n";
    return smokeReloads > 0 && module.Generation () < uint64_t (smokeReloads + 1) ? 2 : 0;
}
} // namespace
} // namespace tapioca::playground
int WINAPI wWinMain (HINSTANCE, HINSTANCE, PWSTR, int)
{
    try {
        return tapioca::playground::Run ();
    }
    catch (const std::exception& error) {
        MessageBoxA (nullptr, error.what (), "Tapioca UI Playground", MB_OK | MB_ICONERROR);
        return 1;
    }
}
