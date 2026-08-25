#ifndef GEOMETRYSERVER_PALETTE_COMMANDLISTPANEL_HPP
#define GEOMETRYSERVER_PALETTE_COMMANDLISTPANEL_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "DGModule.hpp"

#include "Palette/CommandScan.hpp" // evp::CommandInfo — what a row stands for

#include <memory>
#include <vector>

class ControlPalette;

namespace evp {

// The command band: a COMBO (text field + drop arrow), the list the combo drops
// down, and the scanned commands behind it. It has no caption of its own — the
// shell's "Commands" label is retired.
//
// The combo is built by hand because DG HAS NO EDITABLE COMBO BOX. DG::PopUp is a
// real native dropdown but cannot be typed into, and typing is half of what this
// control is for — so the closed state is a DG::TextEdit plus a small drawn arrow
// cell (same height and background colour as the field),
// and "dropped down" means the .grc list box is placed directly beneath them.
//
// The list EXPANDS INLINE rather than floating over the parameter block below it:
// DG::Item exposes no z-order at all, so an item drawn over its siblings is
// undefined — see also PaletteScroll, which has to take half-scrolled items off the
// panel for exactly the same reason. While the list is open the shell's column is
// simply longer, and it collapses back when a command is picked.
//
// Extracted from the shell when F4 needed the room, which is exactly the move the
// shell's size exception in tools/quality/check_cpp.py said was owed next. The
// shell keeps what it owns — the splitter BELOW this band, the band's place in the
// column, and the DG event subscription (it is the sole registered observer, so the
// search box and the arrow are attached to IT, never to this object).
//
// The list box comes from the .grc and is therefore bound by the shell in its
// initialiser list, as it always was, and LENT here by reference — same arrangement
// as the pen pool ParamPanel borrows. That is why this object must be declared AFTER
// it in the shell: members are destroyed in reverse order.
class CommandListPanel {
  public:
    CommandListPanel (const DG::Panel& panel, ControlPalette& observer, DG::SingleSelListBox& list);

    // Builds the runtime-created items this band owns (the text field and the drop
    // arrow) and puts the .grc pair into its starting state — which for the list box
    // is HIDDEN, since the combo starts closed. Called during the shell's deferred
    // initialization, before DG event processing begins.
    void Create ();
    void FocusSearch ();

    // Re-run the scanner and rebuild the list from it. Returns the status line the
    // scan produced, for the shell to show — the shell owns the status line.
    GS::UniString Rescan ();

    // Re-filter iff the field's text moved. Called from the shell's idle poll (what
    // makes filtering live — DG has no per-keystroke event) and from the commit path,
    // so Tab/Enter/click-away still filters if idle events are lost. Typing OPENS the
    // dropdown; that is the second way it opens, next to the arrow.
    // Returns true when the list was rebuilt, so the shell knows to repaint;
    // `selectionChanged` says whether it now points at a DIFFERENT command, which
    // is what invalidates the command block and its results table.
    bool RefreshSearch (bool& selectionChanged);

    // The command the list points at, or nullptr for no selection / the
    // "nothing matched" row.
    const CommandInfo* Selected () const;

    // Position the band starting at `top` — the full-width combo on one row, then the
    // list IF it is open — and return the height used. A closed combo costs one row.
    //
    // No PaletteScroll: this band is in the palette's FIXED head, so it goes straight
    // on the panel and never scrolls out of reach. That is also what makes the inline
    // dropdown safe — it pushes the scrolled column down instead of over it.
    short PlaceAt (short top, short left, short right);

    bool IsSource (const DG::Item* item) const;

    // --- the combo's open/closed state ---------------------------------------
    // The shell reads this to decide whether the splitter below the band belongs on
    // screen at all, and calls the closers on the events only it can see.
    bool IsOpen () const
    {
        return open;
    }

    // The drop arrow was pressed — toggles. True when the press was ours, so the
    // shell knows to reflow. The arrow is a drawn cell rather than a button, so
    // this is a mouse-down, not a click event.
    bool HandleUserItemMouseDown (const DG::UserItemMouseDownEvent& ev);

    // ...and the same cell's paint: the field's background and border, then a
    // chevron pointing the way the list is about to move. True when the update
    // was ours.
    bool HandleUserItemUpdate (const DG::UserItemUpdateEvent& ev);

    // The display's scaling changed. The cell draws hairlines against the DEVICE
    // pixel grid, so its paint depends on a number that is per-monitor and can
    // change while the palette is open.
    bool HandleResolutionChanged (const DG::Item* item);

    // The pointer entered (`inside`) or left the arrow cell. Two events, one
    // handler — the cell's answer to both is the same repaint.
    bool HandleUserItemHover (const DG::Item* item, bool inside);

    // Collapse. Every way out of the dropdown ends here — a row clicked (the
    // ordinary one), Esc, the arrow pressed again, a run starting — because they all
    // mean the same thing: the field goes back to showing whatever is selected, so a
    // half-typed query never lingers as if it were the current command.
    void CloseList ();

    // The list's height is dragged by the shell's splitter and persisted with the
    // panel's placement, so it is part of this band's interface, not an internal.
    short Height () const
    {
        return height;
    }
    void SetHeight (short value)
    {
        height = value;
    }
    short Top () const
    {
        return top;
    }

    // How small the list may be dragged before it stops shrinking — about three
    // rows, which is what a filtered query is expected to leave.
    static constexpr short MinHeight = 60;

  private:
    // (Re)build the rows from `commands` through the current query. Scoring and
    // ranking are Palette/CommandFilter's job; this only turns the answer into rows,
    // and it is the ONLY place the list box is populated. `selectionChanged` says
    // whether the command the list points at changed; the return says whether the
    // rows were rebuilt at all.
    bool ApplyFilter (bool& selectionChanged);

    // What the field means depends on whether the user is typing in it: while
    // `editing` it is a QUERY, otherwise it is a read-out of the selection and
    // filtering by it would leave the list showing one row — its own selection.
    GS::UniString Query () const;

    // Repaint the arrow cell alone. Called wherever `open` changes, because the
    // chevron's direction is drawn from it.
    void RedrawCombo ();

    // Put the selection's title back in the field and stop treating it as a query.
    // Every path out of `editing` goes through here, because `lastSearchText` has to
    // move with the text or the idle poll would read the restored text as typing.
    void AdoptSelection ();

    const DG::Panel& panel;
    ControlPalette& observer;

    // NOT owned: the .grc list box, bound by the shell and lent here.
    DG::SingleSelListBox& list;

    // The combo's two runtime-built halves, so no .grc id is renumbered. DG's
    // text-edit change event fires on COMMIT (Tab/Enter/focus loss), never per
    // keystroke, so live filtering cannot hang off it: the shell's idle poll calls
    // RefreshSearch, which compares the field's text to this.
    std::unique_ptr<DG::TextEdit> searchField;
    // The arrow half: a user item ABUTTING the field, carrying DG's own ClientFrame
    // so the platform draws its border with the same code — and therefore the same
    // pixels — as the field's. Not a button (a button cannot be given the field's
    // background) and not a hand-drawn border (a border in logical units cannot be
    // aligned to a native frame snapped in device pixels).
    std::unique_ptr<DG::UserItem> comboFrame;
    GS::UniString lastSearchText;

    // The text the field holds when it is a read-out rather than a query — the
    // selected command's title. `editing` is the difference between the two, and it
    // is decided by comparing the field against this: DG cannot tell us that a
    // keystroke happened, only that the text is not what we last put there.
    GS::UniString displayText;
    bool editing = false;
    bool open = false;
    // Pointer over the arrow cell. A drawn cell gets no hover state for free, so
    // the one feedback a native button gave us has to be kept by hand.
    bool hovered = false;

    // Every scanned command, in display order. NOT parallel to the list's rows —
    // the search box means the list shows a subset, in relevance order.
    std::vector<CommandInfo> commands;

    // list row (1-based) -> index into `commands`. The one mapping between what is
    // on screen and what will run; Selected() is the only reader.
    std::vector<UIndex> visibleCommands;

    // Five rows of the .grc list box (16px each, per its resource line) plus its
    // border — the dropdown's default depth. The splitter below the band overrides
    // it, and palette.json remembers what the user dragged it to.
    short height = 88;
    short top = 0;
};

} // namespace evp

#endif
