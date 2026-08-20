#ifndef GEOMETRYSERVER_PALETTE_CATALOGBROWSER_HPP
#define GEOMETRYSERVER_PALETTE_CATALOGBROWSER_HPP

// A modal "choose one item from a folder tree" browser, behind the
// evp.LibraryPart and evp.Favourite parameters.
//
// THREE PANES, because that is what the thing it imitates has: a search line
// across the top, a FOLDER TREE on the left, and the selected folder's CONTENTS
// on the right, with a draggable divider between them. Archicad's own Object
// Settings browser is laid out exactly this way, and the first cut — one tree
// holding folders and objects together — was reported as unusable next to it:
// every object sat inside a folder of its own, and there was nowhere for a
// preview to go.
//
// ⚠️ FOLDERS LEFT, ITEMS RIGHT, AND NEVER BOTH IN ONE PANE. The split is what
// makes a catalogue of thousands navigable: the left pane stays short enough to
// scan because it holds only folders, and the right pane only ever shows one
// folder's worth. It is also the seam a thumbnail grid drops into later without
// touching the tree.
//
// It is NOT a merge with NavigatorBrowser. That dialog owns things this has no
// use for (three navigator maps behind a tab strip, reveal-the-current-guid, a
// per-map hit count) and has no second pane; folding two dialogs into one
// configurable dialog to save a file is how a 200-line dialog becomes a 700-line
// one.
//
// Modal, and run from a button click on the main thread — like the FilePath
// Browse button and the Navigator browser. It must NEVER be reached through
// MainThreadGate::Invoke: a modal holds for human time and would report a false
// timeout (see the gate's contract).

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "DGModule.hpp"
#include "NativeImage.hpp"

#include <functional>
#include <memory>

namespace evp {

// One selectable item. The caller builds these from whatever it is browsing and
// keeps its own parallel data; this dialog hands back an INDEX into the array it
// was given, never a reconstructed value.
struct CatalogRow {
    GS::UniString label;            // what the row reads
    GS::UniString detail;           // the status line while it is selected
    GS::Array<GS::UniString> group; // folder path above it; empty == top level
    GS::UniString tag;              // also searchable ("Object", "Wall", …)

    // Which physical package shipped this row ("Chairs.libpack"). Shown as one
    // extra tree level ONLY under "Folder View (with sources)" — the catalogue
    // always arrives in plain Folder View, because the source prefix is what made
    // one "Object Library" look like dozens of unrelated roots.
    GS::UniString source;

    // What the preview loader is asked for. Empty means "this kind of row has no
    // thumbnail" — favourites, today — and the grid then draws a name-only cell
    // rather than the caller having to supply a blank image.
    GS::UniString previewKey;
};

// Fetches one row's thumbnail. Returns false when there is none to show, which
// is the COMMON case and not a failure: a stock Archicad library ships parts with
// PNG previews, parts with TIFF previews (undecodable — NativeImage has JPEG and
// PNG only) and parts with no preview section at all.
//
// ⚠️ CALLED ONLY FOR CELLS ABOUT TO BE DRAWN. It touches the library file, so a
// catalogue of 4,676 objects must never be walked through it.
using CatalogPreviewLoader = std::function<bool (const GS::UniString& key, NewDisplay::NativeImage& image)>;

// How the contents pane presents a folder. Mirrors Archicad's own gear menu,
// which is where a user learned these words.
enum class CatalogViewMode { List, MediumIcons, LargeIcons };

class CatalogBrowser : public DG::ModalDialog,
                       public DG::PanelObserver,
                       public DG::ButtonItemObserver,
                       public DG::SearchEditObserver,
                       public DG::TreeViewObserver,
                       public DG::ListBoxObserver,
                       public DG::SplitterObserver,
                       public DG::UserItemObserver,
                       public DG::PopUpObserver,
                       public DG::CompoundItemObserver {
  public:
    // `rows` is BORROWED and must outlive the dialog — it is normally the
    // caller's own catalogue, which it needs anyway to build the chosen value.
    // `currentRow` preselects the row the parameter already holds (-1 for none).
    // `loadPreview` may be empty, and then the icon views draw name-only cells.
    CatalogBrowser (const GS::UniString& title, const GS::Array<CatalogRow>& rows, const GS::UniString& emptyMessage,
                    Int32 currentRow, CatalogPreviewLoader loadPreview = nullptr);
    ~CatalogBrowser ();

    // Valid only after Invoke() returned Accept. -1 means nothing was chosen.
    Int32 GetSelectedRow () const
    {
        return selectedRow;
    }

  private:
    virtual void ButtonClicked (const DG::ButtonClickEvent& ev) override;
    virtual void PanelResized (const DG::PanelResizeEvent& ev) override;

    // ⚠️ A .grc TextEdit would NOT do: TextEditChanged fires on COMMIT, which is
    // what once made the palette's own search box look dead. DG::SearchEdit
    // reports every keystroke. Same reason NavigatorBrowser builds its own.
    virtual void SearchTextChanged (const DG::SearchEditChangeEvent& ev) override;

    virtual void TreeViewSelectionChanged (const DG::TreeViewSelectionEvent& ev) override;
    virtual void ListBoxSelectionChanged (const DG::ListBoxSelectionEvent& ev) override;
    virtual void ListBoxDoubleClicked (const DG::ListBoxDoubleClickEvent& ev) override;

    // The divider. Same two-step as the palette's own splitters: the DRAG moves
    // only the bar (a full relayout per mouse-move is visible churn), and the
    // panes follow once on release.
    virtual void SplitterDragged (const DG::SplitterDragEvent& ev) override;
    virtual void SplitterDragExited (const DG::SplitterDragEvent& ev) override;

    // The icon grid. Drawing follows the DevKit's own preview-image pattern
    // (Favorite_Test/UserItemDialog): a DG::UserItem plus a
    // NewDisplay::UserItemUpdateNativeContext to blit into.
    virtual void UserItemUpdate (const DG::UserItemUpdateEvent& ev) override;

    // The gear menu: Archicad's own wording, because that is where the user
    // learned it — Folder View / Folder View (with sources), then List / Medium
    // Icons / Large Icons.
    virtual void PopUpChanged (const DG::PopUpChangeEvent& ev) override;

    // Rebuild the left pane for the current filter. Affordable per keystroke
    // because the DATA was collected once by the caller — only DG items are
    // rebuilt here, never the catalogue.
    void PopulateTree ();
    // Rebuild the right pane for the folder the tree has selected: its
    // subfolders first, then its items.
    void PopulateContents ();
    void MarkMatches ();
    void SyncSelection ();
    void AcceptIfChosen ();
    void LayoutPanes ();

    // How tall the preview pane is in the current view mode; 0 hides it.
    short PreviewHeight () const;

    // The thumbnail for one contents slot, decoded on first sight and kept.
    // ⚠️ CACHED BY SLOT, and the cache is dropped whenever the contents change:
    // a slot index means nothing once the folder does.
    const NewDisplay::NativeImage* PreviewForSlot (UIndex slot);

    // The folder path a tree item stands for, joined with '\n' — the same key
    // PopulateTree builds its folder map from. Empty for the root.
    GS::UniString FolderKeyOf (Int32 treeItem) const;

    const GS::Array<CatalogRow>& rows; // BORROWED — see the ctor
    GS::UniString emptyMessage;        // shown when the catalogue is empty
    GS::Array<bool> matches;           // parallel to `rows`

    // Every folder key the surviving rows imply, in tree order, and the DG item
    // built for each. Parallel arrays rather than a map so the tree item can
    // carry an INDEX into them — a DGUserData cannot hold a string.
    GS::Array<GS::UniString> folderKeys;
    GS::Array<Int32> folderItems;

    // What the right pane currently lists: a row index (>= 0) for an item, or
    // ~folderIndex for a subfolder, so one array answers both without a second
    // parallel "is this a folder" list that could fall out of step.
    GS::Array<Int32> contentsTargets;

    // Where each row is filed FOR THE CURRENT VIEW. Plain Folder View and
    // "with sources" put the same row under different paths, and PopulateContents
    // must agree with whatever PopulateTree just built rather than re-deriving it.
    GS::Array<GS::Array<GS::UniString>> groupOfRow;

    Int32 selectedRow = -1;
    Int32 openRow = -1; // what to reveal when the dialog opens

    // Where the divider sits, as an x in dialog coordinates. Kept as state
    // because a drag has to survive the relayout that follows it.
    short dividerX = 0;

    CatalogViewMode viewMode = CatalogViewMode::MediumIcons;

    // Folder View vs Folder View (with sources), as Archicad's gear menu names
    // them. The catalogue always ARRIVES in Folder View — the source containers
    // were dropped in the collector, which is where the tree is decided — so
    // "with sources" adds the library the part came from as one extra level
    // rather than re-deriving a second path.
    bool showSources = false;

    CatalogPreviewLoader loadPreview;
    // Parallel to contentsTargets. A slot's thumbnail, decoded once; `tried`
    // separates "no image" from "not looked at yet", so a part with no preview
    // is not re-read on every repaint.
    GS::Array<NewDisplay::NativeImage> previews;
    GS::Array<bool> previewTried;

    Int32 selectedSlot = -1; // the contents slot whose preview is shown, -1 for none

    DG::SingleSelTreeView tree;
    DG::SingleSelListBox contents;
    DG::LeftText searchLabel;
    DG::LeftText statusText;
    DG::Button cancelButton;
    DG::Button okButton;

    std::unique_ptr<DG::SearchEdit> searchField;
    std::unique_ptr<DG::Splitter> divider;
    std::unique_ptr<DG::UserItem> preview;

    // ⚠️ A DG::PopUp, NOT a gear button opening a floating menu. DG has no
    // add-on-facing popup-menu API — the whole ACAPI_Dialog_* surface was
    // grepped for evp.View and holds no such thing — so the honest imitation of
    // Archicad's gear is a compact dropdown carrying the same entries in the
    // same order and the same words.
    std::unique_ptr<DG::PopUp> viewMenu;
};

} // namespace evp

#endif
