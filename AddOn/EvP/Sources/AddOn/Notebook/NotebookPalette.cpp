#include "Notebook/NotebookPalette.hpp"

#include "Python/PathUtils.hpp"
#include "ResourceIds.hpp"

#include <cstring>
#include <string>

const GS::Guid NotebookPalette::paletteGuid ("{7B6F7484-238D-4D79-A4C9-4175FAD8869E}");
GS::Ref<NotebookPalette> NotebookPalette::instance;

namespace {

const GS::UniString kResourceFallbackHtml (
    "<!doctype html><html><head><meta charset=\"utf-8\"><style>"
    "html,body{height:100%;margin:0}body{display:grid;place-items:center;background:#f3f4f6;"
    "color:#202124;font:14px system-ui,sans-serif}main{max-width:360px;padding:24px}"
    "h1{font-size:18px;margin:0 0 10px}p{line-height:1.5;margin:0;color:#5f6368;user-select:text}"
    "@media(prefers-color-scheme:dark){body{background:#252629;color:#f1f3f4}p{color:#bdc1c6}}"
    "</style></head><body><main><h1>Tapioca Notebook could not load</h1>"
    "<p>Build the add-on again, then check %LOCALAPPDATA%\\Tapioca\\logs\\startup.log "
    "for the Notebook resource error.</p></main></body></html>");

void NotebookLog (const GS::UniString& message)
{
    const GS::UniString dataDir = evp::EvpDataDir ();
    if (dataDir.IsEmpty ())
        return;

    const GS::UniString logsDir = dataDir + GS::UniString ("\\logs");
    evp::CreateDirectoryChain (logsDir);
    evp::AppendTextLine (logsDir + GS::UniString ("\\startup.log"),
                         GS::UniString ("Notebook: ") + message);
}

GS::UniString LoadNotebookHtmlResource ()
{
    GSHandle data = RSLoadResource ('DATA', ACAPI_GetOwnResModule (), NotebookHtmlResId);
    if (data == nullptr) {
        NotebookLog (GS::UniString::Printf ("HTML resource %d is missing", NotebookHtmlResId));
        return GS::EmptyUniString;
    }

    const GSSize handleSize = BMhGetSize (data);
    if (handleSize <= 0) {
        NotebookLog (GS::UniString::Printf ("HTML resource %d has invalid size %d",
                                            NotebookHtmlResId,
                                            (int) handleSize));
        BMhKill (&data);
        return GS::EmptyUniString;
    }

    const std::string bytes (*data, *data + handleSize);
    BMhKill (&data);
    if (bytes.find ('\0') != std::string::npos) {
        NotebookLog ("HTML resource contains an embedded NUL byte");
        return GS::EmptyUniString;
    }

    const GS::UniString html (bytes.c_str (), CC_UTF8);
    const auto roundTrip = html.ToCStr (0, GS::MaxUSize, CC_UTF8);
    if (std::strlen (roundTrip.Get ()) != bytes.size () ||
        std::memcmp (roundTrip.Get (), bytes.data (), bytes.size ()) != 0) {
        NotebookLog ("HTML resource is not valid UTF-8");
        return GS::EmptyUniString;
    }
    if (bytes.find ("<!doctype html>") == std::string::npos ||
        bytes.find ("id=\"app\"") == std::string::npos) {
        NotebookLog ("HTML resource is UTF-8 but is not the expected notebook document");
        return GS::EmptyUniString;
    }

    NotebookLog (GS::UniString::Printf ("loaded HTML resource %d with %u bytes",
                                        NotebookHtmlResId,
                                        (unsigned int) handleSize));
    return html;
}

}   // namespace

NotebookPalette::NotebookPalette () :
    DG::Palette (ACAPI_GetOwnResModule (), NotebookPaletteResId,
                 ACAPI_GetOwnResModule (), paletteGuid),
    browser (GetReference (), NotebookBrowserId)
{
    NotebookLog ("constructor entered");
    Attach (*this);
    BeginEventProcessing ();
    NotebookLog ("DG event processing started");

    browser.SetContextMenuMode (DG::BrowserBase::ContextMenuMode::Disabled);
    browser.SetScrollBarVisibility (true);
    NotebookLog ("browser settings applied; navigation remains enabled for initial LoadHTML");

    browser.onLoadingStateChange += [] (const DG::BrowserBase&,
                                        const DG::BrowserLoadingStateChangeArg& event) {
        NotebookLog (event.isLoading ? "browser load started" : "browser load finished");
        if (!event.isLoading && NotebookPalette::HasInstance ()) {
            NotebookPalette& palette = NotebookPalette::GetInstance ();
            const bool accepted = palette.browser.ExecuteJS (
                "document.documentElement.dataset.tapiocaHost='ready'");
            NotebookLog (accepted ? "post-load JavaScript accepted" :
                                    "post-load JavaScript rejected");
            palette.browser.DisableNavigation (true);
            NotebookLog ("browser navigation disabled after load");
        }
    };
    browser.onLoadError += [] (const DG::BrowserBase&, const DG::BrowserLoadErrorArg& event) {
        NotebookLog (GS::UniString::Printf ("browser load error %d at %T",
                                            (int) event.errorCode,
                                            event.url.ToPrintf ()));
    };
    browser.onUrlChanged += [] (const DG::BrowserBase&, const DG::BrowserURLChangeArg& event) {
        NotebookLog (GS::UniString ("browser URL changed: ") + event.newURL);
    };
    NotebookLog ("browser diagnostics attached; waiting for palette show");
}

NotebookPalette::~NotebookPalette ()
{
    NotebookLog ("destructor entered");
    EndEventProcessing ();
    NotebookLog ("DG event processing stopped");
}

bool NotebookPalette::HasInstance ()
{
    return instance != nullptr;
}

void NotebookPalette::CreateInstance ()
{
    if (!HasInstance ()) {
        NotebookLog ("creating palette instance");
        instance = new NotebookPalette ();
        ACAPI_KeepInMemory (true);
        NotebookLog ("palette instance created; add-on kept in memory");
    }
}

NotebookPalette& NotebookPalette::GetInstance ()
{
    return *instance;
}

void NotebookPalette::DestroyInstance ()
{
    NotebookLog ("destroying palette instance");
    instance = nullptr;
    NotebookLog ("palette instance destroyed");
}

void NotebookPalette::Open ()
{
    if (!HasInstance ())
        CreateInstance ();
    GetInstance ().Show ();
    GetInstance ().LoadNotebookPage ();
}

void NotebookPalette::Show ()
{
    NotebookLog ("show requested");
    DG::Palette::Show ();
    NotebookLog ("palette shown");
}

void NotebookPalette::Hide ()
{
    NotebookLog ("hide requested");
    DG::Palette::Hide ();
    NotebookLog ("palette hidden");
}

void NotebookPalette::LoadNotebookPage ()
{
    browser.DisableNavigation (false);
    NotebookLog ("browser navigation enabled for LoadHTML");
    GS::UniString html = LoadNotebookHtmlResource ();
    if (html.IsEmpty ()) {
        NotebookLog ("loading in-browser resource failure page");
        html = kResourceFallbackHtml;
    }
    NotebookLog (GS::UniString::Printf ("calling LoadHTML with %u UTF-16 code units",
                                        (unsigned int) html.GetLength ()));
    browser.LoadHTML (html);
    NotebookLog ("LoadHTML returned");
}

void NotebookPalette::PanelCloseRequested (const DG::PanelCloseRequestEvent&, bool* accepted)
{
    NotebookLog ("close requested");
    Hide ();
    *accepted = true;
}

void NotebookPalette::PanelResized (const DG::PanelResizeEvent& ev)
{
    BeginMoveResizeItems ();
    browser.Resize (ev.GetHorizontalChange (), ev.GetVerticalChange ());
    EndMoveResizeItems ();
}

GSErrCode NotebookPalette::PaletteControlCallBack (Int32,
                                                   API_PaletteMessageID messageId,
                                                   GS::IntPtr param)
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
            *(reinterpret_cast<API_PaletteDeactivationMethod*> (param)) =
                APIPaletteDeactivationMethod_Default;
            break;
        default:
            break;
    }
    return NoError;
}

GSErrCode NotebookPalette::RegisterPaletteControlCallBack ()
{
    return ACAPI_RegisterModelessWindow (
        GS::CalculateHashValue (paletteGuid),
        PaletteControlCallBack,
        API_PalEnabled_FloorPlan + API_PalEnabled_Section + API_PalEnabled_Elevation +
        API_PalEnabled_InteriorElevation + API_PalEnabled_3D + API_PalEnabled_Detail +
        API_PalEnabled_Worksheet + API_PalEnabled_Layout + API_PalEnabled_DocumentFrom3D,
        GSGuid2APIGuid (paletteGuid));
}

GSErrCode NotebookPalette::UnregisterPaletteControlCallBack ()
{
    return ACAPI_UnregisterModelessWindow (GS::CalculateHashValue (paletteGuid));
}
