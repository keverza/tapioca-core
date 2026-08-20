#include "Palette/ActionBar.hpp"

#include "Palette/PaletteMetrics.hpp"
#include "Palette/PaletteScroll.hpp"
#include "Python/CommandCatalog.hpp"

using namespace evp::palette;

namespace evp {

namespace {

// The row wraps rather than shrinking its buttons: a command may declare six
// actions, and six buttons squeezed across a narrow palette are six buttons whose
// text is cut off. Wrapping costs a line and keeps every label readable.
constexpr short ActionButtonWidth = 118;
constexpr short ActionButtonGap = 6;

} // namespace

ActionBar::ActionBar (const DG::Panel& panelRef, DG::ButtonItemObserver& observerRef)
    : panel (panelRef), observer (observerRef)
{
}

void ActionBar::Rebuild (const CommandInfo& info)
{
    Clear ();

    // The two arrays are written together by the scanner, but they cross the bus
    // separately — so trust the shorter one rather than indexing off the end. A
    // malformed scan costs a button, never a crash.
    const USize count = GS::Min (info.actions.GetSize (), info.actionLabels.GetSize ());
    for (USize index = 0; index < count; ++index) {
        // Positions are assigned by PlaceAt; this rect is only a placeholder, the
        // same way every other runtime-built item starts.
        auto button =
            std::make_unique<DG::Button> (panel, DG::Rect (Margin, 0, Margin + ActionButtonWidth, ActionButtonHeight));
        button->SetText (info.actionLabels[index]);
        // Attached to the SHELL, never to this object: the shell is the sole
        // registered DG observer, and a sub-object that subscribes itself is what
        // check_structure.py exists to catch.
        button->Attach (observer);
        button->Hide ();
        if (!enabled)
            button->Disable ();
        buttons.push_back (std::move (button));
        names.push_back (info.actions[index]);
    }
}

void ActionBar::Clear ()
{
    buttons.clear (); // unique_ptrs destroy the DG items
    names.clear ();
    // A fresh command has produced nothing yet, so its buttons start dead whatever
    // the previous command left behind.
    enabled = false;
}

void ActionBar::SetEnabled (bool value)
{
    enabled = value;
    for (std::unique_ptr<DG::Button>& button : buttons) {
        if (value)
            button->Enable ();
        else
            button->Disable ();
    }
}

GS::UniString ActionBar::ActionOf (const DG::Item* item) const
{
    for (UIndex index = 0; index < buttons.size (); ++index) {
        if (buttons[index].get () == item)
            return names[index];
    }
    return GS::UniString ();
}

short ActionBar::PlaceAt (short bandTop, short left, short right, const PaletteScroll& clip)
{
    if (buttons.empty ())
        return 0;

    // At least one per row even in a palette narrower than a single button —
    // dividing to zero here would place nothing and report a height, leaving a
    // blank band the user cannot explain.
    const short usable = (short) GS::Max ((short) ActionButtonWidth, (short) (right - left));
    const short perRow = (short) GS::Max (1, (usable + ActionButtonGap) / (ActionButtonWidth + ActionButtonGap));

    short y = bandTop;
    short column = 0;
    for (std::unique_ptr<DG::Button>& button : buttons) {
        const short x = (short) (left + column * (ActionButtonWidth + ActionButtonGap));
        clip.Place (button.get (), DG::Rect (x, y, (short) (x + ActionButtonWidth), (short) (y + ActionButtonHeight)));
        if (++column >= perRow) {
            column = 0;
            y += ActionButtonHeight + ActionButtonGap;
        }
    }
    if (column != 0)
        y += ActionButtonHeight + ActionButtonGap;

    return (short) (y - bandTop);
}

} // namespace evp
