#include "Palette/GraphEditorPalette.hpp"

#include "NodeGraph/FaultBarrier.hpp"
#include "Palette/WebView2GraphHost.hpp"
#include "Python/PathUtils.hpp"
#include "ResourceIds.hpp"

#include "DGWin.h"

#include <cstdint>
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
    const HMODULE resourceModule = reinterpret_cast<HMODULE> (ACAPI_GetOwnResModule ());
    const HRSRC resource = FindResourceW (resourceModule, MAKEINTRESOURCEW (GraphEditorHtmlResId), L"DATA");
    if (resource == nullptr) {
        GraphEditorLog (GS::UniString::Printf ("HTML resource %d is missing", GraphEditorHtmlResId));
        return GS::EmptyUniString;
    }

    const DWORD resourceSize = SizeofResource (resourceModule, resource);
    const HGLOBAL loadedResource = LoadResource (resourceModule, resource);
    const void* resourceData = loadedResource != nullptr ? LockResource (loadedResource) : nullptr;
    if (resourceData == nullptr || resourceSize < sizeof (std::uint32_t)) {
        GraphEditorLog (GS::UniString::Printf ("HTML resource %d could not be read", GraphEditorHtmlResId));
        return GS::EmptyUniString;
    }

    // ResConv prefixes DATA resources with their unpadded payload size. Reading
    // the resource directly avoids RSLoadResource's 1 MiB GSHandle ceiling.
    std::uint32_t payloadSize = 0;
    std::memcpy (&payloadSize, resourceData, sizeof (payloadSize));
    if (payloadSize == 0 || payloadSize > resourceSize - sizeof (payloadSize)) {
        GraphEditorLog (GS::UniString::Printf ("HTML resource %d has invalid size %u", GraphEditorHtmlResId,
                                               static_cast<unsigned int> (payloadSize)));
        return GS::EmptyUniString;
    }

    const char* payload = static_cast<const char*> (resourceData) + sizeof (payloadSize);
    const std::string bytes (payload, payload + payloadSize);
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
                                           static_cast<unsigned int> (payloadSize)));
    return html;
}

} // namespace

GraphEditorPalette::GraphEditorPalette ()
    : DG::Palette (ACAPI_GetOwnResModule (), GraphEditorPaletteResId, ACAPI_GetOwnResModule (), paletteGuid)
{
    // ⚠️ THE ITEMS ARE BUILT BEFORE EVENT PROCESSING STARTS, AND THE ORDER IS
    // THE BUG THIS PALETTE SPENT AN AFTERNOON ON.
    //
    // BeginEventProcessing makes DG start delivering panel events to the
    // observer this constructor just attached, and DG delivers a PanelResized
    // immediately whenever the restored palette geometry differs from the
    // resource default - which depends on where the user last left the window,
    // the monitor and the DPI, so it fires on some machines and sessions and not
    // others. `PanelResized` dereferences `surface`. With the surface created
    // AFTER this call, that was a null dereference: an access violation inside
    // BeginEventProcessing that took Archicad down with no Windows Error
    // Reporting entry and no Archicad crash report to point at it.
    //
    // WebUIPalette has the same Attach/BeginEventProcessing order and is fine,
    // which is what made this look like house style: its `browser` is a MEMBER
    // INITIALIZED IN THE INITIALISER LIST, so it exists before the body runs.
    // A unique_ptr built in the body does not.
    //
    // The rule: nothing an event handler touches may be constructed after the
    // event processing that can call it.
    GraphEditorLog ("constructor entered");
    surface = std::make_unique<DG::UserItem> (*this, DG::Rect (0, 0, GetClientWidth (), GetClientHeight ()),
                                              DG::UserItem::Normal, DG::UserItem::NoFrame);
    surface->Show ();
    webView = std::make_unique<WebView2GraphHost> ();
    GraphEditorLog ("constructor: items created");
    Attach (*this);
    BeginEventProcessing ();
    GraphEditorLog ("constructor: event processing begun");
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
    // ⚠️ THE PALETTE IS BEHIND THE FAULT BARRIER, NOT ONLY THE GRAPH RUNTIME.
    // ADR-007's third constraint is that a node-graph error must never take
    // Archicad down, and the containment was built for the runtime while the
    // window that HOSTS it had none - so a fault here was fatal no matter how
    // well guarded the evaluator was. `catch (...)` would not do: under /EHsc an
    // access violation is a structured exception and is not required to be
    // caught by it. A failure now leaves Archicad running and says so in
    // startup.log.
    const evp::nodegraph::GuardOutcome guarded = evp::nodegraph::RunGuarded ([] () {
        if (!HasInstance ())
            CreateInstance ();
        GetInstance ().Show ();
        GetInstance ().LoadGraphEditorPage ();
        return true;
    });
    if (!guarded.completed)
        GraphEditorLog (GS::UniString ("OPEN FAILED: ", CC_UTF8) + GS::UniString (guarded.fault.c_str (), CC_UTF8));
}

void GraphEditorPalette::Show ()
{
    GraphEditorLog ("show: entered");
    DG::Palette::Show ();
    GraphEditorLog ("show: palette shown");
    if (webView != nullptr) {
        webView->SetVisible (true);
        webView->Focus ();
    }
    GraphEditorLog ("show: complete");
}

void GraphEditorPalette::Hide ()
{
    if (webView != nullptr)
        webView->SetVisible (false);
    DG::Palette::Hide ();
}

void GraphEditorPalette::LoadGraphEditorPage ()
{
    GraphEditorLog ("page: loading the HTML resource");
    GS::UniString html = LoadGraphEditorHtmlResource ();
    if (html.IsEmpty ())
        html = kResourceFallbackHtml;
    GraphEditorLog ("page: resolving the surface HWND");
    const HWND surfaceWindow = surface != nullptr ? DGGetDialogItemWindow (GetId (), surface->GetId ()) : nullptr;
    GraphEditorLog ("page: starting the web view host");
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
    // Guarded as well as ordered. The ordering above is the fix; this is the
    // second line, because a handler that assumes an item exists is one
    // refactor away from being fatal again - and the other handlers here
    // already null-check what they touch.
    if (surface == nullptr)
        return;
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
