#include "WorkflowPanel.hpp"

#include "ControlPalette.hpp"
#include "PaletteMetrics.hpp"
#include "PaletteScroll.hpp"
#include "ParamLayout.hpp"

#include <cstdlib>

namespace evp {

using namespace evp::palette;

namespace {

// The domain hint's column, to the LEFT of the input so it never eats into the
// field's width. Same reasoning as ParamPanel's.
constexpr short DomainWidth = 66;

GS::UniString ToUniString (const std::string& text)
{
    return GS::UniString (text.c_str (), CC_UTF8);
}

std::string ToStdString (const GS::UniString& text)
{
    return std::string (text.ToCStr (0, MaxUSize, CC_UTF8).Get ());
}

} // namespace

// The field's width when a slider shares the row with it. Wide enough for a
// number and a sign, narrow enough to leave the bar something to drag.
constexpr short SliderFieldWidth = 62;

DG::Item* WorkflowControl::Widget () const
{
    if (checkBox)
        return checkBox.get ();
    if (popUp)
        return popUp.get ();
    if (editText)
        return editText.get ();
    if (realEdit)
        return realEdit.get ();
    if (intEdit)
        return intEdit.get ();
    return nullptr;
}

GS::UniString WorkflowControl::CurrentText () const
{
    if (checkBox)
        return checkBox->IsChecked () ? "true" : "false";

    if (popUp) {
        const short selected = popUp->GetSelectedItem ();
        if (selected < 1 || (size_t) selected > row.choices.size ())
            return GS::UniString ();
        return ToUniString (row.choices[(size_t) (selected - 1)]);
    }

    if (editText)
        return editText->GetText ();

    // ⚠️ RENDERED THROUGH FormatInputNumber, NOT THROUGH DG's OWN FORMATTING.
    // The value that leaves here is hashed into the input snapshot, so it has to
    // have exactly one spelling per number; a locale-formatted "3,5" would also
    // be refused outright by the parser on the other side.
    if (realEdit)
        return ToUniString (FormatInputNumber (realEdit->GetValue ()));
    if (intEdit)
        return ToUniString (FormatInputNumber ((double) intEdit->GetValue ()));

    return GS::UniString ();
}

WorkflowPanel::WorkflowPanel (const DG::Panel& hostPanel, ControlPalette& shellObserver)
    : panel (hostPanel), observer (shellObserver)
{
}

void WorkflowPanel::Create ()
{
    // Nothing is built until a schema arrives: this band has no fixed items at
    // all, unlike ParamPanel's, because a workflow has no header row of its own.
}

void WorkflowPanel::Clear ()
{
    controls.clear ();
    model = InputModel ();
    hasSchema = false;
}

bool WorkflowPanel::HasSchema () const
{
    return hasSchema;
}

const std::vector<std::string>& WorkflowPanel::SchemaErrors () const
{
    return model.errors;
}

const std::string& WorkflowPanel::WorkflowName () const
{
    return model.name;
}

const std::string& WorkflowPanel::WorkflowDescription () const
{
    return model.description;
}

void WorkflowPanel::Rebuild (const std::string& schemaJson)
{
    Clear ();

    model = ParseInputModel (schemaJson);
    hasSchema = true;

    const std::vector<WorkflowRow> rows = BuildWorkflowRows (model);

    // Positions are assigned by PlaceAt; this rect is only a placeholder, and
    // nothing may rely on it -- the panel is resizable and the rows change per
    // definition.
    const DG::Rect seed (Margin, 0, Margin + 100, RowHeight);

    for (const WorkflowRow& row : rows) {
        WorkflowControl control;
        control.row = row;

        control.label = std::make_unique<DG::LeftText> (panel, seed);
        control.label->SetText (ToUniString (row.label));

        if (row.isHeading) {
            controls.push_back (std::move (control));
            continue;
        }

        // Not on a slider row: the bar's two ends already say the domain, and a
        // caption repeating it would only be taking width off the bar.
        if (!row.domainHint.empty () && !UsesSlider (row)) {
            control.domainHint = std::make_unique<DG::LeftText> (panel, seed);
            control.domainHint->SetText (ToUniString (row.domainHint));
        }

        switch (row.kind) {
            case InputKind::Boolean: {
                auto box = std::make_unique<DG::CheckBox> (panel, seed);
                box->SetText ("");
                box->SetState (row.initialValue == "true");
                box->Attach (observer);
                control.checkBox = std::move (box);
                break;
            }

            case InputKind::Enum: {
                auto popup = std::make_unique<DG::PopUp> (panel, seed, RowHeight, 0);
                for (size_t index = 0; index < row.choices.size (); ++index) {
                    popup->AppendItem ();
                    popup->SetItemText ((short) (index + 1), ToUniString (row.choices[index]));
                    if (row.choices[index] == row.initialValue)
                        popup->SelectItem ((short) (index + 1));
                }
                if (row.choices.empty ()) {
                    // An enum with no choices is already reported in the
                    // schema's errors; the row says so where the user is looking
                    // rather than only in a list above.
                    popup->AppendItem ();
                    popup->SetItemText (1, "(this input declares no choices)");
                    popup->Disable ();
                }
                else if (popup->GetSelectedItem () < 1) {
                    popup->SelectItem (1);
                }
                popup->Attach (observer);
                control.popUp = std::move (popup);
                break;
            }

            case InputKind::Integer: {
                auto edit = std::make_unique<DG::IntEdit> (panel, seed);
                // ⚠️ THE WIDGET IS NOT GIVEN THE BOUNDS, DELIBERATELY.
                // DG::IntEdit CLAMPS to its min/max, so a definition saved with
                // a value outside bounds its author narrowed afterwards would be
                // shown a number the document does not hold, silently. The
                // domain is written beside the field instead, and WorkflowRows
                // refuses the value -- which is visible, and true.
                Int32 value = 0;
                const CoerceResult parsed = Coerce (ControlOf (row), row.initialValue);
                if (parsed.ok)
                    value = (Int32) std::atol (parsed.value.c_str ());
                edit->SetValue (value);
                // NOT attached, like ParamPanel's own numeric edits: DG's
                // IntEdit/RealEdit observers are their own types and the shell is
                // not one of them. The shell reads this band back on its idle
                // tick instead, which is also what feeds the controller's settle
                // window -- a keystroke-by-keystroke event would restart that
                // window on every digit of a number being typed.
                control.intEdit = std::move (edit);
                break;
            }

            case InputKind::Number: {
                auto edit = std::make_unique<DG::RealEdit> (panel, seed);
                double value = 0.0;
                const CoerceResult parsed = Coerce (ControlOf (row), row.initialValue);
                if (parsed.ok)
                    value = std::atof (parsed.value.c_str ());
                edit->SetValue (value);
                control.realEdit = std::move (edit);
                break;
            }

            case InputKind::Text:
            case InputKind::Unsupported:
            default: {
                auto edit = std::make_unique<DG::TextEdit> (panel, seed);
                edit->SetText (ToUniString (row.initialValue));
                edit->Attach (observer);
                control.editText = std::move (edit);
                break;
            }
        }

        // The slider, for a row whose author declared both ends. Built AFTER
        // the field so it can be set from what the field ended up holding --
        // which is the coerced value, not the raw text.
        if (UsesSlider (row)) {
            auto bar = std::make_unique<DG::ScrollBar> (panel, seed, DG::ScrollBar::Normal, DG::ScrollBar::Focusable,
                                                        DG::ScrollBar::NoAutoScroll);
            bar->SetMin (0);
            bar->SetMax (WorkflowSliderSteps);
            // A page is a twentieth of the domain: clicking the trough should
            // move a useful amount, and a page of one step makes the trough
            // behave like the arrows.
            bar->SetPageSize (WorkflowSliderSteps / 20);
            bar->SetValue (SliderPositionFor (row, row.initialValue));
            bar->Attach (observer);
            control.slider = std::move (bar);
        }

        // An unsupported input is shown, captioned with the type it asks for,
        // and locked. A row that vanished would look like a definition with
        // fewer inputs.
        if (!row.enabled) {
            if (DG::Item* widget = control.Widget ())
                widget->Disable ();
        }

        controls.push_back (std::move (control));
    }
}

void WorkflowPanel::ShowControls ()
{
    // ⚠️ A DYNAMICALLY CREATED DG ITEM STARTS HIDDEN. Each one must be Show()n
    // or it exists and holds its value while being invisible -- the same trap
    // ParamPanel documents, and the reason Rebuild only BUILDS.
    for (WorkflowControl& control : controls) {
        if (control.label)
            control.label->Show ();
        if (control.domainHint)
            control.domainHint->Show ();
        if (control.slider)
            control.slider->Show ();
        if (DG::Item* widget = control.Widget ())
            widget->Show ();
    }
}

void WorkflowPanel::HideControls ()
{
    for (WorkflowControl& control : controls) {
        if (control.label && control.label->IsVisible ())
            control.label->Hide ();
        if (control.domainHint && control.domainHint->IsVisible ())
            control.domainHint->Hide ();
        if (control.slider && control.slider->IsVisible ())
            control.slider->Hide ();
        if (DG::Item* widget = control.Widget ()) {
            if (widget->IsVisible ())
                widget->Hide ();
        }
    }
}

short WorkflowPanel::PlaceAt (short top, short left, short right, const PaletteScroll& clip)
{
    short y = top;
    const short contentWidth = (short) (right - left);
    const short inputWidth = (short) InputColumnWidth (contentWidth);

    for (WorkflowControl& control : controls) {
        if (control.row.isHeading) {
            // A heading spans the whole content width and gets the gap above it
            // rather than below, so it reads as belonging to the rows it
            // introduces rather than to the ones it follows.
            if (&control != &controls.front ())
                y = (short) (y + RowGap);
            clip.Place (control.label.get (), DG::Rect (left, y, right, (short) (y + RowHeight)));
            y = (short) (y + RowHeight + RowGap);
            continue;
        }

        const short inputLeft = (short) (right - inputWidth);
        const short hintLeft = (short) (inputLeft - DomainWidth);
        clip.Place (control.label.get (), DG::Rect (left, y, hintLeft, (short) (y + RowHeight)));
        if (control.domainHint)
            clip.Place (control.domainHint.get (), DG::Rect (hintLeft, y, inputLeft, (short) (y + RowHeight)));

        if (control.slider) {
            // The bar takes the domain column AND the field's own width, less
            // the narrow field it writes into: a slider the width of a spinner
            // would be worse than the spinner. The domain hint is null on these
            // rows -- the two ends of the bar say the same thing.
            const short fieldLeft = (short) (right - SliderFieldWidth);
            clip.Place (control.slider.get (),
                        DG::Rect (hintLeft, y, (short) (fieldLeft - 4), (short) (y + RowHeight)));
            if (DG::Item* widget = control.Widget ())
                clip.Place (widget, DG::Rect (fieldLeft, y, right, (short) (y + RowHeight)));
            y = (short) (y + RowHeight + RowGap);
            continue;
        }

        if (DG::Item* widget = control.Widget ())
            clip.Place (widget, DG::Rect (inputLeft, y, right, (short) (y + RowHeight)));

        y = (short) (y + RowHeight + RowGap);
    }

    return (short) (y - top);
}

WorkflowSnapshot WorkflowPanel::Collect () const
{
    std::vector<WorkflowRow> rows;
    std::vector<std::string> texts;
    rows.reserve (controls.size ());
    texts.reserve (controls.size ());

    for (const WorkflowControl& control : controls) {
        rows.push_back (control.row);
        texts.push_back (control.row.isHeading ? std::string () : ToStdString (control.CurrentText ()));
    }

    return ReadWorkflowRows (rows, texts);
}

void WorkflowPanel::MarkRefused (const std::vector<bool>& refused)
{
    for (size_t index = 0; index < controls.size (); ++index) {
        DG::LeftText* label = controls[index].label.get ();
        if (label == nullptr || controls[index].row.isHeading)
            continue;

        const bool bad = index < refused.size () && refused[index];
        // The caption carries the mark. There is no per-item colour on a
        // DG::LeftText, and a dialog listing the offending rows somewhere else
        // would make the user match names to fields by hand.
        const GS::UniString base = ToUniString (controls[index].row.label);
        label->SetText (bad ? GS::UniString ("! ") + base : base);
    }
}

bool WorkflowPanel::FollowSlider (const DG::Item* item)
{
    if (item == nullptr)
        return false;

    for (WorkflowControl& control : controls) {
        if (control.slider.get () != item)
            continue;

        const std::string value = SliderValueAt (control.row, (int) control.slider->GetValue ());
        if (control.realEdit)
            control.realEdit->SetValue (std::atof (value.c_str ()));
        else if (control.intEdit)
            control.intEdit->SetValue ((Int32) std::atol (value.c_str ()));
        return true;
    }

    return false;
}

bool WorkflowPanel::OwnsItem (const DG::Item* item) const
{
    if (item == nullptr)
        return false;

    for (const WorkflowControl& control : controls) {
        if (control.Widget () == item || control.label.get () == item || control.domainHint.get () == item ||
            control.slider.get () == item)
            return true;
    }
    return false;
}

} // namespace evp
