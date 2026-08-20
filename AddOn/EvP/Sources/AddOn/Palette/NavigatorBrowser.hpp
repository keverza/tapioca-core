#ifndef GEOMETRYSERVER_PALETTE_NAVIGATORBROWSER_HPP
#define GEOMETRYSERVER_PALETTE_NAVIGATORBROWSER_HPP

// A modal "choose an item from the Navigator" browser, behind an evp.View
// parameter.
//
// WHY IT EXISTS. The first cut of evp.View was a flat popup of every placeable
// view with its folder path appended. It works, and on a real project it is
// unusable: the user's words were *"using current picker user would get lost on a
// larger project."* A project has hundreds of views; a flat list of hundreds is a
// list you scroll, not a list you choose from.
//
// ⚠️ THIS IS NOT ARCHICAD'S OWN NAVIGATOR, AND THERE IS NO WAY TO MAKE IT BE.
// Grepped the whole ACAPI_Dialog_* surface (2026-07-31): AttributePicker,
// ObjectSettings, SettingsDialog, PetPalette, and the LibPart/Picture file
// dialogs. Nothing exposes the Navigator as a chooser, and CreateAttributePicker
// takes attribute types only. So this is a LOOK-ALIKE built from DG parts, and it
// will drift from Archicad's own styling — that is the accepted cost, and the
// alternative was the flat list.
//
// It mirrors the REAL hierarchy rather than regrouping by type, which is what
// makes it recognisable. The categories a user asks for — Stories, Sections,
// Elevations, Interior Elevations, Worksheets, Details, 3D Documents, 3D,
// Schedules — ARE the Project Map's own top-level nodes, so they come for free,
// and the View Map and Layout Book keep their real user folders.
//
// Modal, and run from a button click on the main thread — the same way the
// FilePath Browse button runs the system file dialog. It must NEVER be reached
// through MainThreadGate::Invoke: a modal holds for human time and would report a
// false timeout (see the gate's contract).

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "DGModule.hpp"

#include "Palette/NavItemChoices.hpp"

#include <memory>

namespace evp {

class NavigatorBrowser : public DG::ModalDialog,
                         public DG::PanelObserver,
                         public DG::ButtonItemObserver,
                         public DG::SearchEditObserver,
                         public DG::NormalTabObserver,
                         public DG::TreeViewObserver,
                         public DG::CompoundItemObserver {
public:
    // `currentGuid` preselects and reveals the item the parameter already holds, so
    // reopening the browser lands where the user left it instead of at the top.
    explicit NavigatorBrowser (const GS::UniString& currentGuid);
    ~NavigatorBrowser ();

    // Valid only after Invoke() returned Accept. Empty means nothing was chosen.
    const GS::UniString& GetSelectedGuid  () const { return selectedGuid; }
    const GS::UniString& GetSelectedLabel () const { return selectedLabel; }

private:
    virtual void ButtonClicked   (const DG::ButtonClickEvent& ev) override;
    virtual void PanelResized    (const DG::PanelResizeEvent& ev) override;

    // ⚠️ A .grc TextEdit would NOT do here: TextEditChanged fires on COMMIT
    // (DGEditControl.hpp), which is exactly what once made the palette's own search
    // box look dead. DG::SearchEdit defaults to NoDelay and reports every keystroke.
    virtual void SearchTextChanged (const DG::SearchEditChangeEvent& ev) override;
    virtual void NormalTabChanged  (const DG::NormalTabChangeEvent& ev) override;

    virtual void TreeViewSelectionChanged  (const DG::TreeViewSelectionEvent& ev) override;
    virtual void TreeViewItemDoubleClicked (const DG::TreeViewDoubleClickEvent& ev, bool* processed) override;

    // Rebuild the tree for the current filter text. Called on every keystroke, which
    // is affordable because the DATA is collected once in the constructor — only the
    // DG items are rebuilt. Collecting per keystroke would put an ACAPI tree walk on
    // the main thread between characters.
    void PopulateTree ();

    // Which nodes survive the current filter. A folder whose DESCENDANT matches has
    // to survive too, or filtering would hide the very item it just found — and a
    // matching node keeps its whole ANCESTOR chain for the same reason.
    void MarkMatches ();

    // Read the tree's current selection into selectedGuid/selectedLabel and update
    // the status line and the OK button together. One operation on purpose: an
    // enabled OK with nothing behind it is the bug this prevents.
    void SyncSelection ();

    // Tab labels, with a match count appended while a search is running
    // ("View Map (3)"). Without it a filter that matched nothing in the ACTIVE tab
    // looks like a search that found nothing at all, when the hits are one tab
    // over — the single most confusing thing tabs add to a filtered tree.
    void SyncTabLabels ();

    // Accept, but only when something selectable is actually chosen. Shared by the
    // OK button and the double-click, so the two can never diverge.
    void AcceptIfChosen ();

    GS::Array<NavTreeNode> nodes;        // the whole tree, collected ONCE
    GS::Array<bool>        matches;      // parallel to `nodes`: survives the filter
    GS::Array<Int32>       itemForNode;  // parallel to `nodes`: its DG tree item, 0 if absent

    // The reverse mapping (DG item -> node) is NOT a member: it rides on the item
    // itself via SetItemValue/GetItemValue, so it cannot fall out of step with a
    // rebuild the way a second array would.

    GS::UniString selectedGuid;
    GS::UniString selectedLabel;
    GS::UniString openGuid;              // what to reveal when the dialog opens

    DG::SingleSelTreeView tree;
    DG::LeftText          searchLabel;
    DG::LeftText          statusText;
    DG::Button            cancelButton;
    DG::Button            okButton;

    // Both built at runtime rather than from the .grc, for reasons recorded there:
    // DG::SearchEdit is the only edit control reporting per-keystroke changes, and
    // a .grc NormalTab would need one TabPage 'GDLG' resource per tab.
    std::unique_ptr<DG::SearchEdit> searchField;
    std::unique_ptr<DG::NormalTab>  tabs;   // one per map, in kNavMapNames order
};

}   // namespace evp

#endif
