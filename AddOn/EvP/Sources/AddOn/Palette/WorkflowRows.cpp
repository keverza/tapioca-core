#include "WorkflowRows.hpp"

namespace evp {

// InputModel's own renderer, not a second one: a bound has to read in the hint
// exactly as it reads in the refusal beside it, and two spellings of one number
// in two messages about the same control is the kind of small inconsistency that
// makes a user doubt both.
using evp::FormatInputNumber;

bool UsesSlider (const WorkflowRow& row)
{
    if (row.isHeading || !row.enabled)
        return false;
    if (row.kind != InputKind::Number && row.kind != InputKind::Integer)
        return false;
    // Both ends, and a domain with width. A min equal to its max is a constant,
    // and a slider whose two ends are the same place is a decoration.
    return row.hasMinimum && row.hasMaximum && row.maximum > row.minimum;
}

int SliderPositionFor (const WorkflowRow& row, const std::string& value)
{
    if (!UsesSlider (row))
        return 0;

    // ⚠️ PARSED WITHOUT THE BOUNDS, WHICH IS THE OPPOSITE OF WHAT THE FIELD
    // DOES, AND BOTH ARE RIGHT. Coerce REFUSES a value outside the domain --
    // that refusal is what shows the user a definition saved with a number its
    // author later narrowed out. But a refusal has no slider position, and
    // parking at the minimum would put a too-LARGE value at the bottom of the
    // bar. So the number is read on its own and then clamped to the end it is
    // past: the bar says "beyond here", the field says what it really is.
    InputControl lenient = ControlOf (row);
    lenient.hasMinimum = false;
    lenient.hasMaximum = false;

    const CoerceResult parsed = Coerce (lenient, value);
    if (!parsed.ok)
        return 0;

    const double number = std::atof (parsed.value.c_str ());
    const double span = row.maximum - row.minimum;
    const double fraction = (number - row.minimum) / span;

    // Clamped rather than refused: a definition saved with a value outside
    // bounds its author narrowed afterwards is the ordinary cause, and the field
    // beside the slider still shows -- and refuses -- the real number. The
    // slider parks at the end it is past.
    if (fraction <= 0.0)
        return 0;
    if (fraction >= 1.0)
        return WorkflowSliderSteps;
    return (int) (fraction * WorkflowSliderSteps + 0.5);
}

std::string SliderValueAt (const WorkflowRow& row, int position)
{
    if (!UsesSlider (row))
        return std::string ();

    const int clamped = position < 0 ? 0 : (position > WorkflowSliderSteps ? WorkflowSliderSteps : position);
    const double span = row.maximum - row.minimum;
    double number = row.minimum + span * ((double) clamped / (double) WorkflowSliderSteps);

    if (row.kind == InputKind::Integer) {
        // Rounded, not truncated: half a step short of an integer is the value
        // the user was aiming at, and truncation would make the top of the
        // domain unreachable.
        number = (double) (long long) (number < 0.0 ? number - 0.5 : number + 0.5);
    }

    return FormatInputNumber (number);
}

InputControl ControlOf (const WorkflowRow& row)
{
    InputControl control;
    control.id = row.id;
    control.label = row.label;
    control.kind = row.kind;
    control.choices = row.choices;
    control.required = row.required;
    control.hasMinimum = row.hasMinimum;
    control.minimum = row.minimum;
    control.hasMaximum = row.hasMaximum;
    control.maximum = row.maximum;
    control.defaultValue = row.initialValue;
    return control;
}

std::string DescribeDomain (const InputControl& control)
{
    // Only the numeric kinds have a domain worth writing. An enum's domain is
    // its popup, and a boolean's is a checkbox: spelling either out would be a
    // sentence restating the control the user is looking at.
    if (control.kind != InputKind::Number && control.kind != InputKind::Integer)
        return std::string ();

    if (control.hasMinimum && control.hasMaximum)
        return FormatInputNumber (control.minimum) + "-" + FormatInputNumber (control.maximum);
    if (control.hasMinimum)
        return "at least " + FormatInputNumber (control.minimum);
    if (control.hasMaximum)
        return "at most " + FormatInputNumber (control.maximum);
    return std::string ();
}

std::vector<WorkflowRow> BuildWorkflowRows (const InputModel& model)
{
    std::vector<WorkflowRow> rows;
    rows.reserve (model.controls.size () + 4);

    std::string currentGroup;
    bool first = true;

    for (const InputControl& control : model.controls) {
        // A heading whenever the group CHANGES, which is enough because the
        // model is already sorted by group -- and if it ever were not, a heading
        // per change is still an honest description of what follows it.
        if (first || control.group != currentGroup) {
            currentGroup = control.group;
            first = false;
            if (!currentGroup.empty ()) {
                WorkflowRow heading;
                heading.isHeading = true;
                heading.label = currentGroup;
                rows.push_back (std::move (heading));
            }
        }

        WorkflowRow row;
        row.id = control.id;
        row.label = control.label.empty () ? control.id : control.label;
        row.kind = control.kind;
        row.declaredType = control.declaredType;
        row.choices = control.choices;
        row.required = control.required;
        row.hasMinimum = control.hasMinimum;
        row.minimum = control.minimum;
        row.hasMaximum = control.hasMaximum;
        row.maximum = control.maximum;
        row.domainHint = DescribeDomain (control);
        row.enabled = control.kind != InputKind::Unsupported;

        if (row.enabled) {
            const CoerceResult accepted = Coerce (control, control.defaultValue);
            // The raw default either way: showing the user a value their
            // definition does not hold, because this panel preferred a tidy one,
            // would be a lie about the document.
            row.initialValue = control.defaultValue;
            row.initialValueRefused = !accepted.ok;
            if (accepted.ok)
                row.initialValue = accepted.value;
        }
        else {
            row.initialValue = control.defaultValue;
            // Not marked refused: the value is not wrong, it is unreachable, and
            // a row flagged as bad would send the user looking for a typo.
            row.initialValueRefused = false;
            row.label += " (" + control.declaredType + ")";
        }

        rows.push_back (std::move (row));
    }

    return rows;
}

WorkflowSnapshot ReadWorkflowRows (const std::vector<WorkflowRow>& rows, const std::vector<std::string>& texts)
{
    WorkflowSnapshot snapshot;
    snapshot.refused.assign (rows.size (), false);

    if (texts.size () != rows.size ()) {
        snapshot.errors.push_back ("The workflow panel read back " + std::to_string (texts.size ()) + " values for " +
                                   std::to_string (rows.size ()) + " rows.");
        return snapshot;
    }

    bool ok = true;
    for (size_t index = 0; index < rows.size (); ++index) {
        const WorkflowRow& row = rows[index];
        if (row.isHeading)
            continue;

        if (!row.enabled) {
            // An unsupported input is not sent at all. Sending its saved text
            // would be a value the definition never asked this panel for, and
            // refusing the whole snapshot over it would make the definition
            // unrunnable because of one input nobody can set.
            continue;
        }

        const InputControl control = ControlOf (row);
        const CoerceResult result = Coerce (control, texts[index]);
        snapshot.ids.push_back (row.id);
        // The RAW text on refusal, so the caller can show back exactly what was
        // typed, and the normalised text otherwise.
        snapshot.values.push_back (result.ok ? result.value : texts[index]);
        if (!result.ok) {
            snapshot.errors.push_back (result.error);
            snapshot.refused[index] = true;
            ok = false;
        }
    }

    snapshot.ok = ok;
    return snapshot;
}

} // namespace evp
