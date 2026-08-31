#include "Palette/WebView2GraphHost.hpp"

#include "Python/PathUtils.hpp"

#include <windows.h>
#include <wrl.h>

#include <WebView2.h>

#include <atomic>
#include <string>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kChildClassName[] = L"Tapioca.WebView2GraphChild";

void WebView2Log (const GS::UniString& message)
{
    const GS::UniString dataDir = evp::EvpDataDir ();
    if (dataDir.IsEmpty ())
        return;
    const GS::UniString logsDir = dataDir + GS::UniString ("\\logs");
    evp::CreateDirectoryChain (logsDir);
    evp::AppendTextLine (logsDir + GS::UniString ("\\startup.log"), GS::UniString ("WebView2 graph: ") + message);
}

double DisplayRefreshHz (HWND window)
{
    MONITORINFOEXW info = {};
    info.cbSize = sizeof (info);
    if (!GetMonitorInfoW (MonitorFromWindow (window, MONITOR_DEFAULTTONEAREST), &info))
        return 0.0;
    DEVMODEW mode = {};
    mode.dmSize = sizeof (mode);
    if (!EnumDisplaySettingsW (info.szDevice, ENUM_CURRENT_SETTINGS, &mode) || mode.dmDisplayFrequency <= 1)
        return 0.0;
    return double (mode.dmDisplayFrequency);
}

std::wstring WideString (const GS::UniString& value)
{
    const auto text = value.ToUStr ();
    return std::wstring (reinterpret_cast<const wchar_t*> (text.Get ()), value.GetLength ());
}

} // namespace

struct WebView2GraphHostState {
    HWND parent = nullptr;
    HWND child = nullptr;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webView;
    std::wstring html;
    std::atomic<bool> closing = false;
    bool comInitialized = false;

    void UpdateBounds ()
    {
        if (child == nullptr || controller == nullptr)
            return;
        RECT rect = {};
        GetClientRect (child, &rect);
        controller->put_Bounds (rect);
    }

    std::wstring TelemetryScript () const
    {
        wchar_t script[512] = {};
        swprintf_s (script,
                    L"window.__tapiocaHost='webview2-child';"
                    L"window.__tapiocaNativeTelemetry={actualDisplayRefreshHz:%.3f,"
                    L"topology:'DG::Palette > native child HWND > WebView2',"
                    L"archicadSwapChainIntercepted:false};",
                    DisplayRefreshHz (child));
        return script;
    }
};

namespace {

LRESULT CALLBACK ChildWindowProc (HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<WebView2GraphHostState*> (GetWindowLongPtrW (window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*> (lParam);
        state = static_cast<WebView2GraphHostState*> (create->lpCreateParams);
        SetWindowLongPtrW (window, GWLP_USERDATA, reinterpret_cast<LONG_PTR> (state));
    }
    if (state != nullptr && message == WM_SIZE)
        state->UpdateBounds ();
    if (message == WM_ERASEBKGND)
        return 1;
    return DefWindowProcW (window, message, wParam, lParam);
}

bool RegisterChildClass ()
{
    static const bool registered = [] () {
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof (windowClass);
        windowClass.lpfnWndProc = ChildWindowProc;
        windowClass.hInstance = GetModuleHandleW (nullptr);
        windowClass.hCursor = LoadCursorW (nullptr, IDC_ARROW);
        windowClass.lpszClassName = kChildClassName;
        return RegisterClassExW (&windowClass) != 0 || GetLastError () == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return registered;
}

} // namespace

WebView2GraphHost::WebView2GraphHost () : state_ (std::make_shared<WebView2GraphHostState> ())
{
}

WebView2GraphHost::~WebView2GraphHost ()
{
    state_->closing.store (true);
    if (state_->controller != nullptr)
        state_->controller->Close ();
    state_->webView.Reset ();
    state_->controller.Reset ();
    if (state_->child != nullptr && IsWindow (state_->child))
        DestroyWindow (state_->child);
    state_->child = nullptr;
    if (state_->comInitialized)
        CoUninitialize ();
}

bool WebView2GraphHost::Start (HWND__* parent, const GS::UniString& html)
{
    if (parent == nullptr) {
        WebView2Log ("DGGetDialogItemWindow returned null for the palette surface");
        return false;
    }
    if (!IsWindow (parent)) {
        WebView2Log (GS::UniString::Printf ("palette surface 0x%p is not an HWND", parent));
        return false;
    }
    if (!RegisterChildClass ()) {
        WebView2Log (GS::UniString::Printf ("child window class registration failed: %u", GetLastError ()));
        return false;
    }
    if (state_->child != nullptr) {
        SetVisible (true);
        Resize ();
        return true;
    }

    state_->parent = parent;
    state_->html = WideString (html);
    const HRESULT comResult = CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
    state_->comInitialized = SUCCEEDED (comResult);
    if (FAILED (comResult) && comResult != RPC_E_CHANGED_MODE) {
        WebView2Log (GS::UniString::Printf ("COM initialization failed: 0x%08X", unsigned (comResult)));
        return false;
    }

    state_->child = CreateWindowExW (0, kChildClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                     0, 0, 1, 1, parent, nullptr, GetModuleHandleW (nullptr), state_.get ());
    if (state_->child == nullptr) {
        WebView2Log ("native child HWND creation failed");
        return false;
    }
    Resize ();

    const GS::UniString userDataDir = evp::EvpDataDir () + GS::UniString ("\\WebView2");
    evp::CreateDirectoryChain (userDataDir);
    const std::wstring userDataPath = WideString (userDataDir);
    const std::shared_ptr<WebView2GraphHostState> state = state_;
    const HRESULT result = CreateCoreWebView2EnvironmentWithOptions (
        nullptr, userDataPath.c_str (), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> (
            [state] (HRESULT environmentResult, ICoreWebView2Environment* environment) -> HRESULT {
                if (state->closing.load ())
                    return S_OK;
                if (FAILED (environmentResult) || environment == nullptr) {
                    WebView2Log (
                        GS::UniString::Printf ("environment creation failed: 0x%08X", unsigned (environmentResult)));
                    return S_OK;
                }
                return environment->CreateCoreWebView2Controller (
                    state->child,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> (
                        [state] (HRESULT controllerResult, ICoreWebView2Controller* controller) -> HRESULT {
                            if (state->closing.load ())
                                return S_OK;
                            if (FAILED (controllerResult) || controller == nullptr) {
                                WebView2Log (GS::UniString::Printf ("controller creation failed: 0x%08X",
                                                                    unsigned (controllerResult)));
                                return S_OK;
                            }
                            state->controller = controller;
                            controller->get_CoreWebView2 (&state->webView);
                            state->UpdateBounds ();

                            ComPtr<ICoreWebView2Settings> settings;
                            if (state->webView != nullptr && SUCCEEDED (state->webView->get_Settings (&settings))) {
                                settings->put_AreDefaultContextMenusEnabled (FALSE);
                                settings->put_AreDevToolsEnabled (FALSE);
                                settings->put_IsStatusBarEnabled (FALSE);
                                settings->put_IsZoomControlEnabled (FALSE);
                            }
                            if (state->webView == nullptr)
                                return S_OK;

                            const std::wstring startupScript = state->TelemetryScript ();
                            state->webView->AddScriptToExecuteOnDocumentCreated (startupScript.c_str (), nullptr);
                            const HRESULT navigateResult = state->webView->NavigateToString (state->html.c_str ());
                            WebView2Log (SUCCEEDED (navigateResult)
                                             ? "native child HWND controller started"
                                             : GS::UniString::Printf ("NavigateToString failed: 0x%08X",
                                                                      unsigned (navigateResult)));
                            return S_OK;
                        })
                        .Get ());
            })
            .Get ());
    if (FAILED (result)) {
        WebView2Log (GS::UniString::Printf ("environment request failed: 0x%08X", unsigned (result)));
        return false;
    }
    return true;
}

void WebView2GraphHost::Resize ()
{
    if (state_->parent == nullptr || state_->child == nullptr)
        return;
    RECT rect = {};
    GetClientRect (state_->parent, &rect);
    SetWindowPos (state_->child, nullptr, 0, 0, rect.right, rect.bottom, SWP_NOACTIVATE | SWP_NOZORDER);
    state_->UpdateBounds ();
}

void WebView2GraphHost::SetVisible (bool visible)
{
    if (state_->child != nullptr)
        ShowWindow (state_->child, visible ? SW_SHOWNA : SW_HIDE);
    if (state_->controller != nullptr)
        state_->controller->put_IsVisible (visible ? TRUE : FALSE);
}

void WebView2GraphHost::Focus ()
{
    if (state_->child != nullptr)
        SetFocus (state_->child);
    if (state_->controller != nullptr)
        state_->controller->MoveFocus (COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

void WebView2GraphHost::RefreshNativeTelemetry ()
{
    if (state_->webView != nullptr) {
        const std::wstring script = state_->TelemetryScript ();
        state_->webView->ExecuteScript (script.c_str (), nullptr);
    }
}
