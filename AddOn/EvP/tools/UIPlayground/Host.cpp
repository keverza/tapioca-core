#include "Fixture.hpp"
#include "Reload.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_win32.h>
#include "EngineFactoryD3D11.h"
#include "RenderDevice.h"
#include "DeviceContext.h"
#include "SwapChain.h"
#include "TextureView.h"
#include "RefCntAutoPtr.hpp"
#include "Texture.h"
#include "DataBlobImpl.hpp"
#include "PNGCodec.h"
#include "ImGuiImplDiligent.hpp"
#include "ImGuiDiligentRenderer.hpp"
#include <shellapi.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler (HWND, UINT, WPARAM, LPARAM);
// ImGuiFiles.cpp, in the shared ImGui: every section open while on, closed again when turned off.
void PlaygroundExpandAll (bool on);
size_t PlaygroundExpandedCount ();
void PlaygroundSetCaptureHeight (float height);

namespace tapioca::playground {
namespace fs = std::filesystem;
namespace {
// F12 asks for a screenshot of the next frame drawn (Screenshot below).
bool g_screenshotRequested = false;

LRESULT CALLBACK WindowProc (HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_KEYDOWN && wparam == VK_F12 && !(lparam & (1 << 30)))
        g_screenshotRequested = true;
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
// A fixture file as the workspace holds it: a virtual editor dump is wrapped, its file name the
// variant when it names none ("editor-boolean-refused.json" -> "boolean-refused").
Value FixtureOf (const fs::path& path)
{
    const auto parsed = json::Parse (ReadText (path));
    if (!parsed.ok)
        throw std::runtime_error ("Invalid fixture " + path.filename ().string () + ": " + parsed.error);
    if (!IsEditorDump (parsed.value))
        return parsed.value;
    auto stem = path.stem ().string ();
    if (stem.rfind ("editor-", 0) == 0)
        stem = stem.substr (7);
    return FixtureFromDump (parsed.value, stem);
}
// `dumps`: debug dumps named on the command line (--dump), after the fixtures; the first is opened.
std::string InitialState (const fs::path& folder, const std::vector<fs::path>& dumps, const std::string& select)
{
    json::JsonArray fixtures;
    std::vector<fs::path> paths;
    for (const auto& entry : fs::directory_iterator (folder))
        if (entry.path ().extension () == ".json")
            paths.push_back (entry.path ());
    std::sort (paths.begin (), paths.end ());
    const size_t own = paths.size ();
    paths.insert (paths.end (), dumps.begin (), dumps.end ());
    for (const auto& path : paths) {
        const auto fixture = FixtureOf (path);
        FixtureModel model;
        std::string error;
        if (!DecodeFixture (fixture, model, error))
            throw std::runtime_error ("Invalid fixture " + path.filename ().string () + ": " + error);
        fixtures.push_back (fixture);
    }
    if (fixtures.empty ())
        throw std::runtime_error ("No JSON fixtures found.");
    int64_t selected = dumps.empty () ? 0 : int64_t (own);
    bool editor = !dumps.empty ();
    if (!select.empty ()) {
        selected = -1;
        for (size_t i = 0; i < fixtures.size (); ++i)
            if (Text (fixtures[i], "variant") == select || paths[i].stem ().string () == select ||
                paths[i].stem ().string () == "editor-" + select) {
                selected = int64_t (i);
                editor = Text (fixtures[i], "panel") == "hudeditor";
            }
        if (selected < 0)
            throw std::runtime_error ("--select: no fixture named " + select);
    }
    return json::Write (Value::Object ({ { "version", Value::Integer (1) },
                                         { "selected", Value::Integer (selected) },
                                         { "nativeRequest", Value::String (editor ? "@editor" : "") },
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
// %LOCALAPPDATA%\Tapioca\Export, where the add-on's viewer screenshots go too: a screenshot is
// something to keep and send, not a build or session file.
fs::path ExportFolder ()
{
    const wchar_t* root = _wgetenv (L"LOCALAPPDATA");
    if (!root || !*root)
        throw std::runtime_error ("LOCALAPPDATA is unavailable");
    const auto folder = fs::path (root) / L"Tapioca" / L"Export";
    fs::create_directories (folder);
    return folder;
}

// What Archicad shows of the frame: the native panel and its dock tab, with a small margin; the whole
// frame when neither is drawn (a gallery or another non-native view).
struct Crop {
    uint32_t x = 0, y = 0, width = 0, height = 0;
};
Crop PanelCrop (uint32_t width, uint32_t height)
{
    ImRect area;
    bool any = false;
    for (const char* name : { "###playground.native.hud", "###tapioca.dock" })
        if (const ImGuiWindow* window = ImGui::FindWindowByName (name); window && window->Active) {
            const ImRect rect (window->Pos, window->Pos + window->Size);
            area = any ? ImRect (ImMin (area.Min, rect.Min), ImMax (area.Max, rect.Max)) : rect;
            any = true;
        }
    if (!any)
        return { 0, 0, width, height };
    area.Expand (8.0f);
    const auto x0 = uint32_t (std::clamp (area.Min.x, 0.0f, float (width)));
    const auto y0 = uint32_t (std::clamp (area.Min.y, 0.0f, float (height)));
    const auto x1 = uint32_t (std::clamp (area.Max.x, 0.0f, float (width)));
    const auto y1 = uint32_t (std::clamp (area.Max.y, 0.0f, float (height)));
    if (x1 <= x0 || y1 <= y0)
        return { 0, 0, width, height };
    return { x0, y0, x1 - x0, y1 - y0 };
}

// The frame just drawn into the back buffer -- cropped to the panel Archicad shows (PanelCrop) --
// as "playground-20261115-131500-<n>.png" in the export folder: copied to a staging texture, read
// back, opaque, PNG-encoded. Returns the file written.
fs::path Screenshot (Graphics& graphics, Diligent::ITexture* backBuffer, uint32_t width, uint32_t height)
{
    Diligent::TextureDesc desc;
    desc.Name = "Playground screenshot";
    desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
    desc.Width = width;
    desc.Height = height;
    desc.Format = backBuffer->GetDesc ().Format;
    desc.Usage = Diligent::USAGE_STAGING;
    desc.CPUAccessFlags = Diligent::CPU_ACCESS_READ;
    Diligent::RefCntAutoPtr<Diligent::ITexture> staging;
    graphics.device->CreateTexture (desc, nullptr, &staging);
    if (!staging)
        throw std::runtime_error ("Screenshot: the staging texture could not be created.");
    Diligent::CopyTextureAttribs copy;
    copy.pSrcTexture = backBuffer;
    copy.SrcTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    copy.pDstTexture = staging;
    copy.DstTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    graphics.context->CopyTexture (copy);
    graphics.context->WaitForIdle ();
    Diligent::MappedTextureSubresource mapped;
    graphics.context->MapTextureSubresource (staging, 0, 0, Diligent::MAP_READ, Diligent::MAP_FLAG_NONE, nullptr,
                                             mapped);
    if (!mapped.pData)
        throw std::runtime_error ("Screenshot: the staging texture could not be mapped.");
    const Crop crop = PanelCrop (width, height);
    const size_t row = size_t (crop.width) * 4;
    std::vector<uint8_t> rgba (row * crop.height);
    for (uint32_t y = 0; y < crop.height; ++y)
        std::memcpy (rgba.data () + row * y,
                     static_cast<const uint8_t*> (mapped.pData) + size_t (mapped.Stride) * (crop.y + y) +
                         size_t (crop.x) * 4,
                     row);
    graphics.context->UnmapTextureSubresource (staging, 0, 0);
    if (desc.Format == Diligent::TEX_FORMAT_BGRA8_UNORM || desc.Format == Diligent::TEX_FORMAT_BGRA8_UNORM_SRGB)
        for (size_t i = 0; i < rgba.size (); i += 4)
            std::swap (rgba[i], rgba[i + 2]);
    for (size_t i = 3; i < rgba.size (); i += 4)
        rgba[i] = 255; // the window is opaque; the cleared alpha is not the picture's
    Diligent::RefCntAutoPtr<Diligent::IDataBlob> blob = Diligent::DataBlobImpl::Create ();
    constexpr int kPngColorTypeRgba = 6; // PNG's numeric colour type for RGBA (as DiligentViewportTarget)
    if (Diligent::EncodePng (rgba.data (), crop.width, crop.height, uint32_t (row), kPngColorTypeRgba, blob) !=
            Diligent::ENCODE_PNG_RESULT_OK ||
        !blob || !blob->GetSize ())
        throw std::runtime_error ("Screenshot: PNG encoding failed.");
    const std::time_t now = std::chrono::system_clock::to_time_t (std::chrono::system_clock::now ());
    std::tm local {};
    localtime_s (&local, &now);
    char stamp[32] {};
    std::strftime (stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local);
    const auto folder = ExportFolder ();
    fs::path file;
    for (int n = 1; file.empty () || fs::exists (file); ++n)
        file = folder / ("playground-" + std::string (stamp) + "-" + std::to_string (n) + ".png");
    std::ofstream out (file, std::ios::binary);
    out.write (static_cast<const char*> (blob->GetConstDataPtr ()), std::streamsize (blob->GetSize ()));
    if (!out)
        throw std::runtime_error ("Screenshot: could not write " + file.string ());
    return file;
}

// The panel whole: the live session drawn with the preview at its tallest (PlaygroundSetCaptureHeight)
// into a tall offscreen target, with every section open (PlaygroundExpandAll) until no frame opens
// another -- a section opened shows the ones inside it on the next -- and the panel has laid itself
// out. Then each section is closed again and the height put back. Nothing is swapped, so what the
// session holds and its state file does not -- a close prompt, a running command -- is captured too.
// What these frames return as state is dropped. Returns the file written.
fs::path FullScreenshot (Graphics& graphics, Module& module, uint32_t width, float dpi)
{
    constexpr float kTallest = 2400.0f; // Workspace's preview height limit, logical px
    PlaygroundSetCaptureHeight (kTallest);
    const auto height = uint32_t (kTallest * dpi + 400.0f * dpi);
    Diligent::TextureDesc desc;
    desc.Name = "Playground full screenshot";
    desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
    desc.Width = width;
    desc.Height = height;
    desc.Format = graphics.swapChain->GetDesc ().ColorBufferFormat;
    desc.BindFlags = Diligent::BIND_RENDER_TARGET;
    Diligent::RefCntAutoPtr<Diligent::ITexture> target;
    graphics.device->CreateTexture (desc, nullptr, &target);
    std::string failure;
    fs::path file;
    if (!target)
        failure = "Screenshot: the offscreen target could not be created.";
    PlaygroundExpandAll (true);
    size_t opened = SIZE_MAX;
    for (int frame = 0, settled = 0; frame < 24 && settled < 3 && failure.empty (); ++frame) {
        settled = PlaygroundExpandedCount () == opened ? settled + 1 : 0;
        opened = PlaygroundExpandedCount ();
        graphics.renderer->NewFrame (width, height, Diligent::SURFACE_TRANSFORM_IDENTITY);
        ImGui_ImplWin32_NewFrame ();
        ImGui::GetIO ().DisplaySize = ImVec2 (float (width), float (height));
        ImGui::NewFrame ();
        ImGui::PushFont (nullptr, 13.0f * dpi);
        const TapiocaUiFrame tall { float (width), float (height), dpi, "" };
        TapiocaUiResult result {};
        const bool drawn = module.Draw (tall, result);
        if (result.stateJson)
            std::free (result.stateJson);
        ImGui::PopFont ();
        ImGui::Render ();
        if (!drawn) {
            failure = "Screenshot: the UI did not draw.";
            break;
        }
        auto* rtv = target->GetDefaultView (Diligent::TEXTURE_VIEW_RENDER_TARGET);
        graphics.context->SetRenderTargets (1, &rtv, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        const float clear[] { 0.12f, 0.12f, 0.12f, 1 };
        graphics.context->ClearRenderTarget (rtv, clear, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        graphics.renderer->RenderDrawData (graphics.context, ImGui::GetDrawData ());
    }
    if (failure.empty ()) {
        try {
            file = Screenshot (graphics, target, width, height);
        }
        catch (const std::exception& e) {
            failure = e.what ();
        }
    }
    PlaygroundExpandAll (false);
    PlaygroundSetCaptureHeight (0.0f);
    if (!failure.empty ())
        throw std::runtime_error (failure);
    return file;
}

int Run ()
{
    int argc = 0;
    auto** argv = CommandLineToArgvW (GetCommandLineW (), &argc);
    int smokeFrames = 0, smokeReloads = 0, screenshotFrame = 0;
    std::string select;
    bool fresh = false, verification = false;
    std::vector<fs::path> dumps;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring (argv[i]) == L"--fresh")
            fresh = true;
        if (std::wstring (argv[i]) == L"--verification")
            verification = true;
        if (std::wstring (argv[i]) == L"--smoke-frames" && i + 1 < argc)
            smokeFrames = _wtoi (argv[++i]);
        if (std::wstring (argv[i]) == L"--smoke-reloads" && i + 1 < argc)
            smokeReloads = _wtoi (argv[++i]);
        // A debug dump to open in the Virtual editor; it starts from the fixtures, not the saved
        // workspace, so the dump is what is shown.
        if (std::wstring (argv[i]) == L"--dump" && i + 1 < argc)
            dumps.push_back (argv[++i]), fresh = true;
        // A screenshot of frame N, to the export folder, as F12 takes one (unattended checks).
        if (std::wstring (argv[i]) == L"--screenshot" && i + 1 < argc)
            screenshotFrame = _wtoi (argv[++i]);
        // Open one fixture by its variant or file name; an editor fixture on the native editor page.
        if (std::wstring (argv[i]) == L"--select" && i + 1 < argc) {
            const std::wstring wide = argv[++i];
            select.assign (wide.begin (), wide.end ());
            fresh = true;
        }
    }
    LocalFree (argv);
    wchar_t executable[32768] {};
    GetModuleFileNameW (nullptr, executable, DWORD (std::size (executable)));
    const fs::path bin = fs::path (executable).parent_path ();
    const fs::path uiDll = bin / "TapiocaUIPlaygroundUI.dll";
    const fs::path stateFile = bin / "workspace.json";
    std::string state = InitialState (fs::path (TAPIOCA_PLAYGROUND_SOURCE) / "fixtures", dumps, select);
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
        if (g_screenshotRequested || (screenshotFrame > 0 && frames + 1 == screenshotFrame)) {
            g_screenshotRequested = false;
            try {
                const auto file =
                    FullScreenshot (graphics, module, width, float (GetDpiForWindow (window.handle)) / 96);
                reloadStatus = "Screenshot saved: " + file.string () + "\n";
            }
            catch (const std::exception& failure) {
                reloadStatus = std::string (failure.what ()) + "\n";
            }
            diagnostic << reloadStatus << std::flush;
        }
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
