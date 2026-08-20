#ifndef GEOMETRYSERVER_PALETTE_DESCRIPTIONPANEL_HPP
#define GEOMETRYSERVER_PALETTE_DESCRIPTIONPANEL_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "DGModule.hpp"

#include <memory>
#include <vector>

namespace evp {

class PaletteScroll;

// PLAT-F13 — the selected command's description, as its own collapsible band.
//
// It used to live inside ParamPanel, always fully expanded, with no header and
// no way to put it away. On a command whose description runs to six lines that
// is six lines of the parameter block pushed off the top of a short palette,
// every time, for a sentence the user has already read.
//
// So it is a band now, with the two things a band needs: a header the user can
// click to fold it away, and a splitter below it (owned by the SHELL, like the
// results table's) to drag its height. The height and the folded state are
// persisted in palette.json, because a user who folded this away meant it.
//
// The shell owns the palette, the DG event subscription and the splitter; this
// object owns the header, the wrapped text lines and its own height. Height /
// SetHeight / Top / IsVisible are public for exactly the reason ResultsTable's
// are: the shell's splitter maths and placement persistence need them, so the
// height is part of the interface rather than an internal detail.
class DescriptionPanel {
  public:
    // The shell stays the sole registered DG observer: the header button is
    // attached to `observer` (the palette), never to this object.
    DescriptionPanel (const DG::Panel& panel, DG::ButtonItemObserver& observer);

    // Builds the header button. Called from the shell's constructor BODY, so
    // item creation keeps happening where every other runtime-built item's does
    // — after BeginEventProcessing.
    void Create ();

    // Word-wrap `text` into as many single-line LeftTexts as it needs, and label
    // the fold header with `title` — the command's own name. That header is the
    // ONLY place the palette names the selected command (the .grc title line
    // above it was retired; the two together printed the name twice), so a
    // command with no description still gets a header, just without the arrow.
    // DG::LeftText does not wrap, so a long description would otherwise simply
    // be clipped.
    //
    // ⚠️ Wrapped against palette::WrapWidth, NOT the live panel width. See the
    // comment on that constant: re-flowing from the live width never survived a
    // resize, so the breaks were stale whenever the panel had changed.
    void Rebuild (const GS::UniString& title, const GS::UniString& text);

    // Hide the band and forget its content.
    void Clear ();

    // Position the band starting at `top`; returns the height used — 0 only when
    // there is neither a title nor a description, so it costs the layout no
    // space at all. A command with no description reports just its header. When
    // collapsed it returns just the header's height: folded away is not gone,
    // or there would be nothing left to click to bring it back.
    short PlaceAt (short top, short left, short right, const PaletteScroll& clip);

    // Whether the band shows anything at all — a bare header naming a command
    // with no description still counts.
    bool IsVisible () const
    {
        return !lines.empty () || !title.IsEmpty ();
    }
    bool IsCollapsed () const
    {
        return collapsed;
    }
    void SetCollapsed (bool value);

    // The text area's height, excluding the header. The shell's splitter drags
    // this; PlaceAt clamps it to what the text actually needs, so dragging past
    // the last line does not leave a band of empty panel.
    short Height () const
    {
        return height;
    }
    void SetHeight (short value)
    {
        height = value;
    }
    // Where PlaceAt last put the TEXT (below the header), so a splitter drag can
    // turn a dialog-relative y back into a height.
    short Top () const
    {
        return top;
    }
    // The height the full text wants, so the shell can stop a drag from growing
    // the band past its own content.
    short ContentHeight () const;

    // How small the text area may be dragged before it stops shrinking. Two
    // lines: one line plus a scrollbar-less clip reads as a rendering fault
    // rather than a deliberate size.
    static constexpr short MinHeight = 30;

    // Event routing, for the shell's single ButtonClicked handler.
    bool IsSource (const DG::Item* item) const
    {
        return header != nullptr && item == header.get ();
    }

  private:
    void UpdateHeaderText ();

    // The command's name, shown on the fold header.
    GS::UniString title;

    const DG::Panel& panel;
    DG::ButtonItemObserver& observer;

    // A flat push button, not a checkbox: the band's state is already visible
    // (the text is there or it is not), so a tick box would say the same thing
    // twice and steal the width that says which way the click goes.
    std::unique_ptr<DG::Button> header;
    std::vector<std::unique_ptr<DG::LeftText>> lines;

    bool collapsed = false;
    short height = 0; // 0 = "as tall as the text needs"
    short top = 0;
};

} // namespace evp

#endif
