#ifndef GEOMETRYSERVER_PALETTE_PARAMPANEL_HPP
#define GEOMETRYSERVER_PALETTE_PARAMPANEL_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "APIdefs_Interface.h" // API_AttributePicker (Layer/Pen/Fill/LineType)
#include "DGModule.hpp"

#include <memory>
#include <vector>

class ControlPalette;

namespace evp {

struct CommandInfo;
class PaletteScroll;

// One scanned parameter and the control generated for it.
struct ParamControl {
    GS::UniString name;
    GS::UniString type;
    GS::UniString unit;
    // FilePath only: arbitrary extensions to show in the modal file dialog.
    // An empty list retains the unfiltered Archicad file dialog.
    GS::Array<GS::UniString> fileExtensions;
    // FilePath only: "open" selects an existing file; "save" selects a
    // destination and lets the dialog apply the chosen format's extension.
    GS::UniString fileMode = "open";

    std::unique_ptr<DG::LeftText> label;

    // The generated control. Null for Kind::Pen, whose swatch is a pooled .grc item
    // this row only borrows — use Widget() to get whichever one is live.
    std::unique_ptr<DG::Item> control;

    // Kind::Pen only: a borrowed pool entry, owned by the palette. NOT freed here.
    DG::UserControl* penSwatch = nullptr;

    DG::Item* Widget () const
    {
        return penSwatch != nullptr ? (DG::Item*) penSwatch : control.get ();
    }

    // Reading a control back needs its concrete type, which DG::Item does not
    // carry — so remember which one we built.
    // Action is an Enum in every mechanical respect — same popup, same read-back.
    // It is a separate kind only so PlaceAt can pin it above everything else: it
    // is the command's mode, and the rows below it are there because of it.
    enum class Kind {
        Bool,
        Int,
        Real,
        Length,
        Area,
        Volume,
        Angle,
        Text,
        Enum,
        Action,
        Attribute,
        Pen,
        Story,
        FilePath,
        NavItem,
        NavBrowse,
        Catalog,
        ProjectField
    } kind = Kind::Text;

    GS::Array<GS::UniString> choices; // Enum/Action and Story: the popup's row labels

    // ProjectField only: the value behind each popup row, parallel to `choices`.
    // The row shows a Project Info field's DESCRIPTION (what the user reads in
    // File > Info > Project Info); the value sent to run() is its DATABASE KEY,
    // which is the field's identity — ACAPI_AutoText_SetAnAutoText writes by key,
    // and two fields may carry the same description. Same shape and same hazard as
    // storyIndices/itemGuids: a row appended without pushing its value here shifts
    // every value after it and the picker silently returns the WRONG field.
    GS::Array<GS::UniString> choiceValues;

    // F3 — show_when, flattened by the scanner. Empty controller == always shown.
    GS::UniString showWhenParam;
    GS::Array<GS::UniString> showWhenValues;

    // F4 — the row IS visible by its own rules, but the column is scrolled so far
    // that it does not fit in the viewport. PlaceAt records it; ShowControls has to
    // honour it, or it would put a scrolled-away row back on top of the header.
    bool clipped = false;

    // Whether this row is currently on screen. Invisible rows cost no layout
    // height, never gate Run, and are STILL collected into the JSON — run() is
    // called with its whole signature whatever the palette is showing.
    bool visible = true;

    // The value this control currently holds, as the text a show_when compares
    // against ("true"/"false" for a checkbox, the chosen label for a popup).
    // Empty for the kinds nothing sensibly controls.
    GS::UniString CurrentValueText () const;

    // Story only: the story INDEX behind each popup row. Rows are labelled
    // "0  Ground floor" for the user, but a script wants the index, and index is
    // not the row number — stories run from firstStory, which is often negative.
    GS::Array<GS::Int32> storyIndices;

    // NavItem (evp.View / evp.Database) only: the GUID behind each popup row.
    // Same shape and same hazard as storyIndices — the row reads
    // "Ground Floor  -  Plans", the value run() gets is a guid, and this array is
    // the only thing holding the two together. A row appended without its guid
    // (the empty-project fallback, a filtered item, an early continue) shifts
    // every guid after it and the picker silently returns the WRONG item.
    // pc.type distinguishes View from Database; the control is identical.
    GS::Array<GS::UniString> itemGuids;

    // NavBrowse (evp.View) only: the guid the user chose in the Navigator browser,
    // and the row text to show on the button.
    //
    // ⚠️ WHY evp.View IS NOT A POPUP AND evp.Database IS. The flat popup shipped
    // first and the user's verdict on a real project was "using current picker user
    // would get lost on a larger project" — a project has hundreds of views, and a
    // flat list of hundreds is a list you scroll, not one you choose from. So a View
    // opens Palette/NavigatorBrowser, a modal tree mirroring the real Navigator.
    // A DATABASE list is a dozen entries with no hierarchy at all, so it stays a
    // popup: a tree dialog for twelve rows would be ceremony, not help.
    GS::UniString selectedGuid;
    GS::UniString selectedLabel;

    // Catalog (evp.LibraryPart / evp.Favourite) only. Same button-plus-modal
    // shape as NavBrowse, and three things that differ:
    //
    //  * THE VALUE IS A JSON OBJECT, not a scalar — the only parameter kind whose
    //    is. A library part needs its unID (stable, unreadable) AND its name
    //    (readable, not unique); a favourite needs its name AND the element type
    //    that says what it may be applied to. Sending half of either would make
    //    the command's own error messages unreadable or its guards impossible, so
    //    `selectedValueJson` is emitted verbatim by CollectJson.
    //  * THE CATALOGUE IS COLLECTED LAZILY, on the first click, not at Rebuild.
    //    A loaded Archicad library is thousands of parts, and paying that
    //    enumeration every time the user merely SELECTS a command in the list
    //    would stall the palette for a picker nobody may open.
    //  * `catalogFilter` is the declared subtype / element_type, passed straight
    //    to the collector. Empty means every kind.
    GS::UniString catalogFilter;
    GS::UniString selectedValueJson;

    // FilePath only: a compact Browse icon before the field. Typing a path by hand
    // stays possible; the button exists because remembering one is not the user's job.
    std::unique_ptr<DG::IconButton> browseButton;

    // Attribute pickers (Layer/Pen/Fill/LineType): Archicad's OWN control, so it
    // lists exactly what the project contains and CANNOT invent an attribute —
    // which is why a layer field must never be a text box (a typo would create a
    // new layer: a bug, not a choice).
    // Every type the picker supports hosts on a DG::PushCheck. Pen is NOT one of
    // them (see UserControlTypeFor) and never reaches here.
    GS::Owner<API_AttributePicker> picker;
    API_AttrTypeID attrType = API_ZombieAttrID;

    // A bounded field states its domain in plain text to the LEFT of the box
    // (e.g. "1-255"), rather than rejecting input silently. Written, not hidden
    // behind a popover: it needs no click, so the constraint is known BEFORE
    // typing. On the left so it never eats into the input box's width.
    std::unique_ptr<DG::LeftText> domainHint;

    // A parameter with no default in run()'s signature. Run stays disabled until
    // every one of these has a usable value, so a command cannot be started in a
    // state it will only fail in.
    bool required = false;

    // True once this control holds something usable. Only meaningful for the
    // kinds where "empty" is possible — an attribute picker that resolved to
    // nothing, or a blank text field.
    bool HasValue () const;
};

// The generated parameter block: one row per scanned parameter, rebuilt from
// scratch every time another command is selected.
//
// There is no per-command .grc — commands are user-authored folders the add-on has
// never seen — so every control is built at runtime from its (Panel&, Rect&)
// constructor. Two things that are easy to get wrong here:
//   * a dynamically created DG item starts HIDDEN — each one must be Show()n, or
//     it exists and holds its value while being invisible. That is why Rebuild()
//     only BUILDS: the shell lays out first, then calls ShowControls(), so nothing
//     flashes in the panel's top-left corner on its way to its real place;
//   * nothing may rely on a construction rect. The panel is resizable and the
//     generated controls change per command, so PlaceAt() recomputes every
//     position from the shell's live band.
//
// The shell owns the palette, the DG event subscription (it is the sole registered
// observer — controls built here attach to IT) and the .grc pen swatch pool, which
// this panel only BORROWS from. See ControlPalette's member list for why the
// destruction order matters.
class ParamPanel {
  public:
    ParamPanel (const DG::Panel& panel, ControlPalette& observer,
                std::vector<std::unique_ptr<DG::UserControl>>& penPool);

    // Builds the runtime-created items this panel owns outright. Called from the
    // shell's constructor BODY, so DG item creation stays in one place.
    void Create ();

    // Scanned metadata -> DG controls, INCLUDING the command's wrapped description
    // (it is the first thing in this band, not the shell's). The controls are left
    // HIDDEN; the shell lays out and then calls ShowControls.
    void Rebuild (const CommandInfo& info);
    void ShowControls ();
    void Clear ();

    // Position the block in the band starting at `top` — description, then the rows
    // (required first, then a rule, then optional) — and return the height used.
    // `top` is a VIRTUAL y and every item reaches the panel through `clip`, the
    // shell's virtual scroll (F4), which offsets it and takes it off the panel when
    // it would fall outside the viewport.
    short PlaceAt (short top, short left, short right, const PaletteScroll& clip);

    // The generated controls read back as the JSON run() is called with.
    GS::UniString CollectJson () const;

    // The param half of the run gate: the first required parameter with nothing
    // usable in it, or empty when they are all satisfied.
    GS::UniString WhatIsMissing () const;

    // Which PARAMETER a DG item belongs to, or empty when the item is not one of
    // the generated controls (or is null). The right-click menu asks this to turn
    // the item under the pointer into a region — a "param:<name>" entry appears
    // over its own control and nowhere else — and it is the only thing that can
    // answer it, because it is the only object that knows which item was built for
    // which port. Matches the row's label as well as its control: the label is as
    // much "that parameter" to a user aiming a pointer as the field is.
    GS::UniString ParamNameAt (const DG::Item* item) const;

    // Event routing. Each returns true when the event belonged to a generated
    // control, so the shell's handler can stop there. Sub-objects never Attach
    // themselves — the shell is the observer these were attached to.
    //
    // `reflow` is set when the event changed WHICH ROWS ARE VISIBLE (F3): the
    // shell then has to Layout + ShowControls + Redraw, because rows appearing and
    // disappearing move everything below them. It is an out-parameter rather than
    // a second call so the shell cannot forget to ask.
    bool HandleCheckItemChanged (const DG::CheckItemChangeEvent& ev, bool& reflow);
    bool HandlePopUpChanged (const DG::PopUpChangeEvent& ev, bool& reflow);
    bool HandleButtonClicked (const DG::ButtonClickEvent& ev, GS::UniString* selectedFilePath = nullptr);

  private:
    // F3 — re-evaluate every show_when against the values the controls hold now,
    // and record the answer in each row's `visible`. Returns true when anything
    // moved, which is exactly when a reflow is owed. The rule evaluation itself is
    // Palette/ParamVisibility's — DevKit-free, and tested offline.
    bool ApplyVisibility ();

    // Word-wraps the command's description into as many LeftText lines as the
    // panel's width needs. DG::LeftText is single-line, so a long description was
    // simply clipped before this existed.

    const DG::Panel& panel;
    ControlPalette& observer;

    // NOT owned: the .grc pen swatches, bound once by the shell and lent out.
    std::vector<std::unique_ptr<DG::UserControl>>& penPool;

    std::vector<ParamControl> paramControls;

    // The wrapped description, one item per line. Built at runtime because how many
    // lines it needs depends on the text and the panel's live width.

    // The section rule between the required and the optional rows. The rule alone
    // carries the grouping — "Required"/"Optional" headings said the same thing in
    // words and cost two lines on a panel that is short of them.
    std::unique_ptr<DG::Separator> groupRule;

    // F3 — the rule under the pinned Action row, so the mode reads as the thing
    // the inputs below it answer to rather than as the first of them. Shown only
    // when the command declares an Action.
    std::unique_ptr<DG::Separator> actionRule;
};

} // namespace evp

#endif
