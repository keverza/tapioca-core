#include "APIEnvir.h"
#include "ACAPinc.h"

#include "Palette/CatalogPicker.hpp"
#include "Palette/CatalogBrowser.hpp"               // the modal list this drives
#include "Palette/ParamPanel.hpp"                   // ParamControl — what a picker writes into
#include "Palette/ParamValues.hpp"                  // EscapeJson — the value goes out as JSON
#include "NativeCommands/FavoriteCommands.hpp"      // CollectFavorites — the read ListFavorites does
#include "NativeCommands/LibraryObjectCommands.hpp" // CollectLibraryParts — likewise
#include "Python/PathUtils.hpp"                     // AppendTextLine / ScanLogPath — a refusal must be reportable

namespace evp {

// evp.LibraryPart / evp.Favourite — collect, browse, and store the chosen row.
//
// The catalogue is read STRAIGHT FROM ACAPI here rather than round-tripped
// through Tapioca.ListLibraryParts / Tapioca.ListFavorites, for the reason the
// Story picker reads ACAPI_ProjectSetting_GetStorySettings directly: a button
// click is already on the main thread, and making the palette call its own
// add-on's bus to fill its own list would pay the dispatcher for nothing. The
// native commands exist so a COMMAND can ask the same question; both sides call
// the same collector, so the two lists can never disagree.
void OpenCatalogBrowser (ParamControl& pc)
{
    const bool favourites = (pc.type == "Favourite");
    GS::Array<CatalogRow> catalog;

    // Kept alongside the rows so the chosen INDEX can be turned back into a
    // value. CatalogBrowser deliberately returns an index and never a rebuilt
    // value: two rows that read the same must still be two different answers.
    GS::Array<geomsrv::LibraryPartEntry> parts;
    GS::Array<geomsrv::FavoriteEntry> favorites;

    GS::UniString title, emptyMessage, refusal;
    GSErrCode err = NoError;

    if (favourites) {
        title = "Choose a favourite";
        emptyMessage = "(this project has no favourites)";
        err = geomsrv::CollectFavorites (pc.catalogFilter, favorites);
        for (const geomsrv::FavoriteEntry& favorite : favorites) {
            CatalogRow row;
            row.label = favorite.name;
            row.group = favorite.folder;
            row.tag = favorite.elementType;
            row.detail =
                GS::UniString::Printf ("%T  -  %T", favorite.name.ToPrintf (), favorite.elementType.ToPrintf ());
            catalog.Push (row);
        }
    }
    else {
        title = "Choose a library object";
        emptyMessage = "(this project has no placeable library objects)";
        err = geomsrv::CollectLibraryParts (pc.catalogFilter, parts, refusal);
        for (const geomsrv::LibraryPartEntry& part : parts) {
            CatalogRow row;
            row.label = part.name;
            row.tag = part.type;
            row.source = part.library;  // shown only under "Folder View (with sources)"
            row.previewKey = part.name; // what the thumbnail loader resolves
            // ⚠️ GROUPED BY THE LIBRARY MANAGER'S OWN TREE, not by kind. The first
            // cut grouped by typeID and the report was that the browser did not
            // look like anything in Archicad: the structure a user navigates is
            // Embedded Library / Loaded Libraries / <library> / <folders>, which
            // is what `treePath` carries. An empty path means the modern lookup
            // declined for that part; it then sits at the top level, which is
            // honest — better than a folder invented for it.
            row.group = part.treePath;
            // The status line carries what the row cannot: which .gsm this is, and
            // whether it can be placed at all. A part whose file is missing is
            // still listed — hiding it would read as "the library does not have
            // it", which sends the user looking in the wrong place.
            row.detail = part.missing ? GS::UniString::Printf ("%T  -  DEFINITION MISSING (%T)", part.name.ToPrintf (),
                                                               part.file.ToPrintf ())
                                      : GS::UniString::Printf ("%T  -  %T", part.name.ToPrintf (),
                                                               part.location.IsEmpty () ? part.file.ToPrintf ()
                                                                                        : part.location.ToPrintf ());
            catalog.Push (row);
        }
    }

    // ⚠️ A REFUSAL IS NOT AN EMPTY CATALOGUE. `subtype="Door"` is out of scope
    // for this picker (an opening needs its host's context), and a browser that
    // merely opened empty would read as "this project has no doors" — sending the
    // user to look in the library for something that was never going to be there.
    // Say what was refused, in the box itself.
    if (!refusal.IsEmpty ()) {
        AppendTextLine (ScanLogPath (), GS::UniString::Printf ("  %T '%T': %T", pc.type.ToPrintf (),
                                                               pc.name.ToPrintf (), refusal.ToPrintf ()));
        emptyMessage = refusal;
    }
    else if (err != NoError) {
        AppendTextLine (ScanLogPath (),
                        GS::UniString::Printf ("  %T '%T': the catalogue could not be read (error %d) - "
                                               "the browser is showing what was collected before it failed.",
                                               pc.type.ToPrintf (), pc.name.ToPrintf (), (int) err));
        if (catalog.IsEmpty ())
            emptyMessage = GS::UniString::Printf ("(the catalogue could not be read - error %d)", (int) err);
    }

    // Reopen on the row the parameter already holds. Matched on the LABEL, which
    // is all that survives in the button, and only used to place the highlight —
    // a stale match therefore costs a scroll position, never a wrong value.
    Int32 currentRow = -1;
    for (UIndex i = 0; i < catalog.GetSize () && currentRow < 0; ++i) {
        if (!pc.selectedLabel.IsEmpty () && catalog[i].label == pc.selectedLabel)
            currentRow = (Int32) i;
    }

    // Thumbnails for library objects only. A favourite HAS a preview image in the
    // API (Favorite::GetPreviewImage) but it is a different call with a different
    // signature, so it stays phase 2 rather than being half-wired here — passing
    // nothing leaves the preview pane blank for favourites, which is honest.
    CatalogPreviewLoader previewLoader;
    if (!favourites) {
        previewLoader = [] (const GS::UniString& key, NewDisplay::NativeImage& image) {
            return geomsrv::LoadLibraryPartPreview (key, image);
        };
    }

    CatalogBrowser browser (title, catalog, emptyMessage, currentRow, previewLoader);
    if (browser.Invoke () != DG::ModalDialog::Accept)
        return; // cancelled: keep whatever the row already held

    const Int32 chosen = browser.GetSelectedRow ();
    if (chosen < 0 || (UIndex) chosen >= catalog.GetSize ())
        return;

    // ⚠️ VALUE AND LABEL MOVE TOGETHER OR NOT AT ALL, exactly as for evp.View. A
    // button showing one part's name while sending another part's unID is the
    // failure with no symptom.
    if (favourites) {
        const geomsrv::FavoriteEntry& favorite = favorites[(UIndex) chosen];
        GS::UniString folderJson ("[");
        for (UIndex i = 0; i < favorite.folder.GetSize (); ++i) {
            if (i > 0)
                folderJson += ",";
            folderJson += "\"" + EscapeJson (favorite.folder[i]) + "\"";
        }
        folderJson += "]";
        pc.selectedValueJson = "{\"name\":\"" + EscapeJson (favorite.name) + "\",\"elementType\":\"" +
                               EscapeJson (favorite.elementType) + "\",\"folder\":" + folderJson + "}";
        pc.selectedLabel = favorite.name;
    }
    else {
        const geomsrv::LibraryPartEntry& part = parts[(UIndex) chosen];
        pc.selectedValueJson = "{\"name\":\"" + EscapeJson (part.name) + "\",\"unID\":\"" + EscapeJson (part.unID) +
                               "\",\"type\":\"" + EscapeJson (part.type) + "\",\"file\":\"" + EscapeJson (part.file) +
                               "\",\"location\":\"" + EscapeJson (part.location) + "\"}";
        pc.selectedLabel = part.name;
    }

    static_cast<DG::Button*> (pc.control.get ())->SetText (pc.selectedLabel);
}

} // namespace evp
