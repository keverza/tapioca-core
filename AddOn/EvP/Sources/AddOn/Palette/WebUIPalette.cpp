#include "Palette/WebUIPalette.hpp"

#include "Python/ApiDispatcher.hpp"
#include "Python/PathUtils.hpp"
#include "ResourceIds.hpp"
#include "Server/HttpServer.hpp"

#include <cstring>
#include <string>

#include "ObjectState.hpp"
#include "ObjectStateJSONConversion.hpp"

const GS::Guid WebUIPalette::paletteGuid ("{A31D1E4E-70E7-4D4E-A5DA-CCF994D9DB15}");
GS::Ref<WebUIPalette> WebUIPalette::instance;

namespace {

const GS::UniString kResourceFallbackHtml (
    "<!doctype html><html><head><meta charset=\"utf-8\"><style>"
    "html,body{height:100%;margin:0}body{display:grid;place-items:center;background:#f3f4f6;"
    "color:#202124;font:14px system-ui,sans-serif}main{max-width:420px;padding:24px}"
    "h1{font-size:18px;margin:0 0 10px}p{line-height:1.5;margin:0;color:#5f6368;user-select:text}"
    "@media(prefers-color-scheme:dark){body{background:#252629;color:#f1f3f4}p{color:#bdc1c6}}"
    "</style></head><body><main><h1>Tapioca WebUI could not load</h1>"
    "<p>Build the add-on again, then check %LOCALAPPDATA%\\Tapioca\\logs\\startup.log "
    "for the WebUI resource error.</p></main></body></html>");

void WebUILog (const GS::UniString& message)
{
    const GS::UniString dataDir = evp::EvpDataDir ();
    if (dataDir.IsEmpty ())
        return;

    const GS::UniString logsDir = dataDir + GS::UniString ("\\logs");
    evp::CreateDirectoryChain (logsDir);
    evp::AppendTextLine (logsDir + GS::UniString ("\\startup.log"), GS::UniString ("WebUI: ") + message);
}

GS::UniString LoadWebUIHtmlResource ()
{
    GSHandle data = RSLoadResource ('DATA', ACAPI_GetOwnResModule (), WebUIHtmlResId);
    if (data == nullptr) {
        WebUILog (GS::UniString::Printf ("HTML resource %d is missing", WebUIHtmlResId));
        return GS::EmptyUniString;
    }

    const GSSize handleSize = BMhGetSize (data);
    if (handleSize <= 0) {
        WebUILog (GS::UniString::Printf ("HTML resource %d has invalid size %d", WebUIHtmlResId, (int) handleSize));
        BMhKill (&data);
        return GS::EmptyUniString;
    }

    const std::string bytes (*data, *data + handleSize);
    BMhKill (&data);
    if (bytes.find ('\0') != std::string::npos) {
        WebUILog ("HTML resource contains an embedded NUL byte");
        return GS::EmptyUniString;
    }

    const GS::UniString html (bytes.c_str (), CC_UTF8);
    const auto roundTrip = html.ToCStr (0, GS::MaxUSize, CC_UTF8);
    if (std::strlen (roundTrip.Get ()) != bytes.size () ||
        std::memcmp (roundTrip.Get (), bytes.data (), bytes.size ()) != 0) {
        WebUILog ("HTML resource is not valid UTF-8");
        return GS::EmptyUniString;
    }
    if (bytes.find ("<!doctype html>") == std::string::npos || bytes.find ("id=\"app\"") == std::string::npos) {
        WebUILog ("HTML resource is UTF-8 but is not the expected WebUI document");
        return GS::EmptyUniString;
    }

    WebUILog (
        GS::UniString::Printf ("loaded HTML resource %d with %u bytes", WebUIHtmlResId, (unsigned int) handleSize));
    return html;
}

GS::Ref<JS::Base> JavaScriptString (const GS::UniString& value)
{
    return new JS::Value (value);
}

GS::UniString JavaScriptStringValue (const GS::Ref<JS::Base>& parameter)
{
    const GS::Ref<JS::Value> value = GS::DynamicCast<JS::Value> (parameter);
    if (value == nullptr || value->GetType () != JS::Value::STRING)
        return GS::EmptyUniString;
    return value->GetString ();
}

GS::Ref<JS::Base> DispatchJavaScriptCall (GS::Ref<JS::Base> parameter)
{
    const GS::UniString requestJson = JavaScriptStringValue (parameter);
    GS::ObjectState request;
    if (requestJson.IsEmpty () || JSON::ConvertToObjectState (requestJson, request) != NoError)
        return JavaScriptString ("{\"ok\":false,\"error\":{\"code\":\"BadRequest\",\"message\":\"EvP.call expects a "
                                 "JSON object string.\"}}");

    GS::UniString command;
    GS::ObjectState params;
    request.Get ("command", command);
    request.Get ("params", params);
    if (command.IsEmpty ())
        return JavaScriptString (
            "{\"ok\":false,\"error\":{\"code\":\"BadRequest\",\"message\":\"EvP.call requires command.\"}}");

    GS::UniString paramsJson;
    if (JSON::CreateFromObjectState (params, paramsJson) != NoError)
        return JavaScriptString ("{\"ok\":false,\"error\":{\"code\":\"BadRequest\",\"message\":\"EvP.call params could "
                                 "not be serialized.\"}}");

    return JavaScriptString (evp::DispatchApiCall (command, paramsJson, "webui"));
}

} // namespace

WebUIPalette::WebUIPalette ()
    : DG::Palette (ACAPI_GetOwnResModule (), WebUIPaletteResId, ACAPI_GetOwnResModule (), paletteGuid),
      browser (GetReference (), WebUIBrowserId)
{
    WebUILog ("constructor entered");
    Attach (*this);
    BeginEventProcessing ();
    WebUILog ("DG event processing started");

    browser.SetContextMenuMode (DG::BrowserBase::ContextMenuMode::Disabled);
    browser.SetScrollBarVisibility (true);
    RegisterJavaScriptObject ();
    WebUILog ("browser settings applied; navigation remains enabled for initial LoadHTML");

    browser.onLoadingStateChange += [] (const DG::BrowserBase&, const DG::BrowserLoadingStateChangeArg& event) {
        WebUILog (event.isLoading ? "browser load started" : "browser load finished");
        if (!event.isLoading && WebUIPalette::HasInstance ()) {
            WebUIPalette& palette = WebUIPalette::GetInstance ();
            const bool accepted = palette.browser.ExecuteJS ("document.documentElement.dataset.tapiocaHost='ready'");
            WebUILog (accepted ? "post-load JavaScript accepted" : "post-load JavaScript rejected");
            palette.browser.DisableNavigation (true);
            WebUILog ("browser navigation disabled after load");
        }
    };
    browser.onLoadError += [] (const DG::BrowserBase&, const DG::BrowserLoadErrorArg& event) {
        WebUILog (GS::UniString::Printf ("browser load error %d at %T", (int) event.errorCode, event.url.ToPrintf ()));
        if (!WebUIPalette::HasInstance ())
            return;

        WebUIPalette& palette = WebUIPalette::GetInstance ();
        if (!palette.serverPageRequested || palette.fallbackPageLoaded)
            return;

        palette.serverPageRequested = false;
        palette.fallbackPageLoaded = true;
        palette.browser.DisableNavigation (false);
        WebUILog ("loading embedded WebUI fallback after server page failure");
        palette.browser.LoadHTML (palette.fallbackHtml);
    };
    browser.onUrlChanged += [] (const DG::BrowserBase&, const DG::BrowserURLChangeArg& event) {
        WebUILog (GS::UniString ("browser URL changed: ") + event.newURL);
    };
    WebUILog ("browser diagnostics attached; waiting for palette show");
}

void WebUIPalette::RegisterJavaScriptObject ()
{
    JS::Object* evpObject = new JS::Object ("EvP");
    evpObject->AddItem (new JS::Function ("call", DispatchJavaScriptCall));
    browser.RegisterAsynchJSObject (evpObject);
    WebUILog ("EvP.call browser bridge registered");
}

WebUIPalette::~WebUIPalette ()
{
    WebUILog ("destructor entered");
    EndEventProcessing ();
    WebUILog ("DG event processing stopped");
}

bool WebUIPalette::HasInstance ()
{
    return instance != nullptr;
}

void WebUIPalette::CreateInstance ()
{
    if (!HasInstance ()) {
        WebUILog ("creating palette instance");
        instance = new WebUIPalette ();
        ACAPI_KeepInMemory (true);
        WebUILog ("palette instance created; add-on kept in memory");
    }
}

WebUIPalette& WebUIPalette::GetInstance ()
{
    return *instance;
}

void WebUIPalette::DestroyInstance ()
{
    WebUILog ("destroying palette instance");
    instance = nullptr;
    WebUILog ("palette instance destroyed");
}

void WebUIPalette::Open ()
{
    if (!HasInstance ())
        CreateInstance ();
    GetInstance ().Show ();
    GetInstance ().LoadWebUIPage ();
}

void WebUIPalette::Show ()
{
    WebUILog ("show requested");
    DG::Palette::Show ();
    WebUILog ("palette shown");
}

void WebUIPalette::Hide ()
{
    WebUILog ("hide requested");
    DG::Palette::Hide ();
    WebUILog ("palette hidden");
}

void WebUIPalette::LoadWebUIPage ()
{
    browser.DisableNavigation (false);
    WebUILog ("browser navigation enabled for LoadHTML");
    fallbackHtml = LoadWebUIHtmlResource ();
    if (fallbackHtml.IsEmpty ()) {
        WebUILog ("loading in-browser resource failure page");
        fallbackHtml = kResourceFallbackHtml;
    }

    const auto htmlUtf8 = fallbackHtml.ToCStr (0, GS::MaxUSize, CC_UTF8);
    geomsrv::HttpServer& server = geomsrv::SharedHttpServer ();
    server.SetWebUIPage (std::string (htmlUtf8.Get ()));
    if (!server.Start ()) {
        WebUILog ("shared HTTP server did not start; loading embedded page");
        fallbackPageLoaded = true;
        browser.LoadHTML (fallbackHtml);
        return;
    }

    serverPageRequested = true;
    fallbackPageLoaded = false;
    const GS::UniString url = GS::UniString::Printf ("http://127.0.0.1:%d/ui", server.Port ());
    WebUILog (GS::UniString ("calling LoadURL: ") + url);
    browser.LoadURL (url);
}

void WebUIPalette::PanelCloseRequested (const DG::PanelCloseRequestEvent&, bool* accepted)
{
    WebUILog ("close requested");
    Hide ();
    *accepted = true;
}

void WebUIPalette::PanelResized (const DG::PanelResizeEvent& ev)
{
    BeginMoveResizeItems ();
    browser.Resize (ev.GetHorizontalChange (), ev.GetVerticalChange ());
    EndMoveResizeItems ();
}

GSErrCode WebUIPalette::PaletteControlCallBack (Int32, API_PaletteMessageID messageId, GS::IntPtr param)
{
    switch (messageId) {
        case APIPalMsg_OpenPalette:
            Open ();
            break;
        case APIPalMsg_ClosePalette:
            if (HasInstance ())
                GetInstance ().Hide ();
            break;
        case APIPalMsg_HidePalette_Begin:
            if (HasInstance () && GetInstance ().IsVisible ())
                GetInstance ().Hide ();
            break;
        case APIPalMsg_HidePalette_End:
            if (HasInstance () && !GetInstance ().IsVisible ())
                GetInstance ().Show ();
            break;
        case APIPalMsg_DisableItems_Begin:
            if (HasInstance () && GetInstance ().IsVisible ())
                GetInstance ().DisableItems ();
            break;
        case APIPalMsg_DisableItems_End:
            if (HasInstance () && GetInstance ().IsVisible ())
                GetInstance ().EnableItems ();
            break;
        case APIPalMsg_IsPaletteVisible:
            *(reinterpret_cast<bool*> (param)) = HasInstance () && GetInstance ().IsVisible ();
            break;
        case APIPalMsg_GetPaletteDeactivationMethod:
            *(reinterpret_cast<API_PaletteDeactivationMethod*> (param)) = APIPaletteDeactivationMethod_Default;
            break;
        default:
            break;
    }
    return NoError;
}

GSErrCode WebUIPalette::RegisterPaletteControlCallBack ()
{
    return ACAPI_RegisterModelessWindow (GS::CalculateHashValue (paletteGuid), PaletteControlCallBack,
                                         API_PalEnabled_FloorPlan + API_PalEnabled_Section + API_PalEnabled_Elevation +
                                             API_PalEnabled_InteriorElevation + API_PalEnabled_3D +
                                             API_PalEnabled_Detail + API_PalEnabled_Worksheet + API_PalEnabled_Layout +
                                             API_PalEnabled_DocumentFrom3D,
                                         GSGuid2APIGuid (paletteGuid));
}

GSErrCode WebUIPalette::UnregisterPaletteControlCallBack ()
{
    return ACAPI_UnregisterModelessWindow (GS::CalculateHashValue (paletteGuid));
}
