// F13.A — generated parameter fields should remain compact while the label
// column absorbs a palette resize. This tests the arithmetic outside DG so a
// future visual change cannot silently make fields consume the whole panel.

#include "Palette/ParamLayout.hpp"

#include "gtest/gtest.h"

TEST (ParamLayout, DefaultContentUsesOneThirdForTheInputColumn)
{
    EXPECT_EQ (evp::InputColumnWidth (412), 137);
}

TEST (ParamLayout, WiderPaletteSendsOnlyTenPercentOfExtraWidthToTheInput)
{
    // +200 px in the content column gives the field +20 px, not +67 px.
    EXPECT_EQ (evp::InputColumnWidth (612), 157);
}

TEST (ParamLayout, NarrowPaletteShrinksToNaturalMinimumThenPreservesIt)
{
    EXPECT_EQ (evp::InputColumnWidth (300), 100);
    EXPECT_EQ (evp::InputColumnWidth (0),   100);
}

TEST (ParamLayout, FormulaIsContinuousAtTheDefaultWidth)
{
    EXPECT_EQ (evp::InputColumnWidth (411), 137);
    EXPECT_EQ (evp::InputColumnWidth (412), 137);
    EXPECT_EQ (evp::InputColumnWidth (422), 138);
}
