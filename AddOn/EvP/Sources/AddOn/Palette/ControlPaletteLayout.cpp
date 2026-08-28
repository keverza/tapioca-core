// The palette's band layout — moved out of the shell so the shell keeps its own
// four concerns (DG event routing, the splitters, the run state, and the
// orchestration below) without the size cap forcing a choice between them.
//
// This is a SHELL implementation file, like ControlPaletteParams.cpp: it defines
// ControlPalette methods and may touch the shell's members. It is not a
// sub-object — a sub-object answers the shell and never drives it, and Layout
// drives everything.
//
// The band sequence itself stays here in one readable top-to-bottom pass,
// because "where does everything sit" is exactly the question a reader opens
// this file to answer.
//
// The command combo's drop arrow rides along at the bottom: it is a drawn cell
// rather than a button, so DG delivers its press and its paint as user-item
// events, and both are band geometry — the press is a reflow of this very
// sequence, the paint is twenty pixels of the band's own row. The shell's line
// budget only ever goes down, so they land here rather than beside the button
// routing they replaced.

#include "ControlPalette.hpp"
#include "Palette/PaletteMetrics.hpp" // Margin / BottomMargin / ActionButtonHeight / …

using namespace evp::palette;

// ---------------------------------------------------------------------------
// ONE place decides where everything sits, from the panel's live size. The .grc
// rects are only a starting point: the panel resizes, and the generated controls
// change per command, so nothing may assume its designed position.
void ControlPalette::Layout ()
{
    const short width = GetWidth ();
    const short right = width - Margin;
    const short content = right - Margin;

    short y = 12;

    // ---- THE FIXED HEAD -------------------------------------------------
    // Buttons, command picker and the instruction line never scroll. Between them
    // they are the whole of "what do I press next", and a panel dragged short enough
    // to scroll is exactly when scrolling that out of sight would hurt most.

    // The server's address, on its own line above the row that starts it.
    y += serverBand.PlaceAt (y, Margin, right);
    y += PlaceDynamoStatus (y, Margin, right);

    // ---- action row: Start/Stop server | Rescan | Run, all one line -----
    // Three equal thirds so they stay balanced at any panel width.
    const short gap = 6;
    const short cell = (short) ((content - 2 * gap) / 3);
    runToggle.SetRect (DG::Rect (Margin, y, Margin + cell, y + ActionButtonHeight));
    rescanButton.SetRect (DG::Rect (Margin + cell + gap, y, Margin + 2 * cell + gap, y + ActionButtonHeight));
    runButton.SetRect (DG::Rect (right - cell, y, right, y + ActionButtonHeight));
    y += ActionButtonHeight + 8;

    // ---- the command picker, full width, straight under the buttons -----
    // No caption: a combo holding a command's name, directly below the row that runs
    // it, does not need a word to say what it is.
    y += commandsPanel.PlaceAt (y, Margin, right);
    // The splitter bar belongs to the OPEN dropdown: it drags the depth of the list,
    // so with the list collapsed there is nothing for it to size and a bare rule
    // under a single row would read as a divider that does something. The height it
    // sets is still persisted, so reopening restores the depth the user chose.
    if (commandsPanel.IsOpen ()) {
        commandListSplitter->SetRect (DG::Rect (Margin, y, right, y + SplitterBarHeight));
        commandListSplitter->Show ();
        y += SplitterBarHeight + 6;
    }
    else {
        if (commandListSplitter)
            commandListSplitter->Hide ();
        y += 6;
    }

    // What to do next, under the picker: every instruction it gives is about the
    // command that combo names, so it reads as that command's line, not the panel's.
    commandStatus.SetRect (DG::Rect (Margin, y, right, y + 16));
    y += 22;

    // The Continue button sits right under the status message, and ONLY while it
    // means something — while a command waits on a selection. It takes no layout
    // space otherwise. (Cancelling a run is the Run button's job now — F1.)
    if (continueButton) {
        if (promptActive.load ()) {
            continueButton->SetRect (DG::Rect (Margin, y, Margin + ButtonWidth, y + 28));
            continueButton->Show ();
            y += 34;
        }
        else {
            continueButton->Hide ();
        }
    }

    // ---- everything below here SCROLLS (F4) -----------------------------
    // Virtual coordinates carry straight on from y, so a panel with nothing to
    // scroll places every item exactly where it did before F4 existed. From here
    // on, no item is given to DG directly: `scroll` applies the offset and takes
    // whatever falls outside the viewport off the panel.
    const short footerBottom = (short) (GetHeight () - BottomMargin);
    preview.PlaceAt (Margin, right, footerBottom);
    const short previewHeight = preview.Height ();
    scroll.Begin (y, (short) (footerBottom - previewHeight - (previewHeight > 0 ? 8 : 0)));

    // PLAT-F13 — the description band, above the inputs it explains. It reports
    // 0 when the command has no description, so it costs the layout nothing; a
    // folded band still reports its header, because a fold with nothing left to
    // click cannot be unfolded.
    {
        const short used = description.PlaceAt (y, Margin, right, scroll);
        if (used > 0) {
            y += used;
            // Its own splitter, so the text can be sized against the parameter
            // block below it — and only while there IS text and it is open: a
            // bar under a folded band, or under a command with no description
            // at all, would drag nothing, and a bar that does nothing is worse
            // than no bar.
            if (!description.IsCollapsed () && description.ContentHeight () > 0) {
                scroll.Place (descriptionSplitter.get (), DG::Rect (Margin, y, right, y + SplitterBarHeight));
                y += SplitterBarHeight + 6;
            }
            else if (descriptionSplitter) {
                descriptionSplitter->Hide ();
            }
        }
        else if (descriptionSplitter) {
            descriptionSplitter->Hide ();
        }
    }

    // Element sets sit BETWEEN the description and the inputs: the description
    // says what the command does, then the user builds the set the parameters
    // below operate on. See SelectionSetPanel.hpp.
    y += selectionSets.PlaceAt (y, Margin, right, scroll);

    // The command block places itself in the band starting at y — the generated
    // rows, required, section rule, optional — and reports the height used.
    y += params.PlaceAt (y, Margin, right, scroll);
    // The action bar sits directly under the results it acts on. It reports 0
    // when the command declares no actions, so it costs the layout nothing.
    // ABOVE the results rather than below: the results table is the tallest
    // thing in the panel and a row of buttons under it would be pushed off a
    // short palette exactly when there was something to export.
    {
        const short used = actionBar.PlaceAt (y, Margin, right, scroll);
        if (used > 0)
            y += used;
    }

    // ---- results table (Feature D) --------------------------------------
    // The sub-object places itself in the band starting at y and reports the height
    // it used — 0 when it has nothing to show, so it costs the layout no space.
    {
        const short used = results.PlaceAt (y, Margin, right, scroll);
        if (used > 0) {
            y += used;
            // Its own splitter, so the table can be sized against the parameter
            // block above it — shown only while the table is. The bar stays here,
            // on the shell: it is the boundary between two bands, not the table's.
            scroll.Place (tableSplitter.get (), DG::Rect (Margin, y, right, y + SplitterBarHeight));
            y += SplitterBarHeight + 8;
        }
        else if (tableSplitter) {
            tableSplitter->Hide ();
        }
    }

    // The offset can only be clamped once the column's full height is known — and a
    // clamp moves every item, so the column has to be placed again. One extra pass
    // at most: the second starts from an offset already in range, which cannot
    // clamp again.
    if (scroll.End (y)) {
        Layout ();
        return;
    }

    // One repaint of the whole panel after every reflow. Moving items with SetRect
    // does not always invalidate the area a control VACATED — so text below a shrunk
    // table could keep its old position on screen until the next unrelated redraw.
    RedrawItems ();
}

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
