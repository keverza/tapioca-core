// F3 — show_when evaluation (Palette/ParamVisibility.cpp).
//
// This is the half of the action selector that can be wrong INVISIBLY: a row that
// quietly never appears looks like a command that forgot a setting, and nothing in
// Archicad reports it. So the rules are asserted here, where a wrong answer is a
// failing test rather than an afternoon.

#include "Palette/ParamVisibility.hpp"

#include "gtest/gtest.h"

using evp::EvaluateVisibility;
using evp::VisibilityRule;

namespace {

VisibilityRule Always ()
{
    return VisibilityRule {};
}

VisibilityRule When (const std::string& controller, const std::vector<std::string>& values)
{
    return VisibilityRule { controller, values };
}

}   // namespace

TEST (ParamVisibility, UnconditionalRowsAreAlwaysVisible)
{
    const auto visible = EvaluateVisibility ({ "action", "layer" }, { "Place", "Annotation" },
                                             { Always (), Always () });
    EXPECT_EQ (visible, std::vector<bool> ({ true, true }));
}

TEST (ParamVisibility, RowFollowsItsController)
{
    const std::vector<std::string> names = { "action", "radius" };
    const std::vector<VisibilityRule> rules = { Always (), When ("action", { "Place" }) };

    EXPECT_TRUE  (EvaluateVisibility (names, { "Place",  "1.0" }, rules)[1]);
    EXPECT_FALSE (EvaluateVisibility (names, { "Remove", "1.0" }, rules)[1]);
}

TEST (ParamVisibility, AnyOfSeveralValuesShowsTheRow)
{
    const std::vector<std::string> names = { "action", "layer" };
    const std::vector<VisibilityRule> rules = { Always (), When ("action", { "Place", "Update" }) };

    EXPECT_TRUE  (EvaluateVisibility (names, { "Place",  "" }, rules)[1]);
    EXPECT_TRUE  (EvaluateVisibility (names, { "Update", "" }, rules)[1]);
    EXPECT_FALSE (EvaluateVisibility (names, { "Remove", "" }, rules)[1]);
}

// A checkbox controller. The scanner normalises both sides to "true"/"false", and
// this is the test that says so out loud: any other spelling here matches nothing
// and the row silently disappears.
TEST (ParamVisibility, BoolControllerUsesTrueFalseSpelling)
{
    const std::vector<std::string> names = { "dry_run", "report" };
    const std::vector<VisibilityRule> rules = { Always (), When ("dry_run", { "false" }) };

    EXPECT_TRUE  (EvaluateVisibility (names, { "false", "" }, rules)[1]);
    EXPECT_FALSE (EvaluateVisibility (names, { "true",  "" }, rules)[1]);
}

// The reason this is not a one-liner. `mode` shows only for Place; `radius` shows
// only for mode=Round. Switch the action to Remove and `mode` goes — so `radius`
// must go with it, even though its OWN controller still reads "Round".
TEST (ParamVisibility, HiddenControllerHidesWhatFollowsIt)
{
    const std::vector<std::string> names  = { "action", "mode", "radius" };
    const std::vector<std::string> values = { "Remove", "Round", "1.0" };
    const std::vector<VisibilityRule> rules = {
        Always (), When ("action", { "Place" }), When ("mode", { "Round" })
    };

    const auto visible = EvaluateVisibility (names, values, rules);
    EXPECT_EQ (visible, std::vector<bool> ({ true, false, false }));
}

TEST (ParamVisibility, ChainStaysVisibleWhenEveryLinkHolds)
{
    const std::vector<std::string> names  = { "action", "mode", "radius" };
    const std::vector<std::string> values = { "Place", "Round", "1.0" };
    const std::vector<VisibilityRule> rules = {
        Always (), When ("action", { "Place" }), When ("mode", { "Round" })
    };

    const auto visible = EvaluateVisibility (names, values, rules);
    EXPECT_EQ (visible, std::vector<bool> ({ true, true, true }));
}

// The scanner refuses an unknown controller loudly, so this can only happen if one
// ever slips past it. Showing the row is the recoverable failure: a control that is
// there when it should not be gets reported; one that vanished does not.
TEST (ParamVisibility, UnknownControllerLeavesTheRowVisible)
{
    const auto visible = EvaluateVisibility ({ "layer" }, { "Annotation" },
                                             { When ("nosuchparam", { "Place" }) });
    EXPECT_EQ (visible, std::vector<bool> ({ true }));
}

// Two rows naming each other cannot resolve to anything sensible; what matters is
// that the fixed-point sweep TERMINATES rather than spinning.
TEST (ParamVisibility, MutualControllersTerminate)
{
    const std::vector<std::string> names  = { "a", "b" };
    const std::vector<std::string> values = { "x", "y" };
    const std::vector<VisibilityRule> rules = { When ("b", { "y" }), When ("a", { "z" }) };

    const auto visible = EvaluateVisibility (names, values, rules);
    EXPECT_FALSE (visible[1]);   // "a" is not "z"
    EXPECT_FALSE (visible[0]);   // ...so its controller is gone, and it follows
}

TEST (ParamVisibility, NoRowsIsNotACrash)
{
    EXPECT_TRUE (EvaluateVisibility ({}, {}, {}).empty ());
}
