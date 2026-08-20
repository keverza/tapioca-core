#include "DescriptionPanel.hpp"

#include "Palette/PaletteMetrics.hpp"
#include "Palette/PaletteScroll.hpp"

using namespace evp::palette;

namespace evp {

namespace {

// One wrapped line of the command's description. Was file-local to ParamPanel
// while the description lived there; it moved with the band, not with the file.
constexpr short LineHeight = 15;

// The clickable header. Full width so the whole strip is a target — a fold
// control the user has to aim at is a fold control they stop using.
constexpr short HeaderHeight = 18;

// The gap under the band, matching the one ParamPanel used to leave after the
// description so the first input row does not sit against the text.
constexpr short GapBelow = 6;

} // namespace

DescriptionPanel::DescriptionPanel (const DG::Panel& panelRef, DG::ButtonItemObserver& observerRef)
    : panel (panelRef), observer (observerRef)
{
}

void DescriptionPanel::Create ()
{
    // Positions are assigned by PlaceAt; this rect is only a placeholder, the
    // same way every other runtime-built item starts.
    header = std::make_unique<DG::Button> (panel, DG::Rect (Margin, 0, Margin + 100, HeaderHeight));
    // Attached to the SHELL, never to this object: the shell is the sole
    // registered DG observer, and a sub-object that subscribes itself is what
    // check_structure.py exists to catch.
    header->Attach (observer);
    header->Hide ();
    UpdateHeaderText ();
}

void DescriptionPanel::UpdateHeaderText ()
{
    if (header == nullptr)
        return;
    // The command's NAME is the header, and the arrow says which way the click
    // goes. There is no separate title line above the band any more - the two
    // together read as the same name printed twice.
    //
    // No arrow when there is no description: the header is then just the
    // command's name, and an arrow promising a fold that folds nothing is worse
    // than none.
    //
    // ⚠️ ASCII arrows. A narrow literal cannot hold "\x25B8" (error C7744, out of
    // range), and a real glyph would need a u8/u16 literal whose rendering in a
    // DG::Button on this machine's font is unverified. "v" and ">" are
    // understood everywhere and cost nothing.
    const GS::UniString name (title.IsEmpty () ? GS::UniString ("Description") : title);
    if (lines.empty ())
        header->SetText (name);
    else
        header->SetText ((collapsed ? GS::UniString ("> ") : GS::UniString ("v ")) + name);
}

void DescriptionPanel::SetCollapsed (bool value)
{
    collapsed = value;
    UpdateHeaderText ();
}

short DescriptionPanel::ContentHeight () const
{
    return (short) (lines.size () * LineHeight);
}

// Word-wrap into as many single-line LeftTexts as the text needs, and label the
// fold header with the command's own name. DG::LeftText does not wrap, so a long
// description would otherwise simply be clipped.
void DescriptionPanel::Rebuild (const GS::UniString& newTitle, const GS::UniString& text)
{
    title = newTitle;
    lines.clear ();
    if (text.IsEmpty ()) {
        // No description, but the header still carries the command's name - it is
        // the only place the palette names the selected command now.
        UpdateHeaderText ();
        if (header != nullptr) {
            if (title.IsEmpty ())
                header->Hide ();
            else
                header->Show ();
        }
        return;
    }

    // Use a conservative average for SmallPlain. Narrow glyphs make the real
    // average smaller, but 7 px leaves enough room for wide glyphs and avoids
    // clipping the final word.
    //
    // ⚠️ palette::WrapWidth, NOT panel.GetWidth (). See PaletteMetrics.hpp: a
    // wrap computed from the live width goes stale the moment the panel is
    // resized, because a resize does not rebuild the text — so the breaks were
    // whatever width the panel happened to be when the command was selected.
    const USize perLine = (USize) GS::Max ((short) 20, (short) (WrapWidth / 7.0));

    // A scanner-generated notice can carry its own newlines (an unreadable
    // command's reason is written as its own paragraph), so wrap each line
    // separately instead of letting Split(" ") run them together.
    GS::Array<GS::UniString> paragraphs;
    text.Split ("\n", &paragraphs);

    for (const GS::UniString& paragraph : paragraphs) {
        GS::Array<GS::UniString> words;
        paragraph.Split (" ", &words);

        GS::UniString line;
        const auto flush = [&] () {
            if (line.IsEmpty ())
                return;
            auto item = std::make_unique<DG::LeftText> (panel, DG::Rect (Margin, 0, Margin + 100, LineHeight));
            item->SetText (line);
            item->Hide ();
            lines.push_back (std::move (item));
            line.Clear ();
        };

        for (const GS::UniString& word : words) {
            if (!line.IsEmpty () && line.GetLength () + word.GetLength () + 1 > perLine)
                flush ();
            line += (line.IsEmpty () ? word : GS::UniString (" ") + word);
        }
        flush ();
    }

    UpdateHeaderText (); // the arrow appears only now that there is text to fold
    if (header != nullptr)
        header->Show ();
}

void DescriptionPanel::Clear ()
{
    lines.clear (); // unique_ptrs destroy the DG items
    title.Clear ();
    if (header != nullptr)
        header->Hide ();
}

short DescriptionPanel::PlaceAt (short bandTop, short left, short right, const PaletteScroll& clip)
{
    if (lines.empty () && title.IsEmpty ()) {
        // Nothing to say at all: no header, no height. A band that costs nothing
        // when empty is the same contract ResultsTable keeps.
        if (header != nullptr)
            header->Hide ();
        top = bandTop;
        return 0;
    }

    short y = bandTop;
    clip.Place (header.get (), DG::Rect (left, y, right, y + HeaderHeight));
    y += HeaderHeight;
    top = y;

    if (lines.empty ()) {
        // A command with no description still gets its name on the header.
        return (short) (y + GapBelow - bandTop);
    }

    if (collapsed) {
        // Folded away is not gone: the header stays, or there would be nothing
        // left to click to bring the text back.
        for (std::unique_ptr<DG::LeftText>& line : lines)
            line->Hide ();
        return (short) (y + GapBelow - bandTop);
    }

    // Clamped to what the text actually needs, so dragging the splitter past
    // the last line does not leave a band of empty panel below it. A height of
    // 0 means "never dragged" — show all of it.
    const short wanted = ContentHeight ();
    const short visible = (height <= 0) ? wanted : GS::Min (height, wanted);

    short drawn = 0;
    for (std::unique_ptr<DG::LeftText>& line : lines) {
        if (drawn + LineHeight > visible) {
            // Beyond the dragged height: hidden outright rather than left
            // where the band below will paint over it. PaletteScroll::Place
            // hides what falls outside the VIEWPORT; this line is inside the
            // viewport and outside the BAND, which only this object knows.
            line->Hide ();
            continue;
        }
        clip.Place (line.get (), DG::Rect (left, y, right, y + LineHeight));
        y += LineHeight;
        drawn += LineHeight;
    }

    return (short) (y + GapBelow - bandTop);
}

} // namespace evp
