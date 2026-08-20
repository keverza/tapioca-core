#include "APIEnvir.h"
#include "ACAPinc.h"

#include "Palette/CatalogBrowser.hpp"
#include "Palette/CommandFilter.hpp" // SplitSearchTerms / ScoreCommand — DevKit-free, tested
#include "ResourceIds.hpp"

#include "DGNativeContexts.hpp" // UserItemUpdateNativeContext::DrawImage - the DevKit's own preview pattern

#include <string>

namespace evp {

namespace {

// The dialog's own layout, in the .grc's coordinates. Repeated here rather than
// read back off the items, for the reason NavigatorBrowser records: a temporary
// wrapper around a resource item is a lifetime question not worth taking on for
// a handful of numbers. Move an item in the .grc and move it here, same edit.
// NOT in PaletteMetrics.hpp — that header is the PALETTE's layout, and these
// have exactly one consumer.
constexpr short Margin = 12;
constexpr short SearchTop = 12;
constexpr short SearchHeight = 20;
constexpr short SearchLeft = 72; // right of the "Search:" label
constexpr short PanesTop = 44;
constexpr short StatusHeight = 16;
constexpr short ButtonHeight = 24;
constexpr short ButtonWidth = 108;
constexpr short ButtonGap = 8;
constexpr short BottomPad = 16;
constexpr short DividerWidth = 6;
constexpr short MinPaneWidth = 120;  // neither pane may be dragged out of existence
constexpr short ViewMenuWidth = 200; // the gear dropdown; fits "Folder View (with sources)" unclipped
constexpr short ViewMenuGap = 8;

// Cell geometry per view mode. The icon sizes are the DevKit preview pictures'
// own rough scale, and the label strip is two lines' worth so a long object name
// is not clipped to uselessness.
// How much of the right pane the picture takes, per view mode. List gives it
// none; the two icon modes are the same distinction Archicad's menu draws.
constexpr short MediumPreviewHeight = 160;
constexpr short LargePreviewHeight = 260;

// UTF-8 for the searcher, which is deliberately std::string-only.
std::string Utf8 (const GS::UniString& text)
{
    return std::string (text.ToCStr (0, GS::MaxUSize, CC_UTF8).Get ());
}

// A folder path as one key. '\n' rather than '/' or '\\' because a library or
// favourite folder may legitimately contain either, and two different paths that
// collided on their joined key would merge two folders into one.
GS::UniString FolderKey (const GS::Array<GS::UniString>& group, UIndex depth)
{
    GS::UniString key;
    for (UIndex i = 0; i < depth && i < group.GetSize (); ++i)
        key += "\n" + group[i];
    return key;
}

} // namespace

// ---------------------------------------------------------------------------
CatalogBrowser::CatalogBrowser (const GS::UniString& title, const GS::Array<CatalogRow>& rows,
                                const GS::UniString& emptyMessage, Int32 currentRow, CatalogPreviewLoader loadPreview)
    : DG::ModalDialog (ACAPI_GetOwnResModule (), CatalogBrowserResId, ACAPI_GetOwnResModule ()), rows (rows),
      emptyMessage (emptyMessage), openRow (currentRow), loadPreview (std::move (loadPreview)),
      tree (GetReference (), CatalogBrowserTreeId), contents (GetReference (), CatalogBrowserContentsId),
      searchLabel (GetReference (), CatalogBrowserSearchLabelId),
      statusText (GetReference (), CatalogBrowserStatusTextId),
      cancelButton (GetReference (), CatalogBrowserCancelButtonId), okButton (GetReference (), CatalogBrowserOkButtonId)
{
    AttachToAllItems (*this);
    Attach (*this);

    SetTitle (title); // "Choose a library object" / "Choose a favourite"

    matches.SetSize (rows.GetSize ());

    // A third of the width to the folders, which is the proportion Archicad's own
    // browser opens at and leaves room for names on both sides.
    dividerX = (short) (GetWidth () / 3);

    searchField = std::make_unique<DG::SearchEdit> (
        *this, DG::Rect (SearchLeft, SearchTop, (short) (GetWidth () - Margin), (short) (SearchTop + SearchHeight)));
    searchField->Attach (*this);
    searchField->Show ();
    searchField->SetFocus (); // open the browser, start typing

    divider = std::make_unique<DG::Splitter> (
        *this, DG::Rect (dividerX, PanesTop, (short) (dividerX + DividerWidth), (short) (PanesTop + 100)),
        DG::Splitter::Vertical, DG::Splitter::Normal);
    divider->Attach (*this);
    divider->EnableDrag ();
    divider->Show ();

    // ONE text column, for List mode. The icon modes draw into `grid` instead.
    contents.SetTabFieldCount (1);

    // The gear: Archicad's own entries, in Archicad's own order and wording, so
    // the menu reads as the one the user already knows. The two view families are
    // separated by a rule the way the real menu separates them.
    viewMenu = std::make_unique<DG::PopUp> (*this, DG::Rect (0, 0, ViewMenuWidth, SearchHeight), SearchHeight, 0);
    const char* entries[] = { "Folder View", "Folder View (with sources)", nullptr, "List", "Medium Icons",
                              "Large Icons" };
    for (short i = 0; i < (short) (sizeof (entries) / sizeof (entries[0])); ++i) {
        viewMenu->AppendItem ();
        if (entries[i] == nullptr)
            viewMenu->SetItemStatus (i + 1, false); // the separator, unselectable
        else
            viewMenu->SetItemText (i + 1, GS::UniString (entries[i]));
    }
    viewMenu->SelectItem (5); // Medium Icons
    viewMenu->Attach (*this);
    viewMenu->Show ();

    preview = std::make_unique<DG::UserItem> (*this, DG::Rect (0, 0, 10, 10));
    preview->Attach (*this);

    LayoutPanes ();
    PopulateTree ();
    PopulateContents ();
    SyncSelection ();
}

CatalogBrowser::~CatalogBrowser () = default;

// ---------------------------------------------------------------------------
// Filtering
// ---------------------------------------------------------------------------
//
// The search is the palette's own ranker (Palette/CommandFilter.hpp): DevKit-free
// and covered by the offline test suite, so the fuzzy behaviour a user already
// learned in the command list is the behaviour they get here too. A row maps onto
// SearchableCommand as title=label, category=tag, folder=its group path.
void CatalogBrowser::MarkMatches ()
{
    const std::vector<std::string> terms =
        SplitSearchTerms (Utf8 (searchField ? searchField->GetText () : GS::UniString ()));

    if (terms.empty ()) {
        for (UIndex i = 0; i < rows.GetSize (); ++i)
            matches[i] = true;
        return;
    }

    for (UIndex i = 0; i < rows.GetSize (); ++i) {
        GS::UniString path;
        for (const GS::UniString& part : rows[i].group) {
            if (!path.IsEmpty ())
                path += "/";
            path += part;
        }
        SearchableCommand searchable;
        searchable.title = Utf8 (rows[i].label);
        searchable.category = Utf8 (rows[i].tag);
        searchable.folder = Utf8 (path);
        matches[i] = ScoreCommand (searchable, terms) > 0;
    }
}

// The LEFT pane: folders only, built from the folder paths the surviving rows
// imply. A folder exists here only because a row that survived the filter lives
// somewhere under it, so an empty branch can never appear.
void CatalogBrowser::PopulateTree ()
{
    MarkMatches ();

    // One repaint, not one per item — the same reason the command list disables
    // draw around its rebuild.
    tree.DisableDraw ();
    tree.DeleteItem (DG::TreeView::AllItems);
    folderKeys.Clear ();
    folderItems.Clear ();
    // The path each row is filed under FOR THE CURRENT VIEW — plain Folder View
    // and "with sources" put the same row in different places, and PopulateContents
    // has to agree with whatever the tree just built.
    groupOfRow.SetSize (rows.GetSize ());

    Int32 reveal = 0;
    for (UIndex i = 0; i < rows.GetSize (); ++i) {
        if (!matches[i])
            continue;

        // "Folder View (with sources)" is the ONLY place the package shows. The
        // catalogue always arrives without it, because the source prefix is what
        // made a single "Object Library" look like dozens of unrelated roots.
        GS::Array<GS::UniString> group;
        if (showSources && !rows[i].source.IsEmpty ()) {
            group.Push (rows[i].group.IsEmpty () ? GS::UniString ("Loaded Libraries") : rows[i].group[0]);
            group.Push (rows[i].source);
            for (UIndex d = 1; d < rows[i].group.GetSize (); ++d)
                group.Push (rows[i].group[d]);
        }
        else {
            group = rows[i].group;
        }
        groupOfRow[i] = group;

        Int32 parentItem = (Int32) DG::TreeView::RootItem;
        for (UIndex depth = 0; depth < group.GetSize (); ++depth) {
            const GS::UniString key = FolderKey (group, depth + 1);

            UIndex existing = folderKeys.GetSize ();
            for (UIndex f = 0; f < folderKeys.GetSize (); ++f) {
                if (folderKeys[f] == key) {
                    existing = f;
                    break;
                }
            }
            if (existing < folderKeys.GetSize ()) {
                parentItem = folderItems[existing];
                continue;
            }

            const Int32 item = tree.AppendItem (parentItem);
            tree.SetItemText (item, group[depth]);
            // ⚠️ THE ITEM CARRIES ITS INDEX INTO folderKeys, written WITH the text
            // so no exit can leave an item pointing at the wrong folder. A
            // DGUserData cannot hold the key string itself, which is why the two
            // arrays exist at all.
            tree.SetItemValue (item, (DGUserData) folderKeys.GetSize ());
            folderKeys.Push (key);
            folderItems.Push (item);
            parentItem = item;

            if (openRow == (Int32) i)
                reveal = item;
        }
    }

    // Everything one level down is open, so the browser lands on a list of
    // libraries rather than on a single collapsed root.
    for (UIndex f = 0; f < folderItems.GetSize (); ++f) {
        if (tree.GetItem (folderItems[f], DG::TreeView::Parent) <= 0)
            tree.ExpandItem (folderItems[f]);
    }

    // While a search is running, open everything that survived: a hit three
    // folders deep that you cannot see is a search that looks broken.
    if (!SplitSearchTerms (Utf8 (searchField->GetText ())).empty ()) {
        for (UIndex f = 0; f < folderItems.GetSize (); ++f)
            tree.ExpandItem (folderItems[f]);
    }

    // Reopen where the user left off — expand every ancestor, then select. AFTER
    // the expand pass so it cannot be undone by it.
    if (reveal != 0) {
        for (Int32 up = tree.GetItem (reveal, DG::TreeView::Parent); up > 0;
             up = tree.GetItem (up, DG::TreeView::Parent))
            tree.ExpandItem (up);
        tree.SelectItem (reveal);
    }
    else if (!folderItems.IsEmpty () && tree.GetSelectedItem () == 0) {
        // Never open on nothing selected: an empty right pane beside a full tree
        // reads as a browser that failed to load.
        tree.SelectItem (folderItems[0]);
    }

    tree.EnableDraw ();
    tree.Redraw ();
}

GS::UniString CatalogBrowser::FolderKeyOf (Int32 treeItem) const
{
    if (treeItem == 0)
        return GS::UniString ();
    const Int32 index = (Int32) tree.GetItemValue (treeItem);
    if (index < 0 || (UIndex) index >= folderKeys.GetSize ())
        return GS::UniString ();
    return folderKeys[(UIndex) index];
}

// The RIGHT pane: what is directly inside the selected folder — its immediate
// subfolders first, then its own items. Never anything deeper: a contents pane
// that flattened the whole subtree would be the flat list of thousands this
// layout exists to avoid.
void CatalogBrowser::PopulateContents ()
{
    contents.DisableDraw ();
    contents.DeleteItem (DG::ListBox::AllItems);
    contentsTargets.Clear ();
    // ⚠️ THE PREVIEW CACHE IS KEYED BY SLOT, so it dies with the slots. Keeping it
    // across a folder change would show the previous folder's thumbnails under
    // this folder's names.
    previews.Clear ();
    previewTried.Clear ();
    selectedSlot = -1;

    const GS::UniString selectedKey = FolderKeyOf (tree.GetSelectedItem ());

    // Subfolders: a folder key one level longer than the selected one, which is
    // exactly a key that starts with it and has one more '\n'.
    for (UIndex f = 0; f < folderKeys.GetSize (); ++f) {
        const GS::UniString& key = folderKeys[f];
        if (key.GetLength () <= selectedKey.GetLength () || !key.BeginsWith (selectedKey))
            continue;
        const GS::UniString tail =
            key.GetSubstring (selectedKey.GetLength (), key.GetLength () - selectedKey.GetLength ());
        if (tail.Count ('\n') != 1)
            continue; // a grandchild, not a child

        contents.AppendItem ();
        const short listItem = contents.GetItemCount ();
        // A folder reads as one, because the pane mixes the two and a name alone
        // does not say which is which.
        contents.SetTabItemText (listItem, 1,
                                 GS::UniString ("[ ") + tail.GetSubstring (1, tail.GetLength () - 1) + " ]");
        contents.SetItemValue (listItem, (DGUserData) contentsTargets.GetSize ());
        contentsTargets.Push (~(Int32) f); // ~index marks a folder; see the header
    }

    // Then the items whose folder IS the selected one.
    for (UIndex i = 0; i < rows.GetSize (); ++i) {
        if (!matches[i] || FolderKey (groupOfRow[i], groupOfRow[i].GetSize ()) != selectedKey)
            continue;
        contents.AppendItem ();
        const short listItem = contents.GetItemCount ();
        contents.SetTabItemText (listItem, 1, rows[i].label);
        contents.SetItemValue (listItem, (DGUserData) contentsTargets.GetSize ());
        contentsTargets.Push ((Int32) i);
    }

    if (contentsTargets.IsEmpty ()) {
        // Never an empty box with no explanation. The three reasons for it —
        // nothing in the project, nothing matching the search, a folder that only
        // holds subfolders — need different reactions from the user.
        contents.AppendItem ();
        contents.SetTabItemText (contents.GetItemCount (), 1,
                                 rows.IsEmpty () ? emptyMessage
                                 : !SplitSearchTerms (Utf8 (searchField->GetText ())).empty ()
                                     ? GS::UniString ("(nothing here matches that search)")
                                     : GS::UniString ("(this folder holds no objects)"));
        contents.DisableItem (contents.GetItemCount ());
    }

    previews.SetSize (contentsTargets.GetSize ());
    previewTried.SetSize (contentsTargets.GetSize ());
    for (UIndex i = 0; i < previewTried.GetSize (); ++i)
        previewTried[i] = false;

    contents.EnableDraw ();
    contents.Redraw ();
    if (preview && preview->IsVisible ())
        preview->Redraw ();
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------
// Only the RIGHT pane can answer. Selecting a folder on the left changes what is
// listed; it is not a choice, and OK stays disabled until an item is picked —
// an enabled OK with a folder behind it is the bug this prevents.
void CatalogBrowser::SyncSelection ()
{
    selectedRow = -1;

    // Whichever pane is live. They are never both visible, so there is no
    // question of which one wins.
    Int32 slot = -1;
    const short listItem = contents.GetSelectedItem ();
    if (listItem > 0)
        slot = (Int32) contents.GetItemValue (listItem);
    if (slot >= 0 && (UIndex) slot < contentsTargets.GetSize () && contentsTargets[(UIndex) slot] >= 0)
        selectedRow = contentsTargets[(UIndex) slot];

    // The picture follows the highlight, so arrowing down the list flips through
    // the previews — which is the whole point of the pane.
    if (selectedSlot != slot) {
        selectedSlot = slot;
        if (preview && preview->IsVisible ())
            preview->Redraw ();
    }

    if (selectedRow < 0) {
        statusText.SetText ("Nothing selected");
        okButton.Disable ();
    }
    else {
        const CatalogRow& row = rows[(UIndex) selectedRow];
        statusText.SetText (row.detail.IsEmpty () ? row.label : row.detail);
        okButton.Enable ();
    }
}

void CatalogBrowser::AcceptIfChosen ()
{
    if (selectedRow >= 0)
        PostCloseRequest (DG::ModalDialog::Accept);
}

// ---------------------------------------------------------------------------
// The preview pane
// ---------------------------------------------------------------------------
short CatalogBrowser::PreviewHeight () const
{
    switch (viewMode) {
        case CatalogViewMode::LargeIcons:
            return LargePreviewHeight;
        case CatalogViewMode::MediumIcons:
            return MediumPreviewHeight;
        default:
            return 0; // List: no picture, all names
    }
}

//
// ⚠️ A PREVIEW PANE, NOT AN ICON GRID, AND THE REASON IS THE DRAWING API.
// A grid of captioned thumbnails needs three things from NewDisplay::NativeContext:
// DrawImage (verified, used below), text drawing, and hit-testing a click back to
// a cell. The text call is DrawUIText/DrawPlainText and BOTH require a TE::IFont
// the add-on has no source for yet, and the click position comes off an event
// whose accessors are equally unverified. Guessing at two unread APIs to draw a
// grid — with no way to run it — is how the last three rounds of this dialog were
// spent. So: the LIST keeps the names, selection and scrolling it already does
// correctly for free, and the pane below it shows the selected object's picture.
// Arrowing down the list flips through the previews, which is what "I need image
// preview to select object" actually asks for. The captioned grid is a follow-up
// once the font and mouse APIs are read properly.
const NewDisplay::NativeImage* CatalogBrowser::PreviewForSlot (UIndex slot)
{
    if (!loadPreview || slot >= contentsTargets.GetSize () || slot >= previewTried.GetSize ())
        return nullptr;
    const Int32 target = contentsTargets[slot];
    if (target < 0)
        return nullptr; // a subfolder row has no picture

    if (!previewTried[slot]) {
        // Set FIRST: a part with no preview, or one this cannot decode, must not
        // be re-read from its library file on every repaint.
        previewTried[slot] = true;
        NewDisplay::NativeImage image;
        if (loadPreview (rows[(UIndex) target].previewKey, image))
            previews[slot] = image;
    }
    // ⚠️ NativeImage has no IsEmpty/IsValid — it compares against nullptr, and
    // that is the only emptiness test it offers. A default-constructed one (the
    // "no preview" case, which is most of a stock library) reads as == nullptr.
    return previews[slot] != nullptr ? &previews[slot] : nullptr;
}

void CatalogBrowser::UserItemUpdate (const DG::UserItemUpdateEvent& ev)
{
    if (!preview || ev.GetSource () != preview.get ())
        return;

    // The DevKit's own preview-drawing pattern — Favorite_Test/UserItemDialog:
    // a native context over the update event, and DrawImage into it.
    NewDisplay::UserItemUpdateNativeContext context (ev);

    const float width = (float) preview->GetWidth ();
    const float height = (float) preview->GetHeight ();
    context.FillRect (0.0f, 0.0f, width, height, 255, 255, 255);

    const NewDisplay::NativeImage* image = selectedSlot >= 0 ? PreviewForSlot ((UIndex) selectedSlot) : nullptr;
    if (image == nullptr)
        return; // most of a stock library has no decodable preview; a blank pane is the honest answer

    // Fit, never stretch: a preview is not always square, and a distorted chair
    // is worse than a small one.
    const float imageWidth = (float) GS::Max (1u, image->GetWidth ());
    const float imageHeight = (float) GS::Max (1u, image->GetHeight ());
    const float scale = GS::Min (width / imageWidth, height / imageHeight);
    context.DrawImage (*image, scale, scale, 0.0f, (width - imageWidth * scale) / 2.0f,
                       (height - imageHeight * scale) / 2.0f, false);
}

// The gear. Archicad's menu rules a line between the two families; a DG popup
// cannot draw one, so that row is present and disabled — it reads the same and
// keeps the item numbers lined up with the real menu.
void CatalogBrowser::PopUpChanged (const DG::PopUpChangeEvent& ev)
{
    if (!viewMenu || ev.GetSource () != viewMenu.get ())
        return;

    switch (viewMenu->GetSelectedItem ()) {
        case 1:
            showSources = false;
            break;
        case 2:
            showSources = true;
            break;
        case 4:
            viewMode = CatalogViewMode::List;
            break;
        case 5:
            viewMode = CatalogViewMode::MediumIcons;
            break;
        case 6:
            viewMode = CatalogViewMode::LargeIcons;
            break;
        default:
            return; // the separator row
    }

    LayoutPanes ();
    PopulateTree ();
    PopulateContents ();
    SyncSelection ();
    Redraw ();
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
void CatalogBrowser::LayoutPanes ()
{
    const short right = (short) (GetWidth () - Margin);
    const short buttonTop = (short) (GetHeight () - BottomPad - ButtonHeight);
    const short statusTop = (short) (buttonTop - StatusHeight - ButtonGap);
    const short panesBottom = (short) (statusTop - ButtonGap);

    // The divider can never be dragged past the point where either pane stops
    // being usable — a browser with a zero-width tree cannot be recovered from
    // without reopening it.
    if (dividerX < Margin + MinPaneWidth)
        dividerX = (short) (Margin + MinPaneWidth);
    if (dividerX > right - MinPaneWidth - DividerWidth)
        dividerX = (short) (right - MinPaneWidth - DividerWidth);

    tree.SetRect (DG::Rect (Margin, PanesTop, dividerX, panesBottom));
    if (divider)
        divider->SetRect (DG::Rect (dividerX, PanesTop, (short) (dividerX + DividerWidth), panesBottom));

    const short contentsLeft = (short) (dividerX + DividerWidth);

    // The list keeps the whole right pane in List mode; the icon modes give its
    // lower part to the picture. The list is ALWAYS present — it is what carries
    // the names, the selection and the scrolling.
    const short previewHeight = PreviewHeight ();
    const short listBottom = previewHeight > 0 ? (short) (panesBottom - previewHeight - ButtonGap) : panesBottom;

    contents.SetRect (DG::Rect (contentsLeft, PanesTop, right, listBottom));
    if (preview) {
        if (previewHeight > 0) {
            preview->SetRect (DG::Rect (contentsLeft, (short) (listBottom + ButtonGap), right, panesBottom));
            preview->Show ();
        }
        else {
            preview->Hide ();
        }
    }

    // The gear sits at the right end of the search line, where Archicad's is.
    const short searchRight = (short) (right - ViewMenuWidth - ViewMenuGap);
    if (viewMenu)
        viewMenu->SetRect (
            DG::Rect ((short) (searchRight + ViewMenuGap), SearchTop, right, (short) (SearchTop + SearchHeight)));
    if (searchField)
        searchField->SetRect (DG::Rect (SearchLeft, SearchTop, searchRight, (short) (SearchTop + SearchHeight)));
    statusText.SetRect (DG::Rect (Margin, statusTop, right, (short) (statusTop + StatusHeight)));
    okButton.SetRect (DG::Rect ((short) (right - ButtonWidth), buttonTop, right, (short) (buttonTop + ButtonHeight)));
    cancelButton.SetRect (DG::Rect ((short) (right - 2 * ButtonWidth - ButtonGap), buttonTop,
                                    (short) (right - ButtonWidth - ButtonGap), (short) (buttonTop + ButtonHeight)));
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
void CatalogBrowser::ButtonClicked (const DG::ButtonClickEvent& ev)
{
    if (ev.GetSource () == &okButton) {
        AcceptIfChosen ();
    }
    else if (ev.GetSource () == &cancelButton) {
        // Cancel means CANCEL: drop whatever was highlighted, so the caller keeps
        // the value the parameter already had rather than the last thing clicked.
        selectedRow = -1;
        PostCloseRequest (DG::ModalDialog::Cancel);
    }
}

void CatalogBrowser::SearchTextChanged (const DG::SearchEditChangeEvent& /*ev*/)
{
    PopulateTree ();
    PopulateContents ();
    SyncSelection (); // the selected item may have just been filtered away
}

void CatalogBrowser::TreeViewSelectionChanged (const DG::TreeViewSelectionEvent& /*ev*/)
{
    PopulateContents ();
    SyncSelection ();
}

void CatalogBrowser::ListBoxSelectionChanged (const DG::ListBoxSelectionEvent& /*ev*/)
{
    SyncSelection ();
}

// Double-click on an ITEM is "choose this"; on a SUBFOLDER it navigates into it,
// which is the only way to reach a folder that the tree has collapsed.
void CatalogBrowser::ListBoxDoubleClicked (const DG::ListBoxDoubleClickEvent& /*ev*/)
{
    const short listItem = contents.GetSelectedItem ();
    if (listItem <= 0)
        return;
    const Int32 slot = (Int32) contents.GetItemValue (listItem);
    if (slot < 0 || (UIndex) slot >= contentsTargets.GetSize ())
        return;

    const Int32 target = contentsTargets[(UIndex) slot];
    if (target >= 0) {
        SyncSelection ();
        AcceptIfChosen ();
        return;
    }

    const UIndex folderIndex = (UIndex) ~target;
    if (folderIndex < folderItems.GetSize ()) {
        tree.ExpandItem (folderItems[folderIndex]);
        tree.SelectItem (folderItems[folderIndex]);
        PopulateContents ();
        SyncSelection ();
    }
}

// The drag moves ONLY the bar; the panes follow once on release. Same two-step
// as the palette's own splitters, and for the same reason: relaying out two
// scrolled panes on every mouse-move is visible churn.
void CatalogBrowser::SplitterDragged (const DG::SplitterDragEvent& ev)
{
    if (!divider || ev.GetSource () != divider.get ())
        return;
    const short right = (short) (GetWidth () - Margin);
    short x = (short) ev.GetPosition ();
    if (x < Margin + MinPaneWidth)
        x = (short) (Margin + MinPaneWidth);
    if (x > right - MinPaneWidth - DividerWidth)
        x = (short) (right - MinPaneWidth - DividerWidth);
    divider->SetRect (DG::Rect (x, PanesTop, (short) (x + DividerWidth), divider->GetRect ().GetBottom ()));
}

void CatalogBrowser::SplitterDragExited (const DG::SplitterDragEvent& ev)
{
    if (!divider || ev.GetSource () != divider.get ())
        return;
    dividerX = divider->GetRect ().GetLeft ();
    LayoutPanes ();
    Redraw ();
}

// Growable, because a long object name plus its folder needs the room. The
// divider keeps its DISTANCE FROM THE LEFT rather than its proportion: a user
// who sized the folder pane to fit their library names wants that size kept when
// the window grows, and the extra width belongs to the contents.
void CatalogBrowser::PanelResized (const DG::PanelResizeEvent& ev)
{
    LayoutPanes ();
    DG::PanelObserver::PanelResized (ev);
}

} // namespace evp
