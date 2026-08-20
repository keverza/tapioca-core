#include "APIEnvir.h"
#include "ACAPinc.h"

#include "Palette/NavigatorBrowser.hpp"
#include "Palette/CommandFilter.hpp"    // SplitSearchTerms / ScoreCommand — DevKit-free, tested
#include "ResourceIds.hpp"

#include <string>

namespace evp {

namespace {

// The dialog's own layout, in the .grc's coordinates. Repeated here rather than
// read back off the items for the same reason AboutDialog repeats its row tops: a
// temporary wrapper around a resource item is a lifetime question not worth taking
// on for a handful of numbers. Move an item in the .grc and move it here, in the
// same edit.
// NOT in PaletteMetrics.hpp — that header is the PALETTE's layout, and a constant
// only moves there on its SECOND consumer.
constexpr short Margin        = 12;
constexpr short SearchTop     = 12;
constexpr short SearchHeight  = 20;
constexpr short SearchLeft    = 72;    // right of the "Search:" label
constexpr short TabsTop       = 44;
constexpr short TabsHeight    = 24;
constexpr short TreeTop       = 68;    // TabsTop + TabsHeight
constexpr short StatusHeight  = 16;
constexpr short ButtonHeight  = 24;
constexpr short ButtonWidth   = 108;
constexpr short ButtonGap     = 8;
constexpr short BottomPad     = 16;

// UTF-8 for the searcher, which is deliberately std::string-only.
std::string Utf8 (const GS::UniString& text)
{
    return std::string (text.ToCStr (0, GS::MaxUSize, CC_UTF8).Get ());
}

}   // namespace

// ---------------------------------------------------------------------------
NavigatorBrowser::NavigatorBrowser (const GS::UniString& currentGuid) :
    DG::ModalDialog (ACAPI_GetOwnResModule (), NavigatorBrowserResId, ACAPI_GetOwnResModule ()),
    openGuid        (currentGuid),
    tree            (GetReference (), NavBrowserTreeId),
    searchLabel     (GetReference (), NavBrowserSearchLabelId),
    statusText      (GetReference (), NavBrowserStatusTextId),
    cancelButton    (GetReference (), NavBrowserCancelButtonId),
    okButton        (GetReference (), NavBrowserOkButtonId)
{
    AttachToAllItems (*this);
    Attach (*this);

    // ⚠️ COLLECTED ONCE, HERE. The filter re-runs on every keystroke and must never
    // re-walk the Navigator: that is an ACAPI tree walk on the main thread between
    // two characters of typing. Rebuilding DG items is cheap; re-reading is not.
    CollectNavigatorTree (nodes);
    matches.SetSize (nodes.GetSize ());
    itemForNode.SetSize (nodes.GetSize ());

    tabs = std::make_unique<DG::NormalTab> (
        *this, DG::Rect (Margin, TabsTop, (short) (GetWidth () - Margin),
                         (short) (TabsTop + TabsHeight)));
    for (Int32 i = 0; i < 3; ++i) {
        tabs->AppendItem ();
        tabs->SetItemText ((short) (i + 1), GS::UniString (kNavMapNames[i]));
    }
    tabs->Attach (*this);
    tabs->Show ();

    // Open on the tab holding the current value, so reopening the browser lands
    // where the user left it — otherwise a View Map choice would reopen on the
    // Project Map and look lost.
    short openTab = 2;      // View Map: where saved, placeable views live
    if (!openGuid.IsEmpty ()) {
        for (const NavTreeNode& node : nodes) {
            if (node.guid == openGuid) { openTab = (short) (node.mapIndex + 1); break; }
        }
    }
    tabs->SelectItem (openTab);

    searchField = std::make_unique<DG::SearchEdit> (
        *this, DG::Rect (SearchLeft, SearchTop, (short) (GetWidth () - Margin),
                         (short) (SearchTop + SearchHeight)));
    searchField->Attach (*this);
    searchField->Show ();
    searchField->SetFocus ();   // open the browser, start typing

    PopulateTree ();
    SyncTabLabels ();
    SyncSelection ();
}

NavigatorBrowser::~NavigatorBrowser () = default;

// ---------------------------------------------------------------------------
// Filtering
// ---------------------------------------------------------------------------

// Which nodes survive the current search text.
//
// Two directions, and BOTH are required for the result to make sense:
//   * a matching node keeps its ANCESTORS, or the item found would have no branch
//     to sit on and could not be shown at all;
//   * a folder keeps itself when a DESCENDANT matches, which is the same statement
//     from the other end and is what makes "type 's-01', see it inside Sections".
//
// The search itself is the palette's own ranker (Palette/CommandFilter.hpp): it is
// DevKit-free and covered by the offline test suite, so the fuzzy behaviour a user
// already learned in the command list is the behaviour they get here. A node maps
// onto SearchableCommand as title=name, category=type, folder=path.
void NavigatorBrowser::MarkMatches ()
{
    const std::vector<std::string> terms =
        SplitSearchTerms (Utf8 (searchField ? searchField->GetText () : GS::UniString ()));

    if (terms.empty ()) {
        for (UIndex i = 0; i < nodes.GetSize (); ++i)
            matches[i] = true;
        return;
    }

    for (UIndex i = 0; i < nodes.GetSize (); ++i) {
        const NavTreeNode& node = nodes[i];
        // Only real items are matched. A map root or a folder appears because
        // something under it matched, never because its own name did — otherwise
        // typing "map" would return three whole trees.
        if (!node.selectable) {
            matches[i] = false;
            continue;
        }
        SearchableCommand searchable;
        searchable.title    = Utf8 (node.name);
        searchable.category = Utf8 (node.typeName);
        searchable.folder   = Utf8 (node.path);
        matches[i] = ScoreCommand (searchable, terms) > 0;
    }

    // Ancestors of a match, in one backward pass. `nodes` is ordered parent-before-
    // child (CollectNavigatorTree guarantees it), so walking BACKWARDS propagates a
    // hit all the way to its root in a single sweep — a child is always visited
    // before the parent it has to light up.
    for (UIndex i = nodes.GetSize (); i > 0; --i) {
        const UIndex index = i - 1;
        if (matches[index] && nodes[index].parent >= 0)
            matches[(UIndex) nodes[index].parent] = true;
    }
}

void NavigatorBrowser::PopulateTree ()
{
    MarkMatches ();

    // One repaint, not one per item — the same reason the command list disables
    // draw around its rebuild.
    tree.DisableDraw ();
    tree.DeleteItem (DG::TreeView::AllItems);


    const bool filtering = !SplitSearchTerms (Utf8 (searchField->GetText ())).empty ();
    const Int32 activeMap = (Int32) tabs->GetSelectedItem () - 1;   // tabs are 1-based

    Int32 reveal = 0;
    for (UIndex i = 0; i < nodes.GetSize (); ++i) {
        itemForNode[i] = 0;
        if (!matches[i] || nodes[i].mapIndex != activeMap)
            continue;

        // The MAP ROOT itself is not a row: the tab already says which map this is,
        // and a root node inside its own tab would be one pointless level of
        // indentation on everything. Its children become the top-level rows, which
        // in the Project Map are exactly the category nodes.
        if (nodes[i].parent < 0) {
            itemForNode[i] = (Int32) DG::TreeView::RootItem;
            continue;
        }

        const Int32 parentItem = itemForNode[(UIndex) nodes[i].parent];
        const Int32 item = tree.AppendItem (parentItem);
        itemForNode[i] = item;

        // ⚠️ THE ROW AND ITS VALUE, TOGETHER. Same rule as the flat picker: the text
        // the user reads and the guid the script gets are different things, and
        // pairing them in one statement is what stops an early `continue` from
        // shifting every value after it. SetItemValue carries the node index, so
        // the mapping survives any reordering DG might do.
        tree.SetItemText (item, nodes[i].name);
        tree.SetItemValue (item, (DGUserData) i);

        // A folder, or a Project Map item that no command can be given: visible so
        // the user can see WHERE they are, disabled so it cannot be the answer. A
        // Project Map "Ground Floor" that simply vanished would read as "my project
        // has no ground floor".
        if (!nodes[i].selectable)
            tree.DisableItem (item);

        if (!openGuid.IsEmpty () && nodes[i].guid == openGuid)
            reveal = item;
    }

    // ⚠️ EXPANDING IS A SECOND PASS, AND IT HAS TO BE.
    // The first version expanded each item right after appending it — which is a
    // no-op, because at that moment the node has no children yet: they are appended
    // later in the same loop, and a node that gains children after being expanded
    // comes back COLLAPSED. That is the bug the first live run found: "search box
    // shows item in collapsed state, it should reveal matches." Everything is built
    // first; only then is anything expanded.
    if (filtering) {
        // Every surviving branch, so a hit three folders deep is actually on screen.
        // A search whose result you cannot see is a search that looks broken.
        for (UIndex i = 0; i < nodes.GetSize (); ++i) {
            if (itemForNode[i] > 0)
                tree.ExpandItem (itemForNode[i]);
        }
    } else {
        // Unfiltered: open only the first level, so the tab opens on its category
        // list (Stories, Sections, …) rather than on every view in the project.
        for (UIndex i = 0; i < nodes.GetSize (); ++i) {
            if (itemForNode[i] > 0 && nodes[i].parent >= 0 &&
                itemForNode[(UIndex) nodes[i].parent] == (Int32) DG::TreeView::RootItem)
                tree.ExpandItem (itemForNode[i]);
        }
    }

    // Reopen where the user left off, revealing it: select, then expand every
    // ancestor. Done AFTER the expand pass so it cannot be undone by it.
    if (reveal != 0) {
        for (Int32 up = tree.GetItem (reveal, DG::TreeView::Parent);
             up > 0; up = tree.GetItem (up, DG::TreeView::Parent))
            tree.ExpandItem (up);
        tree.SelectItem (reveal);
        openGuid.Clear ();
    }

    tree.EnableDraw ();
    tree.Redraw ();
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------
// "View Map (3)" while filtering, the bare name otherwise.
//
// ⚠️ THIS IS WHAT MAKES TABS SAFE TO ADD. Splitting the tree across three tabs
// means a filter can match items you cannot see, and an empty active tab then
// reads as "nothing found" when the hits are one tab over. The counts are the
// whole answer to that, so do not drop them to tidy the labels.
void NavigatorBrowser::SyncTabLabels ()
{
    const bool filtering = !SplitSearchTerms (Utf8 (searchField->GetText ())).empty ();

    Int32 found[3] = { 0, 0, 0 };
    if (filtering) {
        for (UIndex i = 0; i < nodes.GetSize (); ++i) {
            // Only real, selectable hits are counted. Counting the folders that
            // survive to carry them would inflate every number.
            if (matches[i] && nodes[i].selectable)
                ++found[nodes[i].mapIndex];
        }
    }

    for (Int32 i = 0; i < 3; ++i) {
        tabs->SetItemText ((short) (i + 1),
            filtering ? GS::UniString::Printf ("%s (%d)", kNavMapNames[i], (int) found[i])
                      : GS::UniString (kNavMapNames[i]));
    }
}

void NavigatorBrowser::SyncSelection ()
{
    selectedGuid.Clear ();
    selectedLabel.Clear ();

    const Int32 item = tree.GetSelectedItem ();
    if (item != 0) {
        const UIndex index = (UIndex) tree.GetItemValue (item);
        if (index < nodes.GetSize () && nodes[index].selectable) {
            selectedGuid  = nodes[index].guid;
            selectedLabel = nodes[index].path.IsEmpty ()
                          ? nodes[index].name
                          : GS::UniString::Printf ("%T  -  %T", nodes[index].name.ToPrintf (),
                                                                nodes[index].path.ToPrintf ());
        }
    }

    if (selectedGuid.IsEmpty ()) {
        statusText.SetText ("Nothing selected");
        okButton.Disable ();    // OK cannot be pressed with nothing behind it
    } else {
        statusText.SetText (selectedLabel);
        okButton.Enable ();
    }
}

void NavigatorBrowser::AcceptIfChosen ()
{
    if (!selectedGuid.IsEmpty ())
        PostCloseRequest (DG::ModalDialog::Accept);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
void NavigatorBrowser::ButtonClicked (const DG::ButtonClickEvent& ev)
{
    if (ev.GetSource () == &okButton) {
        AcceptIfChosen ();
    } else if (ev.GetSource () == &cancelButton) {
        // Cancel means CANCEL: drop whatever was highlighted, so the caller keeps
        // the value the parameter already had rather than the last thing clicked.
        selectedGuid.Clear ();
        selectedLabel.Clear ();
        PostCloseRequest (DG::ModalDialog::Cancel);
    }
}

void NavigatorBrowser::SearchTextChanged (const DG::SearchEditChangeEvent& /*ev*/)
{
    PopulateTree ();
    SyncTabLabels ();   // where the hits are, including the tabs you cannot see
    SyncSelection ();   // the selected row may have just been filtered away
}

void NavigatorBrowser::NormalTabChanged (const DG::NormalTabChangeEvent& /*ev*/)
{
    // The search text deliberately SURVIVES a tab change: the counts on the tabs
    // are an invitation to go and look at the other map's hits, and clearing the
    // filter on arrival would throw away the query that found them.
    PopulateTree ();
    SyncSelection ();
}

void NavigatorBrowser::TreeViewSelectionChanged (const DG::TreeViewSelectionEvent& /*ev*/)
{
    SyncSelection ();
}

void NavigatorBrowser::TreeViewItemDoubleClicked (const DG::TreeViewDoubleClickEvent& /*ev*/,
                                                  bool* processed)
{
    // Double-click on a real item is "choose this". On a folder it is not handled
    // here, so DG keeps its own expand/collapse behaviour — which is what a user
    // expects from every tree they have ever used.
    SyncSelection ();
    if (!selectedGuid.IsEmpty ()) {
        AcceptIfChosen ();
        if (processed != nullptr)
            *processed = true;
    }
}

// The dialog is growable because a deep Navigator path needs the room. Everything
// stretches with it; only the button row stays pinned to the bottom right.
void NavigatorBrowser::PanelResized (const DG::PanelResizeEvent& ev)
{
    const short width  = (short) (GetWidth ()  - Margin);
    const short height = GetHeight ();

    const short buttonTop  = (short) (height - BottomPad - ButtonHeight);
    const short statusTop  = (short) (buttonTop - StatusHeight - ButtonGap);

    if (searchField)
        searchField->SetRect (DG::Rect (SearchLeft, SearchTop, width, (short) (SearchTop + SearchHeight)));
    if (tabs)
        tabs->SetRect (DG::Rect (Margin, TabsTop, width, (short) (TabsTop + TabsHeight)));
    tree.SetRect       (DG::Rect (Margin, TreeTop, width, (short) (statusTop - ButtonGap)));
    statusText.SetRect (DG::Rect (Margin, statusTop, width, (short) (statusTop + StatusHeight)));
    okButton.SetRect     (DG::Rect ((short) (width - ButtonWidth), buttonTop,
                                    width, (short) (buttonTop + ButtonHeight)));
    cancelButton.SetRect (DG::Rect ((short) (width - 2 * ButtonWidth - ButtonGap), buttonTop,
                                    (short) (width - ButtonWidth - ButtonGap),
                                    (short) (buttonTop + ButtonHeight)));

    DG::PanelObserver::PanelResized (ev);
}

}   // namespace evp
