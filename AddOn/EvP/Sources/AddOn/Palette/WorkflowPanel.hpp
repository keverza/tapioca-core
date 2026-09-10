#ifndef GEOMETRYSERVER_PALETTE_WORKFLOWPANEL_HPP
#define GEOMETRYSERVER_PALETTE_WORKFLOWPANEL_HPP

// The band that shows one Grasshopper workflow's inputs.
//
// ⚠️ IT IS NOT A SECOND ParamPanel, AND THE FIVE-KIND LIMIT IS WHY IT CAN BE
// SEPARATE RATHER THAN A BRANCH INSIDE ONE. ParamPanel serves Python commands:
// eighteen control kinds, Archicad attribute pickers, story indices, navigator
// guids, catalogues, show_when rules, a pen swatch pool it borrows from the
// shell. A Grasshopper workflow declares five primitive types
// (HANDOFF-GHHost.md §13, milestone item 4) and nothing else crosses the pipe.
// Teaching ParamPanel a second source would put a Grasshopper branch through
// every one of those eighteen kinds; teaching this one Archicad attributes would
// be the same mistake pointing the other way. If a workflow ever needs a layer
// picker, the neutral InputModel grows a kind and BOTH panels learn it there.
//
// ⚠️ IT DECIDES NOTHING. Which rows exist, what they are captioned, what their
// domain reads as, whether a typed value is acceptable and what the snapshot
// looks like are all Palette/WorkflowRows, which is DevKit-free and covered by
// tests/cpp/test_workflowrows.cpp. This file turns those rows into DG items and
// reads the items back. Every decision that lives here is one no test can reach.
//
// ⚠️ A SUB-OBJECT: IT ANSWERS THE SHELL AND NEVER DRIVES IT. Same contract as
// ParamPanel, ActionBar and ResultsTable, and tools/quality/check_cpp.py
// enforces it -- nothing here may name ControlPalette.
//
// MAIN THREAD ONLY, like every other DG owner in this folder.

#include "WorkflowRows.hpp"

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "DGModule.hpp"

#include <memory>
#include <vector>

class ControlPalette;

namespace evp {

class PaletteScroll;

// One drawn row: the items, and the row it was built from.
struct WorkflowControl {
    WorkflowRow row;

    std::unique_ptr<DG::LeftText> label;

    // The domain, to the LEFT of the field so it never eats into the input's
    // width. Null when the row declares no bounds.
    std::unique_ptr<DG::LeftText> domainHint;

    // Exactly one of these is built, chosen by the row's kind. A heading builds
    // none of them and only `label`.
    std::unique_ptr<DG::CheckBox> checkBox; // Boolean
    std::unique_ptr<DG::PopUp> popUp;       // Enum
    std::unique_ptr<DG::TextEdit> editText; // Text
    std::unique_ptr<DG::RealEdit> realEdit; // Number
    std::unique_ptr<DG::IntEdit> intEdit;   // Integer

    // A bounded number's slider, beside its field rather than instead of it.
    // Null on every other row. The FIELD is the value; this only writes into it.
    std::unique_ptr<DG::ScrollBar> slider;

    // The live item, whichever kind it turned out to be, or null for a heading.
    DG::Item* Widget () const;

    // What the control currently holds, as the text WorkflowRows validates.
    GS::UniString CurrentText () const;
};

class WorkflowPanel {
  public:
    WorkflowPanel (const DG::Panel& panel, ControlPalette& observer);

    // Builds the items this panel owns. Called from the shell's constructor
    // BODY, so DG item creation stays in one place.
    void Create ();

    // Schema JSON -> rows -> DG controls, left HIDDEN. The shell lays out and
    // then calls ShowControls, so nothing flashes in the panel's top-left corner
    // on its way to its real position.
    //
    // ⚠️ REBUILDING DISCARDS WHAT THE USER TYPED, AND THAT IS CORRECT RATHER
    // THAN LOSSY. A new schema is a different definition (or the same one
    // re-read after an edit); its ids are not promised to be the previous ones,
    // and carrying values across by id would silently apply one definition's
    // numbers to another's inputs.
    void Rebuild (const std::string& schemaJson);

    void ShowControls ();

    // Takes every generated row off the panel, KEEPING them and their values.
    //
    // ⚠️ THIS IS NOT Clear, AND THE DIFFERENCE IS THE BUG IT FIXES. The rows
    // are placed through the shell's virtual scroll, which hides whatever falls
    // outside the viewport -- but only for items it is GIVEN. Selecting another
    // command stops the band being placed at all, so nothing was ever handed to
    // the scroll and every row kept its pixels: a Grasshopper input sitting on
    // top of a Python command's parameters. Clear would have worked and would
    // also have thrown away the schema and everything typed into it, so coming
    // back to the command would have shown an empty panel until the next solve.
    void HideControls ();
    void Clear ();

    // Position the block in the band starting at virtual `top`, returning the
    // height used. Every item reaches the panel through `clip`, the shell's
    // virtual scroll.
    short PlaceAt (short top, short left, short right, const PaletteScroll& clip);

    // Reads the controls back into an input snapshot. `ok` false means at least
    // one row was refused and `errors` says which; the ids and values are still
    // aligned either way.
    WorkflowSnapshot Collect () const;

    // True when the panel holds a schema at all -- an empty definition still
    // counts, because "loaded with no inputs" and "nothing loaded" are different
    // states and only the second one disables Solve.
    bool HasSchema () const;

    // What the parsed schema could not offer, for the shell's status line: the
    // author's own errors, verbatim from discovery.
    const std::vector<std::string>& SchemaErrors () const;

    const std::string& WorkflowName () const;

    // What the definition says about itself, for the description band. Empty
    // when it carries no Tapioca Description component.
    const std::string& WorkflowDescription () const;

    // Marks the rows a snapshot refused, so a failed read-back shows WHERE. The
    // vector is the snapshot's own `refused`, parallel to the rows.
    void MarkRefused (const std::vector<bool>& refused);

    // Event routing. Returns true when the item belonged to one of these
    // controls, so the shell's handler can stop there. Sub-objects never Attach
    // themselves -- the shell is the observer these were attached to.
    bool OwnsItem (const DG::Item* item) const;

    // A slider moved: writes its position into the field beside it and answers
    // true. False means the bar was not one of these -- the shell's own scroll,
    // or the preview band's opacity.
    //
    // ⚠️ IT DOES NOT SOLVE, AND NOTHING HERE DOES. Solve is a button. A slider
    // that solved on every position would be the per-keystroke solve again,
    // sixty times a second instead of five times a word.
    bool FollowSlider (const DG::Item* item);

  private:
    const DG::Panel& panel;
    ControlPalette& observer;

    InputModel model;
    std::vector<WorkflowControl> controls;
    bool hasSchema = false;
};

} // namespace evp

#endif
