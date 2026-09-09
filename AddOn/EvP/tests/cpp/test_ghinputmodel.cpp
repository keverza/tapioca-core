// Palette/InputModel.cpp — the neutral input model the panel builds controls
// from, and the validation that runs before a solve is requested.
//
// Two kinds of failure live here and neither announces itself.
//
// A PARSING failure drops a control. The panel then shows a definition with
// fewer inputs than it has, which is indistinguishable from a definition whose
// author forgot one — and the solve succeeds, using whatever the definition had
// saved for the input nobody could see.
//
// A NORMALISATION failure is quieter still. "True" and "true" are the same value
// and two different snapshot hashes, so a model that passed both through would
// make the change detector report edits the user did not make, and the
// diagnostics written to catch a race would be the thing producing one.

#include "Palette/InputModel.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace evp;

namespace {

// The shape TapiocaInputSchema.ToJson actually emits, written out rather than
// built: this test's job is to prove the two halves agree, and generating the
// input from the same idea that reads it would prove nothing.
const char* const kSchema = R"({
  "workflowId": "abc123",
  "name": "facade.gh",
  "version": 1,
  "inputs": [
    {"id":"height","label":"Height","type":"number","group":"Massing","order":1,
     "required":true,"min":0.5,"max":12,"default":"3.2"},
    {"id":"floors","label":"Floors","type":"integer","group":"Massing","order":0,
     "required":false,"min":1,"max":40,"default":"4"},
    {"id":"mirrored","label":"Mirrored","type":"boolean","group":"Massing","order":2,
     "required":false,"default":"false"},
    {"id":"note","label":"Note","type":"string","group":"Labels","order":0,
     "required":false,"default":"north wing"},
    {"id":"material","label":"Material","type":"enum","group":"Labels","order":1,
     "required":true,"choices":["Concrete","Timber","Steel"],"default":"Timber"}
  ],
  "errors": []
})";

const InputControl* Find (const InputModel& model, const std::string& id)
{
    for (const InputControl& control : model.controls) {
        if (control.id == id)
            return &control;
    }
    return nullptr;
}

InputControl NumberControl (double minimum, double maximum)
{
    InputControl control;
    control.id = "n";
    control.label = "Height";
    control.kind = InputKind::Number;
    control.hasMinimum = true;
    control.minimum = minimum;
    control.hasMaximum = true;
    control.maximum = maximum;
    return control;
}

} // namespace

TEST (GhInputModel, ParsesEveryDeclaredFieldOfEveryKind)
{
    const InputModel model = ParseInputModel (kSchema);
    ASSERT_TRUE (model.parsed);
    EXPECT_EQ ("abc123", model.workflowId);
    EXPECT_EQ ("facade.gh", model.name);
    EXPECT_TRUE (model.errors.empty ());
    ASSERT_EQ (5u, model.controls.size ());

    const InputControl* height = Find (model, "height");
    ASSERT_NE (nullptr, height);
    EXPECT_EQ ("Height", height->label);
    EXPECT_EQ ("Massing", height->group);
    EXPECT_EQ (InputKind::Number, height->kind);
    EXPECT_TRUE (height->required);
    EXPECT_TRUE (height->hasMinimum);
    EXPECT_DOUBLE_EQ (0.5, height->minimum);
    EXPECT_TRUE (height->hasMaximum);
    EXPECT_DOUBLE_EQ (12.0, height->maximum);
    EXPECT_EQ ("3.2", height->defaultValue);

    const InputControl* material = Find (model, "material");
    ASSERT_NE (nullptr, material);
    EXPECT_EQ (InputKind::Enum, material->kind);
    ASSERT_EQ (3u, material->choices.size ());
    EXPECT_EQ ("Concrete", material->choices[0]);
    EXPECT_EQ ("Steel", material->choices[2]);

    // The package writes "string"; this model calls it text. Both spellings are
    // accepted because only one of them is ours to change.
    const InputControl* note = Find (model, "note");
    ASSERT_NE (nullptr, note);
    EXPECT_EQ (InputKind::Text, note->kind);
}

TEST (GhInputModel, RowsAreOrderedByGroupThenOrder)
{
    // The same rule the worker's discovery applied. Two sorts that disagreed
    // would reorder the user's controls every time the schema was refetched.
    // Groups sort alphabetically, so Labels precedes Massing -- the same rule
    // TapiocaInputSchema.Discover applies on the worker side. Within a group it
    // is the author's declared order.
    const InputModel model = ParseInputModel (kSchema);
    ASSERT_EQ (5u, model.controls.size ());
    EXPECT_EQ ("note", model.controls[0].id);
    EXPECT_EQ ("material", model.controls[1].id);
    EXPECT_EQ ("floors", model.controls[2].id);
    EXPECT_EQ ("height", model.controls[3].id);
    EXPECT_EQ ("mirrored", model.controls[4].id);
}

TEST (GhInputModel, AnAbsentBoundIsNotAZeroBound)
{
    // A minimum of 0 is a real constraint and the commonest one there is, so
    // "absent" and "zero" have to stay different things.
    const InputModel model = ParseInputModel (
        R"({"workflowId":"x","name":"x","inputs":[
             {"id":"free","type":"number","default":"1"},
             {"id":"floored","type":"number","min":0,"default":"1"}],"errors":[]})");
    ASSERT_TRUE (model.parsed);

    const InputControl* free = Find (model, "free");
    ASSERT_NE (nullptr, free);
    EXPECT_FALSE (free->hasMinimum);

    const InputControl* floored = Find (model, "floored");
    ASSERT_NE (nullptr, floored);
    EXPECT_TRUE (floored->hasMinimum);
    EXPECT_DOUBLE_EQ (0.0, floored->minimum);

    EXPECT_TRUE (Coerce (*free, "-5").ok) << "an absent minimum constrains nothing";
    EXPECT_FALSE (Coerce (*floored, "-5").ok);
}

TEST (GhInputModel, AnUnknownTypeIsKeptAsUnsupportedRatherThanDropped)
{
    // The user has to be able to see that the definition asks for something this
    // Tapioca cannot offer. A row that silently vanished would look like a
    // definition with fewer inputs.
    const InputModel model = ParseInputModel (
        R"({"workflowId":"x","name":"x","inputs":[
             {"id":"surface","label":"Surface","type":"brep","default":""}],"errors":[]})");
    ASSERT_TRUE (model.parsed);
    ASSERT_EQ (1u, model.controls.size ());
    EXPECT_EQ (InputKind::Unsupported, model.controls[0].kind);
    EXPECT_EQ ("brep", model.controls[0].declaredType);

    const CoerceResult result = Coerce (model.controls[0], "anything");
    EXPECT_FALSE (result.ok);
    EXPECT_NE (std::string::npos, result.error.find ("brep")) << "the refusal must name the type it cannot offer";
}

TEST (GhInputModel, AnIdlessInputIsDroppedAndSaidSo)
{
    const InputModel model = ParseInputModel (
        R"({"workflowId":"x","name":"x","inputs":[
             {"id":"","label":"Nameless","type":"number","default":"1"},
             {"id":"ok","type":"number","default":"1"}],"errors":[]})");
    ASSERT_TRUE (model.parsed);
    ASSERT_EQ (1u, model.controls.size ());
    EXPECT_EQ ("ok", model.controls[0].id);
    ASSERT_EQ (1u, model.errors.size ());
    EXPECT_NE (std::string::npos, model.errors[0].find ("Nameless"));
}

TEST (GhInputModel, TheSchemasOwnErrorsAreCarriedThrough)
{
    // A duplicate id, an enum with no choices, a minimum above a maximum: the
    // author's problems, and the panel shows them rather than hiding them behind
    // an empty row list.
    const InputModel model = ParseInputModel (
        R"({"workflowId":"x","name":"x","inputs":[{"id":"a","type":"number","default":"1"}],
            "errors":["Two or more inputs share the id 'a'."]})");
    ASSERT_TRUE (model.parsed);
    ASSERT_EQ (1u, model.errors.size ());
    EXPECT_NE (std::string::npos, model.errors[0].find ("share the id"));
    EXPECT_EQ (1u, model.controls.size ()) << "errors do not stop the rows that did parse from being shown";
}

TEST (GhInputModel, AMalformedSchemaParsesToNothingRatherThanToSomeOfIt)
{
    // A half-built model would render as a definition with some of its inputs
    // missing, which is the one failure mode that looks like a working panel.
    const InputModel model = ParseInputModel ("{\"inputs\": [ {\"id\": ");
    EXPECT_FALSE (model.parsed);
    EXPECT_TRUE (model.controls.empty ());
    ASSERT_EQ (1u, model.errors.size ());
    EXPECT_NE (std::string::npos, model.errors[0].find ("could not be read"));
}

TEST (GhInputModel, AnEmptySchemaIsNotAFailure)
{
    // A definition with no Tapioca inputs is a legitimate definition; it just
    // has nothing to configure.
    const InputModel model = ParseInputModel (R"({"workflowId":"x","name":"x","inputs":[],"errors":[]})");
    EXPECT_TRUE (model.parsed);
    EXPECT_TRUE (model.controls.empty ());
    EXPECT_TRUE (model.errors.empty ());
}

TEST (GhInputModel, BooleansNormaliseToOneSpelling)
{
    InputControl control;
    control.id = "b";
    control.label = "Mirrored";
    control.kind = InputKind::Boolean;

    EXPECT_EQ ("true", Coerce (control, "true").value);
    EXPECT_EQ ("true", Coerce (control, "True").value);
    EXPECT_EQ ("true", Coerce (control, "  TRUE ").value);
    EXPECT_EQ ("true", Coerce (control, "1").value);
    EXPECT_EQ ("false", Coerce (control, "False").value);
    EXPECT_EQ ("false", Coerce (control, "0").value);

    const CoerceResult bad = Coerce (control, "yes");
    EXPECT_FALSE (bad.ok);
    EXPECT_NE (std::string::npos, bad.error.find ("Mirrored")) << "the refusal names the label, not the id";
}

TEST (GhInputModel, NumbersNormaliseAndRoundTrip)
{
    const InputControl control = NumberControl (0.0, 100.0);
    EXPECT_EQ ("1.5", Coerce (control, "1.50").value);
    EXPECT_EQ ("1.5", Coerce (control, " 1.5 ").value);
    EXPECT_EQ ("3", Coerce (control, "3.0").value);

    // The shortest spelling that reads back identically: a number the author
    // typed must come back as the number they typed.
    EXPECT_EQ ("0.1", Coerce (control, "0.1").value);
}

TEST (GhInputModel, AShortSpellingIsNeverAnExponentOne)
{
    // Taking the first precision that reads back identically renders 40 as
    // "4e+01": exactly equal on the way back, and unrecognisable as the number
    // the author typed. It would reach the domain hint the user reads, the value
    // sent over the pipe, and the input-snapshot hash.
    EXPECT_EQ ("40", FormatInputNumber (40.0));
    EXPECT_EQ ("100", FormatInputNumber (100.0));
    EXPECT_EQ ("0.001", FormatInputNumber (0.001));
    EXPECT_EQ ("-5", FormatInputNumber (-5.0));
    EXPECT_EQ ("1.5", FormatInputNumber (1.5));

    // A number that genuinely needs one still gets one: writing 1e300 out in
    // full would be three hundred digits nobody can read.
    EXPECT_NE (std::string::npos, FormatInputNumber (1e300).find ('e'));

    const InputControl control = NumberControl (0.0, 1000.0);
    EXPECT_EQ ("40", Coerce (control, "40").value);
    EXPECT_EQ ("40", Coerce (control, "40.0").value);
}

TEST (GhInputModel, ANumberWithATailIsRefused)
{
    // strtod happily reads "3abc" as 3 and stops. A control that accepted that
    // would send 3 for a value the user can plainly see is wrong.
    const InputControl control = NumberControl (0.0, 100.0);
    EXPECT_FALSE (Coerce (control, "3abc").ok);
    EXPECT_FALSE (Coerce (control, "").ok);
    EXPECT_FALSE (Coerce (control, "--2").ok);
    EXPECT_FALSE (Coerce (control, "nan").ok) << "NaN parses but cannot be solved with";
    EXPECT_FALSE (Coerce (control, "inf").ok);
}

TEST (GhInputModel, BoundsAreEnforcedBeforeARoundTripToRhino)
{
    const InputControl control = NumberControl (0.5, 12.0);
    EXPECT_TRUE (Coerce (control, "0.5").ok) << "the bound itself is inside it";
    EXPECT_TRUE (Coerce (control, "12").ok);
    EXPECT_FALSE (Coerce (control, "0.49").ok);
    EXPECT_FALSE (Coerce (control, "12.01").ok);

    const CoerceResult low = Coerce (control, "0");
    EXPECT_NE (std::string::npos, low.error.find ("0.5")) << "the refusal states the bound it broke";
}

TEST (GhInputModel, AnIntegerIsRefusedRatherThanRounded)
{
    // Rounding would let a user type 2.6, see it accepted, and get a solution
    // computed from 3 with nothing on screen saying so.
    InputControl control;
    control.id = "i";
    control.label = "Floors";
    control.kind = InputKind::Integer;
    control.hasMinimum = true;
    control.minimum = 1;

    EXPECT_EQ ("4", Coerce (control, "4").value);
    EXPECT_EQ ("4", Coerce (control, "4.0").value);
    EXPECT_FALSE (Coerce (control, "2.6").ok);
    EXPECT_FALSE (Coerce (control, "0").ok);
}

TEST (GhInputModel, AnEnumPrefersTheExactChoice)
{
    // A definition offering both "Wall" and "wall" must resolve to the one the
    // user picked, not to whichever came first.
    InputControl control;
    control.id = "e";
    control.label = "Material";
    control.kind = InputKind::Enum;
    control.choices = { "Wall", "wall", "Slab" };

    EXPECT_EQ ("wall", Coerce (control, "wall").value);
    EXPECT_EQ ("Wall", Coerce (control, "Wall").value);
    // No exact match: the case-insensitive pass takes the first.
    EXPECT_EQ ("Slab", Coerce (control, "SLAB").value);
    EXPECT_FALSE (Coerce (control, "Timber").ok);
}

TEST (GhInputModel, AnEnumWithNoChoicesRefusesRatherThanSendingNothing)
{
    InputControl control;
    control.id = "e";
    control.label = "Material";
    control.kind = InputKind::Enum;
    EXPECT_FALSE (Coerce (control, "anything").ok);
}

TEST (GhInputModel, TextKeepsItsSpacesAndHonoursRequired)
{
    // Leading and trailing spaces are CONTENT in a text input -- a label, a
    // prefix, a separator.
    InputControl control;
    control.id = "t";
    control.label = "Note";
    control.kind = InputKind::Text;

    const CoerceResult padded = Coerce (control, "  north wing  ");
    ASSERT_TRUE (padded.ok);
    EXPECT_EQ ("  north wing  ", padded.value);
    EXPECT_TRUE (Coerce (control, "").ok);

    control.required = true;
    EXPECT_FALSE (Coerce (control, "   ").ok);
    EXPECT_TRUE (Coerce (control, " x ").ok);
}

TEST (GhInputModel, CoerceAllReportsEveryBadRowAndStaysParallel)
{
    // A caller has to be able to mark every offending control at once, so the
    // values array stays aligned with the model's rows whatever failed.
    const InputModel model = ParseInputModel (kSchema);
    ASSERT_EQ (5u, model.controls.size ());

    // note, material, floors, height, mirrored -- in the model's own order.
    // floors is over its maximum and mirrored is not a boolean; the rest pass.
    const std::vector<std::string> texts = { "wing", "Timber", "99", "3.20", "maybe" };
    std::vector<std::string> values;
    std::vector<std::string> errors;
    EXPECT_FALSE (CoerceAll (model, texts, values, errors));

    ASSERT_EQ (model.controls.size (), values.size ());
    EXPECT_EQ ("Timber", values[1]);
    EXPECT_EQ ("3.2", values[3]) << "the rows that passed are normalised";
    EXPECT_EQ ("99", values[2]) << "a refused row keeps the text the user typed, so the caller can show it back";
    ASSERT_EQ (2u, errors.size ()) << "both bad rows are reported, not just the first";
}

TEST (GhInputModel, CoerceAllRefusesAMismatchedCount)
{
    const InputModel model = ParseInputModel (kSchema);
    std::vector<std::string> values;
    std::vector<std::string> errors;
    EXPECT_FALSE (CoerceAll (model, { "1" }, values, errors));
    ASSERT_EQ (1u, errors.size ());
}

TEST (GhInputModel, EveryKindIsNamed)
{
    const InputKind kinds[] = { InputKind::Number, InputKind::Integer, InputKind::Boolean,
                                InputKind::Text,   InputKind::Enum,    InputKind::Unsupported };
    for (InputKind kind : kinds)
        EXPECT_STRNE ("", DescribeInputKind (kind));

    EXPECT_EQ (InputKind::Number, InputKindFromName ("number"));
    EXPECT_EQ (InputKind::Text, InputKindFromName ("string"));
    EXPECT_EQ (InputKind::Text, InputKindFromName ("text"));
    EXPECT_EQ (InputKind::Unsupported, InputKindFromName ("curve"));
}
