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
// The command combo's drop-arrow events USED to ride along at the bottom of this
// file. They left for ControlPaletteUserItems.cpp when the Grasshopper PAGE
// needed one line here to take the layout over: the shell's budget only ever
// goes down, so a new line is paid for by moving something out, and event
// routing was never band geometry to begin with.

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
    y += PlaceWorkflowStatusLine (y, Margin, right); // see ControlPalette.hpp for why it is here and not in the band

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
    // 0 while no worker is running; see ControlPaletteGrasshopper.cpp.
    y += PlaceWorkflowBand (y, Margin, right, scroll, scroll.ViewBottom ());
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
