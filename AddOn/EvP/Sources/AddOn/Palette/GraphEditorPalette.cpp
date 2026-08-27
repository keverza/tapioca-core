#include "Palette/GraphEditorPalette.hpp"

#include "Python/ApiDispatcher.hpp"
#include "Python/PathUtils.hpp"
#include "ResourceIds.hpp"

#include <cstring>
#include <string>

#include "ObjectState.hpp"
#include "ObjectStateJSONConversion.hpp"

const GS::Guid GraphEditorPalette::paletteGuid ("{D08D1A6F-962C-4C96-A20B-9B176981AEB2}");
GS::Ref<GraphEditorPalette> GraphEditorPalette::instance;

namespace {

const GS::UniString kResourceFallbackHtml (
    "<!doctype html><html><head><meta charset=\"utf-8\"><style>"
    "html,body{height:100%;margin:0}body{display:grid;place-items:center;background:#11151a;"
    "color:#edf2f7;font:14px system-ui,sans-serif}main{max-width:420px;padding:24px}"
    "h1{font-size:18px;margin:0 0 10px}p{line-height:1.5;margin:0;color:#aeb8c4;user-select:text}"
    "</style></head><body><main><h1>Tapioca Node Graph could not load</h1>"
    "<p>Build the add-on again, then check %LOCALAPPDATA%\\Tapioca\\logs\\startup.log "
    "for the Node Graph resource error.</p></main></body></html>");

void GraphEditorLog (const GS::UniString& message)
{
    const GS::UniString dataDir = evp::EvpDataDir ();
    if (dataDir.IsEmpty ())
        return;

    const GS::UniString logsDir = dataDir + GS::UniString ("\\logs");
    evp::CreateDirectoryChain (logsDir);
    evp::AppendTextLine (logsDir + GS::UniString ("\\startup.log"), GS::UniString ("Node Graph: ") + message);
}

GS::UniString LoadGraphEditorHtmlResource ()
{
    GSHandle data = RSLoadResource ('DATA', ACAPI_GetOwnResModule (), GraphEditorHtmlResId);
    if (data == nullptr) {
        GraphEditorLog (GS::UniString::Printf ("HTML resource %d is missing", GraphEditorHtmlResId));
        return GS::EmptyUniString;
    }

    const GSSize handleSize = BMhGetSize (data);
    if (handleSize <= 0) {
        GraphEditorLog (
            GS::UniString::Printf ("HTML resource %d has invalid size %d", GraphEditorHtmlResId, (int) handleSize));
        BMhKill (&data);
        return GS::EmptyUniString;
    }

    const std::string bytes (*data, *data + handleSize);
    BMhKill (&data);
    if (bytes.find ('\0') != std::string::npos) {
        GraphEditorLog ("HTML resource contains an embedded NUL byte");
        return GS::EmptyUniString;
    }

    const GS::UniString html (bytes.c_str (), CC_UTF8);
    const auto roundTrip = html.ToCStr (0, GS::MaxUSize, CC_UTF8);
    if (std::strlen (roundTrip.Get ()) != bytes.size () ||
        std::memcmp (roundTrip.Get (), bytes.data (), bytes.size ()) != 0) {
        GraphEditorLog ("HTML resource is not valid UTF-8");
        return GS::EmptyUniString;
    }
    if (bytes.find ("<!doctype html>") == std::string::npos || bytes.find ("id=\"app\"") == std::string::npos) {
        GraphEditorLog ("HTML resource is UTF-8 but is not the expected Node Graph document");
        return GS::EmptyUniString;
    }

    GraphEditorLog (GS::UniString::Printf ("loaded HTML resource %d with %u bytes", GraphEditorHtmlResId,
                                           (unsigned int) handleSize));
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

GraphEditorPalette::GraphEditorPalette ()
    : DG::Palette (ACAPI_GetOwnResModule (), GraphEditorPaletteResId, ACAPI_GetOwnResModule (), paletteGuid),
      browser (GetReference (), GraphEditorBrowserId)
{
    GraphEditorLog ("constructor entered");
    Attach (*this);
    BeginEventProcessing ();

    browser.SetContextMenuMode (DG::BrowserBase::ContextMenuMode::Disabled);
    browser.SetScrollBarVisibility (true);
    RegisterJavaScriptObject ();

    browser.onLoadingStateChange += [] (const DG::BrowserBase&, const DG::BrowserLoadingStateChangeArg& event) {
        GraphEditorLog (event.isLoading ? "browser load started" : "browser load finished");
        if (!event.isLoading && GraphEditorPalette::HasInstance ()) {
            GraphEditorPalette& palette = GraphEditorPalette::GetInstance ();
            const bool accepted = palette.browser.ExecuteJS ("document.documentElement.dataset.tapiocaHost='ready'");
            GraphEditorLog (accepted ? "post-load JavaScript accepted" : "post-load JavaScript rejected");
            palette.browser.DisableNavigation (true);
            palette.browser.SetFocus ();
        }
    };
    browser.onLoadError += [] (const DG::BrowserBase&, const DG::BrowserLoadErrorArg& event) {
        GraphEditorLog (
            GS::UniString::Printf ("browser load error %d at %T", (int) event.errorCode, event.url.ToPrintf ()));
    };
}

GraphEditorPalette::~GraphEditorPalette ()
{
    EndEventProcessing ();
}

bool GraphEditorPalette::HasInstance ()
{
    return instance != nullptr;
}

void GraphEditorPalette::CreateInstance ()
{
    if (!HasInstance ()) {
        instance = new GraphEditorPalette ();
        ACAPI_KeepInMemory (true);
    }
}

GraphEditorPalette& GraphEditorPalette::GetInstance ()
{
    return *instance;
}

void GraphEditorPalette::DestroyInstance ()
{
    instance = nullptr;
}

void GraphEditorPalette::Open ()
{
    if (!HasInstance ())
        CreateInstance ();
    GetInstance ().Show ();
    GetInstance ().LoadGraphEditorPage ();
}

void GraphEditorPalette::Show ()
{
    DG::Palette::Show ();
    browser.SetFocus ();
}

void GraphEditorPalette::Hide ()
{
    DG::Palette::Hide ();
}

void GraphEditorPalette::LoadGraphEditorPage ()
{
    browser.DisableNavigation (false);
    GS::UniString html = LoadGraphEditorHtmlResource ();
    if (html.IsEmpty ())
        html = kResourceFallbackHtml;
    browser.LoadHTML (html);
}

void GraphEditorPalette::RegisterJavaScriptObject ()
{
    JS::Object* evpObject = new JS::Object ("EvP");
    evpObject->AddItem (new JS::Function ("call", DispatchJavaScriptCall));
    browser.RegisterAsynchJSObject (evpObject);
    GraphEditorLog ("EvP.call browser bridge registered");
}

void GraphEditorPalette::PanelCloseRequested (const DG::PanelCloseRequestEvent&, bool* accepted)
{
    Hide ();
    *accepted = true;
}

void GraphEditorPalette::PanelResized (const DG::PanelResizeEvent& ev)
{
    BeginMoveResizeItems ();
    browser.Resize (ev.GetHorizontalChange (), ev.GetVerticalChange ());
    EndMoveResizeItems ();
}

GSErrCode GraphEditorPalette::PaletteControlCallBack (Int32, API_PaletteMessageID messageId, GS::IntPtr param)
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

GSErrCode GraphEditorPalette::RegisterPaletteControlCallBack ()
{
    return ACAPI_RegisterModelessWindow (GS::CalculateHashValue (paletteGuid), PaletteControlCallBack,
                                         API_PalEnabled_FloorPlan + API_PalEnabled_Section + API_PalEnabled_Elevation +
                                             API_PalEnabled_InteriorElevation + API_PalEnabled_3D +
                                             API_PalEnabled_Detail + API_PalEnabled_Worksheet + API_PalEnabled_Layout +
                                             API_PalEnabled_DocumentFrom3D,
                                         GSGuid2APIGuid (paletteGuid));
}

GSErrCode GraphEditorPalette::UnregisterPaletteControlCallBack ()
{
    return ACAPI_UnregisterModelessWindow (GS::CalculateHashValue (paletteGuid));
}
