// The palette shell's DRAWN-CELL AND BAR EVENTS - the command combo's arrow, the
// preview band's, and every scroll bar in the panel.
//
// WHY THEY ARE NOT BUTTONS: DG HAS NO EDITABLE COMBO BOX (CommandListPanel.hpp
// says so at length). The combo is a TextEdit and a drawn arrow side by side, so
// the arrow's press and its paint arrive as user-item events and the cell has to
// paint itself.
//
// WHY THE BARS TOO: there are three of them, and none is a control the user
// thinks of as a bar -- the scrolled column's, the preview's opacity, and a
// bounded numeric input's slider. So each of those events arrives with two
// other candidates to be told apart from, which is routing, which is what this
// file is. It also kept the shell's own budget going down, which is the rule.
//
// WHY THIS FILE: they are event routing, and routing is the shell's own concern
// - but they had been living in ControlPaletteLayout.cpp, whose header called
// them band geometry. They are not: the press causes a reflow, which is not the
// same as being one. They moved when the Grasshopper page needed a line in that
// file, and the shell's line budget only ever goes down.
//
// Everything here runs on Archicad's MAIN thread, like every other DG handler.

#include "ControlPalette.hpp"

// Opening or closing the command combo reflows the column, including rows the
// list vacates and DG does not redraw itself.
void ControlPalette::UserItemMouseDown (const DG::UserItemMouseDownEvent& ev, bool* processed)
{
    if (preview.HandleUserItemMouseDown (ev)) {
        if (processed != nullptr)
            *processed = true;
        return;
    }
    if (!commandsPanel.HandleUserItemMouseDown (ev))
        return;

    Layout ();
    Redraw ();
    if (processed != nullptr)
        *processed = true;
}

void ControlPalette::UserItemMouseUp (const DG::UserItemMouseUpEvent& ev, bool* processed)
{
    if (preview.HandleUserItemMouseUp (ev) && processed != nullptr)
        *processed = true;
}

void ControlPalette::UserItemDoubleClicked (const DG::UserItemDoubleClickEvent& ev)
{
    preview.HandleUserItemDoubleClicked (ev);
}

// The preview canvas owns its wheel; elsewhere the wheel scrolls the column.
void ControlPalette::PanelWheelTracked (const DG::PanelWheelTrackEvent& ev, bool* processed)
{
    if (preview.HandleWheelTracked (ev)) {
        if (processed != nullptr)
            *processed = true;
        return;
    }
    const DG::Item* const over = ev.GetItem ();
    if (commandsPanel.IsSource (over) || results.IsSource (over) || !scroll.Wheel (ev.GetYTrackValue ()))
        return;
    Layout ();
    if (processed != nullptr)
        *processed = true;
}

// ...and the same cell's paint: the field's background colour, then the chevron.
// The band draws it; this only routes the event.
void ControlPalette::UserItemUpdate (const DG::UserItemUpdateEvent& ev)
{
    if (!preview.HandleUserItemUpdate (ev))
        commandsPanel.HandleUserItemUpdate (ev);
}

void ControlPalette::UserItemMouseEntered (const DG::UserItemMouseEnteredEvent& ev)
{
    if (!preview.HandleUserItemMouseEntered (ev))
        commandsPanel.HandleUserItemHover (ev.GetSource (), true);
}

// The preview's business alone: the command combo's arrow highlights on entering
// and leaving, which says everything a cell that size has to say.
void ControlPalette::UserItemMouseMoved (const DG::UserItemMouseMoveEvent& ev, bool* /*noDefaultCursor*/)
{
    preview.HandleUserItemMouseMoved (ev);
}

void ControlPalette::UserItemMouseExited (const DG::UserItemMouseExitedEvent& ev)
{
    if (!preview.HandleUserItemMouseExited (ev))
        commandsPanel.HandleUserItemHover (ev.GetSource (), false);
}

void ControlPalette::ItemResolutionFactorChanged (const DG::ItemResolutionFactorChangeEvent& ev)
{
    commandsPanel.HandleResolutionChanged (ev.GetSource ());
}

void ControlPalette::ScrollBarChanged (const DG::ScrollBarChangeEvent& ev)
{
    // A bounded numeric input's slider first: it writes into the field beside it,
    // which the band reads back on its next idle. It is not the palette's own
    // scroll and must not move the column.
    if (workflow.FollowSlider (ev.GetSource ()))
        Redraw ();
    else if (!preview.HandleScrollBarChanged (ev) && scroll.IsSource (ev.GetSource ()) && scroll.FollowBar ())
        Layout ();
}

// ...and while the thumb is still held. Both are needed: a column that only moved
// when the thumb was let go would not read as a scroll bar at all.
void ControlPalette::ScrollBarTracked (const DG::ScrollBarTrackEvent& ev)
{
    if (workflow.FollowSlider (ev.GetSource ()))
        Redraw ();
    else if (!preview.HandleScrollBarTracked (ev) && scroll.IsSource (ev.GetSource ()) && scroll.FollowBar ())
        Layout ();
}
