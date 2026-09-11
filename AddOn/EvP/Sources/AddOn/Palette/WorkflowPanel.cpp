#include "WorkflowPanel.hpp"

#include "ControlPalette.hpp"
#include "PaletteMetrics.hpp"
#include "PaletteScroll.hpp"
#include "ParamLayout.hpp"

#include "NativeCommands/SelectionSetStore.hpp" // a selection row's value lives here
#include "AddOnCommands.hpp"                    // ExecuteNativeCommand, for the selection verbs

#include "Python/PathUtils.hpp"             // AppendTextLine / ScanLogPath - why a row fell back
#include "Palette/AttributePickerTypes.hpp" // UserControlTypeFor - which Archicad picker lists a type
#include "NativeCommands/CommandUtils.hpp"  // AttributeNameToIndex / AttributeIndexToName

#include <cstring>

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
    if (attributeHost)
        return attributeHost.get ();
    if (attributePopUp)
        return attributePopUp.get ();
    return nullptr;
}

namespace {

// Every guid the named selection set holds, newline-joined -- the wire format
// TapiocaSelectionParam splits on the other side.
//
// ⚠️ READ LIVE RATHER THAN CACHED, because the buttons that change it are
// the SelectionSetPanel's and this band never hears them. Reading the store at
// the moment a snapshot is taken is what makes "press Add, press Solve" mean
// what it says.
// The first attribute of a type, and its name.
//
// ⚠️ AN UNSET INDEX READS AS "Missing" IN ARCHICAD'S OWN PICKER. The picker is
// handed an API_AttributeIndex and shows whatever that index resolves to; a
// freshly placed input carries no default, the name lookup therefore fails, and
// a picker nobody set opens on an index that resolves to nothing -- which
// Archicad labels Missing. That looked like a broken row rather than an unset
// one, and it is the whole reason this function exists: a picker with no
// declared default opens on something the project actually contains.
//
// Not a guess at a good default -- the FIRST is chosen precisely because it
// carries no opinion, and the log says the row opened on it.
bool FirstAttributeOf (API_AttrTypeID type, API_AttributeIndex& index, GS::UniString& name)
{
    GS::Array<API_Attribute> attributes;
    if (ACAPI_Attribute_GetAttributesByType (type, attributes) != NoError || attributes.IsEmpty ())
        return false;

    index = attributes[0].header.index;
    name = GS::UniString (attributes[0].header.name);
    return true;
}

GS::UniString SelectionValue (const std::string& role)
{
    const GS::Array<GS::UniString> guids = geomsrv::SelectionSetStore::Get ().Values (ToUniString (role));
    GS::UniString joined;
    for (UIndex index = 0; index < guids.GetSize (); ++index) {
        if (index > 0)
            joined += "\n";
        joined += guids[index];
    }
    return joined;
}

} // namespace

GS::UniString SelectionCount (const std::string& role)
{
    const GS::Array<GS::UniString> guids = geomsrv::SelectionSetStore::Get ().Values (ToUniString (role));
    if (guids.IsEmpty ())
        return GS::UniString ("none");
    if (guids.GetSize () == 1)
        return GS::UniString ("1 element");
    return GS::UniString::Printf ("%u elements", (unsigned int) guids.GetSize ());
}

// ⚠️ THE SAME TWO COMMANDS SelectionSetPanel USES, and deliberately not a
// reimplementation: ModifySelectionSet with current:true takes Archicad's live
// selection, and ReselectSelectionSet puts a set back into it. The store is
// Configured from this definition's own selection inputs, so a role that is not
// one of them is refused there rather than invented here.
bool RunSelectionVerb (const std::string& role, const char* op)
{
    GS::ObjectState params;
    params.Add ("name", ToUniString (role));

    GS::String command;
    if (op == nullptr) {
        command = "ReselectSelectionSet";
    }
    else {
        command = "ModifySelectionSet";
        params.Add ("op", GS::UniString (op));
        // "current" is what makes update/add/remove read the live selection;
        // clear needs no input and must not claim to have read one.
        if (std::strcmp (op, "clear") != 0)
            params.Add ("current", true);
    }

    return geomsrv::ExecuteNativeCommand (command, params).ok;
}

GS::UniString WorkflowControl::CurrentText () const
{
    // ⚠️ BEFORE THE WIDGET CHECKS, because a selection row HAS no widget
    // holding its value -- the caption is a display and the store is the truth.
    if (row.kind == InputKind::Selection)
        return SelectionValue (row.id);

    // ⚠️ THE NAME, NOT THE INDEX, for the reason ParamReadback gives for the same
    // control: a name matches the declared default, reads well in a log and
    // survives being written down, while an index means nothing in another
    // project. A definition resolves name -> index when it needs to.
    if (picker != nullptr)
        return geomsrv::AttributeIndexToName (attrType, picker->GetSelectedAttributeIndex ());

    if (attributePopUp) {
        const short selected = attributePopUp->GetSelectedItem ();
        if (selected < 1 || (USize) selected > attributeChoices.GetSize ())
            return GS::UniString ();
        return attributeChoices[(USize) (selected - 1)];
    }

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

            case InputKind::Selection: {
                // ⚠️ CAPTIONED, NOT ICONS, AND THE ROW GAINED A SECOND LINE
                // TO PAY FOR IT. Five glyphs in one input column were compact and
                // unreadable: nothing distinguishes "add to the selection" from
                // "take the current selection" pictorially, and getting it wrong
                // silently changes what a headless run will read. The words say
                // it. PlaceAt spreads them over two lines across the full width
                // rather than squeezing five captions into one field's worth.
                auto button = [&] (const char* caption) {
                    auto made = std::make_unique<DG::Button> (panel, seed);
                    made->SetText (caption);
                    made->Attach (observer);
                    return made;
                };

                control.selectionUpdate = button ("Update");
                control.selectionAdd = button ("Add");
                control.selectionRemove = button ("Remove");
                control.selectionReselect = button ("Reselect");
                control.selectionClear = button ("Clear");

                // In the domain column, which a selection row never uses: the
                // count belongs beside the buttons that change it.
                control.selectionCount = std::make_unique<DG::LeftText> (panel, seed);
                control.selectionCount->SetText (SelectionCount (row.id));
                break;
            }

            case InputKind::Attribute: {
                API_UserControlType controlType = APIUserControlType_Layer;
                // The subtype, not the whole declared name: the prefix is how the
                // DevKit-free model recognised this row, and the table knows
                // only Archicad's own type names.
                const std::string subtype = AttributeSubtypeOf (row.declaredType);
                if (!UserControlTypeFor (ToUniString (subtype), controlType, control.attrType)) {
                    // The kind was decided by the same table, so this cannot
                    // happen -- and if the two ever disagree, a text field is a
                    // worse answer than an empty picker. Fall through to the
                    // default row rather than pretend, and SAY SO: a silent
                    // fallback here is indistinguishable from a panel that never
                    // understood the input at all.
                    AppendTextLine (ScanLogPath (),
                                    GS::UniString::Printf ("  workflow input '%T': declared type '%T' has no entry in "
                                                           "AttributePickerTypes, so the row is a plain text field. "
                                                           "The spelling must match that table exactly.",
                                                           ToUniString (row.id).ToPrintf (),
                                                           ToUniString (row.declaredType).ToPrintf ()));
                    auto edit = std::make_unique<DG::TextEdit> (panel, seed);
                    edit->SetText (ToUniString (row.initialValue));
                    edit->Attach (observer);
                    control.editText = std::move (edit);
                    break;
                }

                auto host = std::make_unique<DG::PushCheck> (panel, seed);

                API_AttributePickerParams params;
                params.type = controlType;
                params.dialogID = panel.GetId ();
                params.itemID = host->GetId ();
                params.pushCheckAppearance = API_AttributePickerParams::PushCheckAppearance::ArrowIconAndText;

                const bool created =
                    ACAPI_Dialog_CreateAttributePicker (params, control.picker) == NoError && control.picker != nullptr;
                if (created) {
                    // ⚠️ THE PICKER IS ALWAYS GIVEN AN INDEX THAT RESOLVES, and
                    // the declared default is only the first choice of three. An
                    // input placed on the canvas a moment ago has no default at
                    // all; a name typed against another project no longer
                    // resolves in this one. Either way an unset picker shows
                    // "Missing", which reads as a broken row -- so the fallback
                    // is the project's first attribute of the type, and the log
                    // says which of the three happened.
                    const GS::UniString wanted = ToUniString (row.initialValue);
                    API_AttributeIndex index;
                    GS::UniString opened;

                    if (!wanted.IsEmpty () && geomsrv::AttributeNameToIndex (control.attrType, wanted, index)) {
                        control.picker->SetSelectedAttributeIndex (index);
                    }
                    else if (FirstAttributeOf (control.attrType, index, opened)) {
                        control.picker->SetSelectedAttributeIndex (index);
                        AppendTextLine (ScanLogPath (),
                                        GS::UniString::Printf ("  workflow input '%T' (%T): opened on '%T' because "
                                                               "the declared default '%T' does not name a %T in this "
                                                               "project. An unset picker would have shown Missing.",
                                                               ToUniString (row.id).ToPrintf (),
                                                               ToUniString (subtype).ToPrintf (), opened.ToPrintf (),
                                                               wanted.ToPrintf (), ToUniString (subtype).ToPrintf ()));
                    }
                    else {
                        AppendTextLine (ScanLogPath (),
                                        GS::UniString::Printf ("  workflow input '%T': this project contains no %T "
                                                               "attributes at all, so its picker has nothing to open "
                                                               "on and will read as Missing.",
                                                               ToUniString (row.id).ToPrintf (),
                                                               ToUniString (subtype).ToPrintf ()));
                    }

                    control.attributeHost = std::move (host);
                    break;
                }

                // ⚠️ THE SAME FALLBACK ParamPanel HAS, AND FOR THE SAME REASON:
                // a popup listing what the project ACTUALLY contains. Still no
                // typing, so still no invented attribute -- just not Archicad's
                // own widget. ParamPanel learned this the hard way by asking the
                // picker for a type that is not on its supported list.
                host.reset ();
                auto popup = std::make_unique<DG::PopUp> (panel, seed, RowHeight, 0);
                GS::Array<API_Attribute> attributes;
                if (ACAPI_Attribute_GetAttributesByType (control.attrType, attributes) == NoError) {
                    for (const API_Attribute& attribute : attributes)
                        control.attributeChoices.Push (GS::UniString (attribute.header.name));
                }
                for (USize index = 0; index < control.attributeChoices.GetSize (); ++index) {
                    popup->AppendItem ();
                    popup->SetItemText ((short) (index + 1), control.attributeChoices[index]);
                    if (control.attributeChoices[index] == ToUniString (row.initialValue))
                        popup->SelectItem ((short) (index + 1));
                }
                if (control.attributeChoices.IsEmpty ()) {
                    popup->AppendItem ();
                    popup->SetItemText (1, "(none in this project)");
                    popup->Disable ();
                }
                AppendTextLine (ScanLogPath (), GS::UniString::Printf (
                                                    "  workflow input '%T' (%T): "
                                                    "ACAPI_Dialog_CreateAttributePicker REFUSED the type, so the "
                                                    "row lists %u project attribute(s) in a popup instead. The "
                                                    "requested API_UserControlType is probably not on the "
                                                    "supported list in API_AttributePickerParams.",
                                                    ToUniString (row.id).ToPrintf (), ToUniString (subtype).ToPrintf (),
                                                    (unsigned) control.attributeChoices.GetSize ()));
                popup->Attach (observer);
                control.attributePopUp = std::move (popup);
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
        // ⚠️ A SELECTION INPUT HAS NO ROW HERE, AND STILL HAS A VALUE. Its
        // controls are the Update / Add / Remove / Reselect row the palette
        // builds above these inputs from this definition's own schema - the same
        // SelectionSetPanel a Python command's selection_sets produces. A caption
        // here as well would be the same number in two places, and the band's
        // job is the definition's own inputs plus the solve controls, kept clean.
        //
        // Nothing is placed, so PaletteScroll leaves it hidden; the entry stays
        // in `controls` because Collect reads the value off it.
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

        if (control.selectionUpdate) {
            // ⚠️ TWO LINES, AND ACROSS THE WHOLE WIDTH RATHER THAN THE
            // FIELD COLUMN. A captioned button needs room for its word, and
            // "Reselect" does not fit a fifth of one input field. So the verbs
            // take the domain column as well -- the only thing that column held
            // on a selection row was the count, which moves to the end of the
            // second line where it sits beside the buttons that change it.
            DG::Item* const verbs[] = { control.selectionUpdate.get (), control.selectionAdd.get (),
                                        control.selectionRemove.get (), control.selectionReselect.get (),
                                        control.selectionClear.get () };

            const short verbsLeft = hintLeft;
            const short lineWidth = (short) (right - verbsLeft);

            // Three then two, because Update / Add / Remove are the three that
            // change what is held and Reselect / Clear are the two that do not.
            const short firstSpan = (short) (lineWidth / 3);
            for (short index = 0; index < 3; ++index) {
                const short left = (short) (verbsLeft + index * firstSpan);
                const short edge = index == 2 ? right : (short) (left + firstSpan);
                clip.Place (verbs[index], DG::Rect (left, y, edge, (short) (y + RowHeight)));
            }

            y = (short) (y + RowHeight + RowGap);

            const short secondSpan = (short) (lineWidth / 3);
            clip.Place (verbs[3], DG::Rect (verbsLeft, y, (short) (verbsLeft + secondSpan), (short) (y + RowHeight)));
            clip.Place (verbs[4], DG::Rect ((short) (verbsLeft + secondSpan), y, (short) (verbsLeft + 2 * secondSpan),
                                            (short) (y + RowHeight)));
            clip.Place (control.selectionCount.get (),
                        DG::Rect ((short) (verbsLeft + 2 * secondSpan + 6), y, right, (short) (y + RowHeight)));

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

bool WorkflowPanel::HandleSelectionButton (const DG::Item* item)
{
    if (item == nullptr)
        return false;

    for (WorkflowControl& control : controls) {
        if (control.row.kind != InputKind::Selection)
            continue;

        const char* op = nullptr;
        bool mine = true;
        if (item == control.selectionUpdate.get ())
            op = "update";
        else if (item == control.selectionAdd.get ())
            op = "add";
        else if (item == control.selectionRemove.get ())
            op = "remove";
        else if (item == control.selectionClear.get ())
            op = "clear";
        else if (item == control.selectionReselect.get ())
            op = nullptr; // Reselect: the one verb that writes TO Archicad.
        else
            mine = false;

        if (!mine)
            continue;

        RunSelectionVerb (control.row.id, op);
        // Read back rather than assumed: the command may have selected fewer
        // elements than were asked for, and the count is the only thing on
        // screen that says so.
        if (control.selectionCount)
            control.selectionCount->SetText (SelectionCount (control.row.id));
        return true;
    }

    return false;
}

void WorkflowPanel::RefreshSelections ()
{
    for (WorkflowControl& control : controls) {
        if (!control.selectionCount)
            continue;

        // Compared before assigning: SetText invalidates the item even when the
        // text is unchanged, and this runs on the palette's idle tick.
        const GS::UniString count = SelectionCount (control.row.id);
        if (control.selectionCount->GetText () != count)
            control.selectionCount->SetText (count);
    }
}

std::vector<std::string> WorkflowPanel::SelectionRoles () const
{
    std::vector<std::string> roles;
    for (const WorkflowControl& control : controls) {
        if (control.row.kind == InputKind::Selection)
            roles.push_back (control.row.label.empty () ? control.row.id : control.row.label);
    }
    return roles;
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
