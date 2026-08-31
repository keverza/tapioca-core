#include "Palette/GraphEditorPalette.hpp"

#include "Palette/WebView2GraphHost.hpp"
#include "Python/PathUtils.hpp"
#include "ResourceIds.hpp"

#include "DGWin.h"

#include <cstring>
#include <string>

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

} // namespace

GraphEditorPalette::GraphEditorPalette ()
    : DG::Palette (ACAPI_GetOwnResModule (), GraphEditorPaletteResId, ACAPI_GetOwnResModule (), paletteGuid)
{
    GraphEditorLog ("constructor entered");
    Attach (*this);
    BeginEventProcessing ();
    surface = std::make_unique<DG::UserItem> (*this, DG::Rect (0, 0, GetClientWidth (), GetClientHeight ()),
                                              DG::UserItem::Normal, DG::UserItem::NoFrame);
    surface->Show ();
    webView = std::make_unique<WebView2GraphHost> ();
}

GraphEditorPalette::~GraphEditorPalette ()
{
    webView.reset ();
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
    if (webView != nullptr) {
        webView->SetVisible (true);
        webView->Focus ();
    }
}

void GraphEditorPalette::Hide ()
{
    if (webView != nullptr)
        webView->SetVisible (false);
    DG::Palette::Hide ();
}

void GraphEditorPalette::LoadGraphEditorPage ()
{
    GS::UniString html = LoadGraphEditorHtmlResource ();
    if (html.IsEmpty ())
        html = kResourceFallbackHtml;
    const HWND surfaceWindow = surface != nullptr ? DGGetDialogItemWindow (GetId (), surface->GetId ()) : nullptr;
    if (webView != nullptr && webView->Start (surfaceWindow, html))
        webView->Focus ();
    else
        GraphEditorLog ("WebView2 child host did not start");
}

void GraphEditorPalette::PanelCloseRequested (const DG::PanelCloseRequestEvent&, bool* accepted)
{
    Hide ();
    *accepted = true;
}

void GraphEditorPalette::PanelMoved (const DG::PanelMoveEvent&)
{
    if (webView != nullptr)
        webView->RefreshNativeTelemetry ();
}

void GraphEditorPalette::PanelResized (const DG::PanelResizeEvent& ev)
{
    BeginMoveResizeItems ();
    surface->Resize (ev.GetHorizontalChange (), ev.GetVerticalChange ());
    EndMoveResizeItems ();
    ResizeWebView ();
}

void GraphEditorPalette::ResizeWebView ()
{
    if (webView != nullptr)
        webView->Resize ();
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
