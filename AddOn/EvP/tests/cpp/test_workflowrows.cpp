// Palette/WorkflowRows.cpp — what the workflow panel draws, and the read-back
// that turns it into an input snapshot.
//
// Everything here fails silently on screen, which is why it is not left to be
// judged by eye in Archicad:
//
//   * a read-back that loses its alignment sends every value one input along.
//     The definition still solves. The answer is still plausible. Nothing is
//     marked.
//   * a heading emitted for a group with no rows leaves a title over nothing,
//     and one emitted per row turns structure into noise -- neither reads as a
//     bug, so neither gets reported.
//   * a default that the definition's own bounds refuse opens the panel in a
//     state where Solve is disabled and no row says why.

#include "Palette/WorkflowRows.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace evp;

namespace {

const char* const kGrouped = R"({
  "workflowId":"x","name":"x","inputs":[
    {"id":"note","label":"Note","type":"string","group":"Labels","order":0,"default":"north"},
    {"id":"material","label":"Material","type":"enum","group":"Labels","order":1,
     "choices":["Concrete","Timber"],"default":"Timber"},
    {"id":"floors","label":"Floors","type":"integer","group":"Massing","order":0,
     "min":1,"max":40,"default":"4"},
    {"id":"height","label":"Height","type":"number","group":"Massing","order":1,
     "min":0.5,"max":12,"default":"3.2"}
  ],"errors":[]})";

// The texts the panel would read back if the user changed nothing: every row's
// initial value, and an empty string for each heading.
std::vector<std::string> AsTyped (const std::vector<WorkflowRow>& rows)
{
    std::vector<std::string> texts;
    for (const WorkflowRow& row : rows)
        texts.push_back (row.isHeading ? std::string () : row.initialValue);
    return texts;
}

size_t IndexOf (const std::vector<WorkflowRow>& rows, const std::string& id)
{
    for (size_t index = 0; index < rows.size (); ++index) {
        if (!rows[index].isHeading && rows[index].id == id)
            return index;
    }
    return rows.size ();
}

} // namespace

TEST (WorkflowRows, AHeadingIsEmittedPerGroupAndNotPerRow)
{
    const std::vector<WorkflowRow> rows = BuildWorkflowRows (ParseInputModel (kGrouped));

    // Labels: heading, note, material. Massing: heading, floors, height.
    ASSERT_EQ (6u, rows.size ());
    EXPECT_TRUE (rows[0].isHeading);
    EXPECT_EQ ("Labels", rows[0].label);
    EXPECT_FALSE (rows[1].isHeading);
    EXPECT_FALSE (rows[2].isHeading);
    EXPECT_TRUE (rows[3].isHeading);
    EXPECT_EQ ("Massing", rows[3].label);
    EXPECT_FALSE (rows[4].isHeading);
    EXPECT_FALSE (rows[5].isHeading);
}

TEST (WorkflowRows, AnUngroupedDefinitionGetsNoHeadingsAtAll)
{
    // A heading over every row of a definition whose author never used groups
    // would be noise per row rather than structure.
    const std::vector<WorkflowRow> rows = BuildWorkflowRows (ParseInputModel (
        R"({"workflowId":"x","name":"x","inputs":[
             {"id":"a","type":"number","default":"1"},
             {"id":"b","type":"number","default":"2"}],"errors":[]})"));
    ASSERT_EQ (2u, rows.size ());
    EXPECT_FALSE (rows[0].isHeading);
    EXPECT_FALSE (rows[1].isHeading);
}

TEST (WorkflowRows, AnEmptyModelDrawsNothing)
{
    EXPECT_TRUE (BuildWorkflowRows (ParseInputModel (R"({"inputs":[],"errors":[]})")).empty ());
}

TEST (WorkflowRows, TheDomainIsWrittenOutForNumericRowsOnly)
{
    const std::vector<WorkflowRow> rows = BuildWorkflowRows (ParseInputModel (kGrouped));

    EXPECT_EQ ("1-40", rows[IndexOf (rows, "floors")].domainHint);
    EXPECT_EQ ("0.5-12", rows[IndexOf (rows, "height")].domainHint);
    // An enum's domain is its popup and a boolean's is a checkbox; spelling
    // either out would restate the control the user is looking at.
    EXPECT_EQ ("", rows[IndexOf (rows, "material")].domainHint);
    EXPECT_EQ ("", rows[IndexOf (rows, "note")].domainHint);
}

TEST (WorkflowRows, AOneSidedDomainReadsAsASentence)
{
    InputControl control;
    control.kind = InputKind::Number;
    control.hasMinimum = true;
    control.minimum = 0.5;
    EXPECT_EQ ("at least 0.5", DescribeDomain (control));

    control.hasMinimum = false;
    control.hasMaximum = true;
    control.maximum = 12;
    EXPECT_EQ ("at most 12", DescribeDomain (control));

    control.hasMaximum = false;
    EXPECT_EQ ("", DescribeDomain (control));
}

TEST (WorkflowRows, ANegativeMinimumSurvivesTheHint)
{
    // The reason the bounds are CARRIED on the row rather than parsed back out
    // of this string: "-5-5" cannot be read back, and a validator that tried
    // would accept anything.
    InputControl control;
    control.kind = InputKind::Number;
    control.hasMinimum = true;
    control.minimum = -5;
    control.hasMaximum = true;
    control.maximum = 5;
    EXPECT_EQ ("-5-5", DescribeDomain (control));

    const std::vector<WorkflowRow> rows = BuildWorkflowRows (
        ParseInputModel (R"({"inputs":[{"id":"n","type":"number","min":-5,"max":5,"default":"0"}],"errors":[]})"));
    ASSERT_EQ (1u, rows.size ());
    EXPECT_TRUE (rows[0].hasMinimum);
    EXPECT_DOUBLE_EQ (-5.0, rows[0].minimum);

    const WorkflowSnapshot low = ReadWorkflowRows (rows, { "-6" });
    EXPECT_FALSE (low.ok) << "the carried bound still refuses, whatever the hint reads like";
    const WorkflowSnapshot inside = ReadWorkflowRows (rows, { "-5" });
    EXPECT_TRUE (inside.ok);
}

TEST (WorkflowRows, TheInitialValueIsNormalisedButADefaultIsNotAssumedValid)
{
    // A definition can be saved with a slider outside bounds its author narrowed
    // afterwards. The panel opens showing what the definition holds, and says
    // the row is refused rather than looking broken.
    const std::vector<WorkflowRow> rows = BuildWorkflowRows (ParseInputModel (
        R"({"inputs":[
             {"id":"tidy","type":"number","min":0,"max":10,"default":"3.50"},
             {"id":"stale","label":"Stale","type":"number","min":0,"max":10,"default":"99"}],
           "errors":[]})"));
    ASSERT_EQ (2u, rows.size ());

    EXPECT_EQ ("3.5", rows[0].initialValue) << "an acceptable default is normalised";
    EXPECT_FALSE (rows[0].initialValueRefused);

    EXPECT_EQ ("99", rows[1].initialValue) << "a refused default is shown AS THE DEFINITION HOLDS IT";
    EXPECT_TRUE (rows[1].initialValueRefused);
}

TEST (WorkflowRows, AnUnsupportedRowIsDrawnDisabledAndNamesItsType)
{
    const std::vector<WorkflowRow> rows = BuildWorkflowRows (
        ParseInputModel (R"({"inputs":[{"id":"srf","label":"Surface","type":"brep","default":""}],"errors":[]})"));
    ASSERT_EQ (1u, rows.size ());
    EXPECT_FALSE (rows[0].enabled);
    EXPECT_NE (std::string::npos, rows[0].label.find ("brep"));
    EXPECT_FALSE (rows[0].initialValueRefused) << "unreachable is not the same as wrong";
}

TEST (WorkflowRows, AnUnsupportedRowIsNotSentAndDoesNotBlockTheSolve)
{
    // Sending its saved text would be a value the definition never asked this
    // panel for; refusing the whole snapshot would make the definition
    // unrunnable because of one input nobody can set.
    const std::vector<WorkflowRow> rows = BuildWorkflowRows (ParseInputModel (
        R"({"inputs":[
             {"id":"srf","type":"brep","default":""},
             {"id":"h","type":"number","default":"1"}],"errors":[]})"));
    ASSERT_EQ (2u, rows.size ());

    const WorkflowSnapshot snapshot = ReadWorkflowRows (rows, { "whatever", "2" });
    EXPECT_TRUE (snapshot.ok);
    ASSERT_EQ (1u, snapshot.ids.size ());
    EXPECT_EQ ("h", snapshot.ids[0]);
    EXPECT_EQ ("2", snapshot.values[0]);
}

TEST (WorkflowRows, TheReadBackPairsEveryIdWithItsOwnValue)
{
    // The alignment claim, and the one whose failure is invisible: every value
    // one input along still solves and still looks plausible.
    const std::vector<WorkflowRow> rows = BuildWorkflowRows (ParseInputModel (kGrouped));
    std::vector<std::string> texts = AsTyped (rows);
    texts[IndexOf (rows, "note")] = "south";
    texts[IndexOf (rows, "material")] = "Concrete";
    texts[IndexOf (rows, "floors")] = "9";
    texts[IndexOf (rows, "height")] = "4.25";

    const WorkflowSnapshot snapshot = ReadWorkflowRows (rows, texts);
    ASSERT_TRUE (snapshot.ok);
    ASSERT_EQ (4u, snapshot.ids.size ());
    ASSERT_EQ (snapshot.ids.size (), snapshot.values.size ());

    for (size_t index = 0; index < snapshot.ids.size (); ++index) {
        if (snapshot.ids[index] == "note")
            EXPECT_EQ ("south", snapshot.values[index]);
        else if (snapshot.ids[index] == "material")
            EXPECT_EQ ("Concrete", snapshot.values[index]);
        else if (snapshot.ids[index] == "floors")
            EXPECT_EQ ("9", snapshot.values[index]);
        else if (snapshot.ids[index] == "height")
            EXPECT_EQ ("4.25", snapshot.values[index]);
        else
            ADD_FAILURE () << "unexpected id " << snapshot.ids[index];
    }
}

TEST (WorkflowRows, HeadingsDoNotContributeAValue)
{
    const std::vector<WorkflowRow> rows = BuildWorkflowRows (ParseInputModel (kGrouped));
    const WorkflowSnapshot snapshot = ReadWorkflowRows (rows, AsTyped (rows));
    ASSERT_TRUE (snapshot.ok);
    EXPECT_EQ (4u, snapshot.ids.size ()) << "six rows, two of them headings";
}

TEST (WorkflowRows, ARefusedRowStillContributesItsIdAndItsRawText)
{
    // Dropping it would shorten the arrays and shift every id after it against
    // its value -- the alignment bug the whole struct exists to prevent. `ok` is
    // what gates the solve, never the array lengths.
    const std::vector<WorkflowRow> rows = BuildWorkflowRows (ParseInputModel (kGrouped));
    std::vector<std::string> texts = AsTyped (rows);
    const size_t floors = IndexOf (rows, "floors");
    texts[floors] = "99";

    const WorkflowSnapshot snapshot = ReadWorkflowRows (rows, texts);
    EXPECT_FALSE (snapshot.ok);
    EXPECT_EQ (4u, snapshot.ids.size ());
    ASSERT_EQ (1u, snapshot.errors.size ());
    EXPECT_NE (std::string::npos, snapshot.errors[0].find ("Floors")) << "the refusal names the label";

    ASSERT_EQ (rows.size (), snapshot.refused.size ());
    EXPECT_TRUE (snapshot.refused[floors]);
    EXPECT_FALSE (snapshot.refused[0]) << "a heading is never refused";

    for (size_t index = 0; index < snapshot.ids.size (); ++index) {
        if (snapshot.ids[index] == "floors")
            EXPECT_EQ ("99", snapshot.values[index]) << "the raw text comes back so it can be shown again";
    }
}

TEST (WorkflowRows, EveryBadRowIsReportedNotJustTheFirst)
{
    const std::vector<WorkflowRow> rows = BuildWorkflowRows (ParseInputModel (kGrouped));
    std::vector<std::string> texts = AsTyped (rows);
    texts[IndexOf (rows, "floors")] = "99";
    texts[IndexOf (rows, "height")] = "nonsense";
    texts[IndexOf (rows, "material")] = "Steel";

    const WorkflowSnapshot snapshot = ReadWorkflowRows (rows, texts);
    EXPECT_FALSE (snapshot.ok);
    EXPECT_EQ (3u, snapshot.errors.size ());
}

TEST (WorkflowRows, AMismatchedReadBackIsRefusedRatherThanRealigned)
{
    // Guessing which value belongs to which row is how the alignment bug gets
    // in. There is no correct guess, so there is no guess.
    const std::vector<WorkflowRow> rows = BuildWorkflowRows (ParseInputModel (kGrouped));
    const WorkflowSnapshot snapshot = ReadWorkflowRows (rows, { "one", "two" });
    EXPECT_FALSE (snapshot.ok);
    EXPECT_TRUE (snapshot.ids.empty ());
    ASSERT_EQ (1u, snapshot.errors.size ());
}

TEST (WorkflowRows, ARowWithNoLabelFallsBackToItsId)
{
    const std::vector<WorkflowRow> rows =
        BuildWorkflowRows (ParseInputModel (R"({"inputs":[{"id":"h","type":"number","default":"1"}],"errors":[]})"));
    ASSERT_EQ (1u, rows.size ());
    EXPECT_EQ ("h", rows[0].label) << "a control with no caption is worse than one captioned by its id";
}

// ---------------------------------------------------------------------------
// The slider. A bounded number is a range, and the mapping between a bar
// position and a value is exactly the kind of arithmetic that is wrong by one
// step forever if nobody states it.

TEST (WorkflowRows, OnlyABoundedNumberGetsASlider)
{
    evp::WorkflowRow row;
    row.kind = evp::InputKind::Number;
    row.enabled = true;

    EXPECT_FALSE (evp::UsesSlider (row)) << "no bounds at all";

    row.hasMinimum = true;
    row.minimum = 0.0;
    EXPECT_FALSE (evp::UsesSlider (row)) << "one end is not a range";

    row.hasMaximum = true;
    row.maximum = 0.0;
    EXPECT_FALSE (evp::UsesSlider (row)) << "a domain with no width is a constant";

    row.maximum = 10.0;
    EXPECT_TRUE (evp::UsesSlider (row));

    row.kind = evp::InputKind::Text;
    EXPECT_FALSE (evp::UsesSlider (row)) << "text has no positions between two ends";

    row.kind = evp::InputKind::Integer;
    EXPECT_TRUE (evp::UsesSlider (row));

    row.enabled = false;
    EXPECT_FALSE (evp::UsesSlider (row)) << "an unsupported row is locked, slider included";
}

TEST (WorkflowRows, ASliderPositionRoundTripsThroughItsValue)
{
    evp::WorkflowRow row;
    row.kind = evp::InputKind::Number;
    row.enabled = true;
    row.hasMinimum = true;
    row.minimum = 2.0;
    row.hasMaximum = true;
    row.maximum = 12.0;

    EXPECT_EQ (evp::SliderValueAt (row, 0), "2");
    EXPECT_EQ (evp::SliderValueAt (row, evp::WorkflowSliderSteps), "12");
    EXPECT_EQ (evp::SliderValueAt (row, evp::WorkflowSliderSteps / 2), "7");

    EXPECT_EQ (evp::SliderPositionFor (row, "2"), 0);
    EXPECT_EQ (evp::SliderPositionFor (row, "12"), evp::WorkflowSliderSteps);
    EXPECT_EQ (evp::SliderPositionFor (row, "7"), evp::WorkflowSliderSteps / 2);
}

TEST (WorkflowRows, AValueOutsideTheDomainParksTheSliderAtTheEnd)
{
    // A definition saved with a value outside bounds its author narrowed
    // afterwards is the ordinary cause. The FIELD still shows the real number
    // and still refuses it; the slider has to be somewhere.
    evp::WorkflowRow row;
    row.kind = evp::InputKind::Number;
    row.enabled = true;
    row.hasMinimum = true;
    row.minimum = 0.0;
    row.hasMaximum = true;
    row.maximum = 1.0;

    EXPECT_EQ (evp::SliderPositionFor (row, "-5"), 0);
    EXPECT_EQ (evp::SliderPositionFor (row, "17"), evp::WorkflowSliderSteps);
    EXPECT_EQ (evp::SliderPositionFor (row, "not a number"), 0);
}

TEST (WorkflowRows, AnIntegerSliderLandsOnWholeNumbersAndCanReachTheTop)
{
    // Truncation instead of rounding would make the top of the domain
    // unreachable and every position land one below what it looks like.
    evp::WorkflowRow row;
    row.kind = evp::InputKind::Integer;
    row.enabled = true;
    row.hasMinimum = true;
    row.minimum = 1.0;
    row.hasMaximum = true;
    row.maximum = 4.0;

    EXPECT_EQ (evp::SliderValueAt (row, evp::WorkflowSliderSteps), "4");
    EXPECT_EQ (evp::SliderValueAt (row, 0), "1");
    // Two thirds along: 1 + 3*0.667 = 3.0
    const std::string middle = evp::SliderValueAt (row, (evp::WorkflowSliderSteps * 2) / 3);
    EXPECT_EQ (middle.find ('.'), std::string::npos) << "an integer row must not offer a fraction";
    EXPECT_EQ (middle, "3");
}

TEST (WorkflowRows, ANonSliderRowAnswersNothingRatherThanGuessing)
{
    evp::WorkflowRow row;
    row.kind = evp::InputKind::Text;
    row.enabled = true;
    EXPECT_EQ (evp::SliderValueAt (row, 500), "");
    EXPECT_EQ (evp::SliderPositionFor (row, "anything"), 0);
}
