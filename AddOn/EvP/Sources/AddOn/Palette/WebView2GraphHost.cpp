#include "Palette/WebView2GraphHost.hpp"

#include "Palette/GraphLibraryChooser.hpp"
#include "Python/ApiDispatcher.hpp"
#include "Python/PathUtils.hpp"

#include <windows.h>
#include <wrl.h>

#include <WebView2.h>

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kChildClassName[] = L"Tapioca.WebView2GraphChild";

// RSLoadResource failed first when the bundle crossed 1 MiB; the palette now
// bypasses that GSHandle path. Serving also avoids NavigateToString's separate
// 2 MiB UTF-16 limit and gives localStorage a non-opaque origin. The .invalid
// name is reserved and cannot collide with a real site.
constexpr wchar_t kEditorUrl[] = L"https://graph.tapioca.invalid/index.html";
constexpr wchar_t kEditorFilter[] = L"https://graph.tapioca.invalid/*";

// The `window.EvP.call` bridge, in the shape the GraphUI bundle already expects
// from DG::Browser: one JSON request string in, one JSON envelope string out.
//
// ⚠️ WITHOUT THIS THE PANEL IS NOT WRONG, IT IS EMPTY. The editor asks the
// runtime for its catalog through EvP.call, and when the call is unavailable it
// falls back to a two-node browser fixture - which is exactly what this host
// showed while it was a pure rendering measurement. The component picker then
// offers Source and Sink and none of the real node types, because there is
// nobody to ask.
//
// It is installed with AddScriptToExecuteOnDocumentCreated, so it is in place
// BEFORE the bundle's first line runs. The editor samples the bridge once at
// startup, and this ordering is what makes that sample right rather than racy.
//
// The wire framing is "<id>\n<command>\n<paramsJson>" rather than a JSON
// message. The request arrives here already JSON-encoded, and splitting it in
// JavaScript - which has a parser - keeps the native side free of one.
constexpr wchar_t kBridgeScript[] = L"(function(){"
                                    L"var wv=window.chrome&&window.chrome.webview;"
                                    L"if(!wv)return;"
                                    L"var next=0,pending={};"
                                    L"wv.addEventListener('message',function(event){"
                                    L"var text=String(event.data),cut=text.indexOf('\\n');"
                                    L"if(cut<0)return;"
                                    L"var id=text.slice(0,cut),resolve=pending[id];"
                                    L"if(resolve===undefined)return;"
                                    L"delete pending[id];"
                                    L"resolve(text.slice(cut+1));"
                                    L"});"
                                    L"window.EvP={call:function(requestJson){"
                                    L"return new Promise(function(resolve){"
                                    L"var request=null;"
                                    L"try{request=JSON.parse(String(requestJson));}catch(error){request=null;}"
                                    L"if(request===null||typeof request.command!=='string'||request.command===''){"
                                    L"resolve('{\"ok\":false,\"error\":{\"code\":\"BadRequest\",\"message\":"
                                    L"\"EvP.call expects a JSON object string carrying a command.\"}}');"
                                    L"return;"
                                    L"}"
                                    L"var id=String(++next);"
                                    L"pending[id]=resolve;"
                                    L"wv.postMessage(id+'\\n'+request.command+'\\n'+"
                                    L"JSON.stringify(request.params===undefined?{}:request.params));"
                                    L"});"
                                    L"}};"
                                    L"})();";

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

// The other direction, for a web message coming back off the wire. Windows
// wchar_t and UniChar::Layout are both UTF-16, so this is a reinterpretation
// rather than a conversion - and the length is passed explicitly so a request
// is not truncated at a NUL a page could put in it.
GS::UniString UniStringFrom (const std::wstring& value)
{
    static_assert (sizeof (wchar_t) == sizeof (GS::UniChar::Layout),
                   "UniString expects UTF-16, which is what wchar_t is on Windows");
    return GS::UniString (reinterpret_cast<const GS::UniChar::Layout*> (value.c_str ()), USize (value.size ()));
}

// One bridge request, answered on the thread the web view delivered it on -
// which is the thread that created the controller, which is Archicad's main
// thread. DispatchApiCall does its own gating from there, and the chooser is a
// modal dialog that MUST already be on the main thread; see
// GraphLibraryChooser.hpp for why that one is not a registered verb.
GS::UniString AnswerBridgeRequest (const GS::UniString& command, const GS::UniString& paramsJson)
{
    if (command == GS::UniString (evp::kGraphLibraryBrowseCommand))
        return evp::RunGraphLibraryChooser (paramsJson);
    return evp::DispatchApiCall (command, paramsJson, "nodegraph");
}

// The page as the BYTES that go on the wire. The response is served as UTF-8,
// which is what the document declares, so it is converted once here rather than
// per request.
std::vector<char> Utf8Bytes (const GS::UniString& value)
{
    const auto text = value.ToCStr (0, GS::MaxUSize, CC_UTF8);
    const char* begin = text.Get ();
    return std::vector<char> (begin, begin + std::strlen (begin));
}

// An IStream over a copy of `bytes`, owned by the stream. ole32 is already in
// this translation unit for CoInitializeEx, so this adds no dependency.
ComPtr<IStream> StreamOver (const std::vector<char>& bytes)
{
    const HGLOBAL memory = GlobalAlloc (GMEM_MOVEABLE, bytes.size ());
    if (memory == nullptr)
        return nullptr;
    void* target = GlobalLock (memory);
    if (target == nullptr) {
        GlobalFree (memory);
        return nullptr;
    }
    std::memcpy (target, bytes.data (), bytes.size ());
    GlobalUnlock (memory);
    ComPtr<IStream> stream;
    // TRUE: the stream frees the block, so a failure below cannot leak it.
    if (FAILED (CreateStreamOnHGlobal (memory, TRUE, &stream))) {
        GlobalFree (memory);
        return nullptr;
    }
    return stream;
}

} // namespace

struct WebView2GraphHostState {
    HWND parent = nullptr;
    HWND child = nullptr;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webView;
    // Kept so the resource handler can answer every request, including a reload.
    ComPtr<ICoreWebView2Environment> environment;
    std::vector<char> htmlBytes;
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
    state_->htmlBytes = Utf8Bytes (html);
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
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> ([state] (HRESULT environmentResult,
                                                                                       ICoreWebView2Environment*
                                                                                           environment) -> HRESULT {
            if (state->closing.load ())
                return S_OK;
            if (FAILED (environmentResult) || environment == nullptr) {
                WebView2Log (
                    GS::UniString::Printf ("environment creation failed: 0x%08X", unsigned (environmentResult)));
                return S_OK;
            }
            state->environment = environment;
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

                        // Kept separate from the telemetry script because that
                        // one is re-executed on every monitor change, and the
                        // bridge must be installed exactly once per document.
                        state->webView->AddScriptToExecuteOnDocumentCreated (kBridgeScript, nullptr);
                        EventRegistrationToken messageToken = {};
                        state->webView->add_WebMessageReceived (
                            Callback<ICoreWebView2WebMessageReceivedEventHandler> (
                                [state] (ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                    if (args == nullptr || state->closing.load () || state->webView == nullptr)
                                        return S_OK;

                                    LPWSTR raw = nullptr;
                                    if (FAILED (args->TryGetWebMessageAsString (&raw)) || raw == nullptr)
                                        return S_OK;
                                    const std::wstring message (raw);
                                    CoTaskMemFree (raw);

                                    // "<id>\n<command>\n<paramsJson>". A message
                                    // that is not that shape is not ours; the
                                    // page has no other reason to post one, but
                                    // dropping it is cheaper than guessing.
                                    const size_t firstBreak = message.find (L'\n');
                                    if (firstBreak == std::wstring::npos)
                                        return S_OK;
                                    const size_t secondBreak = message.find (L'\n', firstBreak + 1);
                                    if (secondBreak == std::wstring::npos)
                                        return S_OK;

                                    const std::wstring id = message.substr (0, firstBreak);
                                    const GS::UniString command =
                                        UniStringFrom (message.substr (firstBreak + 1, secondBreak - firstBreak - 1));
                                    const GS::UniString params = UniStringFrom (message.substr (secondBreak + 1));

                                    const GS::UniString envelope = AnswerBridgeRequest (command, params);
                                    // The reply carries the id back rather than
                                    // relying on order: a command that opens a
                                    // modal dialog finishes long after a status
                                    // call the page made behind it.
                                    const std::wstring reply = id + L"\n" + WideString (envelope);
                                    state->webView->PostWebMessageAsString (reply.c_str ());
                                    return S_OK;
                                })
                                .Get (),
                            &messageToken);

                        // Serve the bundle from a virtual origin instead of
                        // pushing it through NavigateToString's 2 MB cap.
                        state->webView->AddWebResourceRequestedFilter (kEditorFilter,
                                                                       COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                        EventRegistrationToken token = {};
                        state->webView->add_WebResourceRequested (
                            Callback<ICoreWebView2WebResourceRequestedEventHandler> (
                                [state] (ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                                    if (args == nullptr || state->environment == nullptr)
                                        return S_OK;

                                    // Only the document itself. The filter is
                                    // the whole origin, so a browser-initiated
                                    // /favicon.ico lands here too, and
                                    // answering it with a megabyte of HTML
                                    // would be wrong as well as wasteful.
                                    ComPtr<ICoreWebView2WebResourceRequest> request;
                                    LPWSTR uri = nullptr;
                                    bool wantsDocument = false;
                                    if (SUCCEEDED (args->get_Request (&request)) && request != nullptr &&
                                        SUCCEEDED (request->get_Uri (&uri)) && uri != nullptr) {
                                        wantsDocument = wcscmp (uri, kEditorUrl) == 0;
                                        CoTaskMemFree (uri);
                                    }

                                    ComPtr<ICoreWebView2WebResourceResponse> response;
                                    if (!wantsDocument) {
                                        if (SUCCEEDED (state->environment->CreateWebResourceResponse (
                                                nullptr, 404, L"Not Found", L"", &response))) {
                                            args->put_Response (response.Get ());
                                        }
                                        return S_OK;
                                    }

                                    const ComPtr<IStream> body = StreamOver (state->htmlBytes);
                                    if (body == nullptr)
                                        return S_OK;
                                    if (FAILED (state->environment->CreateWebResourceResponse (
                                            body.Get (), 200, L"OK", L"Content-Type: text/html; charset=utf-8",
                                            &response))) {
                                        return S_OK;
                                    }
                                    args->put_Response (response.Get ());
                                    return S_OK;
                                })
                                .Get (),
                            &token);

                        const HRESULT navigateResult = state->webView->Navigate (kEditorUrl);
                        WebView2Log (
                            SUCCEEDED (navigateResult)
                                ? GS::UniString::Printf ("native child HWND controller started, serving %u bytes",
                                                         unsigned (state->htmlBytes.size ()))
                                : GS::UniString::Printf ("Navigate failed: 0x%08X", unsigned (navigateResult)));
                        return S_OK;
                    })
                    .Get ());
        }).Get ());
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
