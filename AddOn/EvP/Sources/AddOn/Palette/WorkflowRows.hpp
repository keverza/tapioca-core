#ifndef GEOMETRYSERVER_PALETTE_WORKFLOWROWS_HPP
#define GEOMETRYSERVER_PALETTE_WORKFLOWROWS_HPP

// What the workflow panel actually puts on screen: one row per input, group
// headings between them, and the read-back that turns the controls back into an
// input snapshot.
//
// DELIBERATELY FREE OF THE DEVKIT, like Palette/ParamLayout and
// Palette/ParamVisibility beside it, and for the same reason those are: the
// decisions here are the ones that go wrong SILENTLY. A row built for the wrong
// input sends the user's value to another parameter. A heading emitted for a
// group that has no rows leaves a title floating over nothing. A read-back that
// loses its alignment with the rows sends every value one place along — and the
// definition still solves, and the answer is still plausible.
//
// Palette/WorkflowPanel builds DG items from these and reads them back through
// them; it decides nothing.
//
// ⚠️ THIS IS NOT A SECOND ParamPanel AND IT MUST NOT GROW INTO ONE. ParamPanel
// serves Python commands: eighteen control kinds, attribute pickers, story
// indices, navigator guids, catalogues, show_when. A Grasshopper workflow
// declares five primitive types (HANDOFF-GHHost.md §13, milestone item 4) and
// nothing else crosses the pipe. When a sixth is added it is added to
// InputModel's neutral vocabulary first, not here.

#include "InputModel.hpp"

#include <string>
#include <vector>

namespace evp {

// How many positions a slider has between its two ends.
//
// ⚠️ THE SLIDER IS NOT THE VALUE, IT IS A COARSE WAY TO REACH ONE. DG's bar
// controls carry an Int32, so a 0..1 domain has to be quantised to something --
// and a thousand steps is finer than the pixels a palette-width bar has, so the
// quantisation is invisible while the number beside it stays exact. The EDIT is
// authoritative: anything the slider cannot express is still typeable.
constexpr int WorkflowSliderSteps = 1000;

// One row of the workflow band
struct WorkflowRow {
    // A heading carries only `label` and is not a control. Rows and headings are
    // one list rather than two, because their ORDER is the thing being
    // described and two lists would let them disagree.
    bool isHeading = false;

    // Empty for a heading. This is what the read-back keys the value by, and it
    // is never shown.
    std::string id;

    // What the user reads beside the control.
    std::string label;

    // The domain, spelled out to the left of the field ("1-40", "at least 0.5").
    // Written rather than hidden behind a popover, and rather than left to a
    // refusal after the fact: it needs no click, so the constraint is known
    // BEFORE typing. Empty when the input declares no bounds.
    std::string domainHint;

    InputKind kind = InputKind::Text;

    // The bounds the input declared, carried rather than re-derived from
    // `domainHint`.
    //
    // ⚠️ THE HINT IS FOR READING AND THESE ARE FOR CHECKING, AND THE TWO MUST
    // NEVER SWAP JOBS. Parsing the bounds back out of "1-40" to validate against
    // them would make a display string load-bearing: it is lossy on a negative
    // minimum, it has no spelling at all for a one-sided bound that reads "at
    // least 0.5", and a change to the wording would silently change what the
    // panel accepts.
    bool hasMinimum = false;
    double minimum = 0.0;
    bool hasMaximum = false;
    double maximum = 0.0;

    // Enum rows in author order; empty for every other kind.
    std::vector<std::string> choices;

    // What the control starts holding.
    //
    // ⚠️ THE SCHEMA'S DEFAULT IS NOT ASSUMED VALID. A definition can be saved
    // with a slider outside bounds its author narrowed afterwards, and a panel
    // that trusted the default would open with a value it would then refuse to
    // send, with nothing on screen explaining why Solve was disabled.
    // `initialValueRefused` says the default did not pass, so the panel can mark
    // the row instead of looking broken.
    std::string initialValue;
    bool initialValueRefused = false;

    // A control the panel builds but cannot offer a value for: an unsupported
    // type. Shown, disabled, and labelled with what it asks for -- a row that
    // vanished would look like a definition with fewer inputs.
    bool enabled = true;

    bool required = false;
};

// Whether this row is drawn with a slider beside its field.
//
// A bounded number is a RANGE, and a range with both ends known is the one case
// a slider is better at than a field: the author has already said what the
// sensible values are, so dragging cannot leave them. An unbounded number gets
// no slider, because a slider with an invented end would be inventing the
// author's intent.
bool UsesSlider (const WorkflowRow& row);

// The slider position that stands for `value`, clamped into the row's domain.
// A value that does not parse answers with the minimum's position rather than
// with nothing: a slider has to be somewhere.
int SliderPositionFor (const WorkflowRow& row, const std::string& value);

// The value at a slider position, spelled as the field would hold it -- an
// integer row lands on whole numbers, and both go through FormatInputNumber so
// the text has exactly one spelling per number.
std::string SliderValueAt (const WorkflowRow& row, int position);

// Turns one parsed schema into the panel's row list.
//
// A heading is emitted whenever the group changes and never for a group with no
// rows. The ungrouped rows come first with no heading at all: a heading over
// every row of a definition whose author never used groups would be noise per
// row rather than structure.
std::vector<WorkflowRow> BuildWorkflowRows (const InputModel& model);

// The result of reading the controls back.
struct WorkflowSnapshot {
    bool ok = false;

    // Parallel arrays, id and value, in row order. The pipe wants exactly this,
    // and building it here rather than at the DG layer keeps the one place that
    // knows which value belongs to which id away from the one that knows what a
    // DG::EditText is.
    std::vector<std::string> ids;
    std::vector<std::string> values;

    // One sentence per refused row, each naming the row's LABEL.
    std::vector<std::string> errors;

    // Which rows were refused, parallel to the ROW list (headings included, and
    // always false for those), so the panel can mark them without re-deriving
    // the mapping.
    std::vector<bool> refused;
};

// Reads the controls back. `texts` is parallel to `rows` -- a heading's entry is
// ignored and may be empty.
//
// ⚠️ A REFUSED ROW STILL CONTRIBUTES ITS id AND ITS RAW TEXT. Dropping it would
// shorten the arrays and shift every id after it against its value, which is the
// alignment bug this whole struct exists to make impossible. `ok` is what a
// caller gates the solve on, never the array lengths.
WorkflowSnapshot ReadWorkflowRows (const std::vector<WorkflowRow>& rows, const std::vector<std::string>& texts);

// The declaration a row was built from, reassembled from the row's own fields.
//
// ⚠️ ONE VALIDATOR FOR THE PANEL AND THE WIRE, AND THIS IS WHAT KEEPS IT ONE. A
// second copy of the rules at the DG layer would be a second thing to keep in
// step with InputModel, and the first divergence would be a value the panel
// accepted and the definition refused.
InputControl ControlOf (const WorkflowRow& row);

// The domain sentence for one control, or empty when it declares no bounds.
// Exposed for the tests and for a caller that wants the hint without a row.
std::string DescribeDomain (const InputControl& control);

} // namespace evp

#endif
