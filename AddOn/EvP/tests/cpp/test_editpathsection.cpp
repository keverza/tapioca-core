#include "Edit/EditPath.hpp"
#include <gtest/gtest.h>

namespace ed = geomsrv::edit;

namespace {
std::vector<ed::Section> One ()
{
    ed::Section s;
    s.id = 1;
    s.widthLeft = 1.0;
    s.widthRight = 0.5;
    return { s };
}
} // namespace

TEST (EditPathSection, CrossfallDropsEachEdgeTheRightWay)
{
    ed::Section s = One ()[0];
    double l = 0, r = 0;
    ed::EdgeDrops (s, l, r); // crown, 2 %
    EXPECT_NEAR (l, -0.02, 1e-12);
    EXPECT_NEAR (r, -0.01, 1e-12);
    s.fall = ed::Fall::Left;
    ed::EdgeDrops (s, l, r);
    EXPECT_NEAR (l, -0.02, 1e-12);
    EXPECT_NEAR (r, 0.01, 1e-12);
    s.fall = ed::Fall::Right;
    ed::EdgeDrops (s, l, r);
    EXPECT_NEAR (l, 0.02, 1e-12);
    EXPECT_NEAR (r, -0.01, 1e-12);
}

TEST (EditPathSection, RangesApplyFromTheirStationOn)
{
    auto s = One ();
    std::string error;
    ASSERT_TRUE (ed::SplitSection (s, 100.0, 40.0, 2, error)) << error;
    ASSERT_TRUE (ed::SetWidth (s, 2, 2.0, -1.0, error));
    ASSERT_TRUE (ed::ValidateSections (s, 100.0, error)) << error;
    EXPECT_DOUBLE_EQ (ed::SectionAt (s, 39.9)->widthLeft, 1.0);
    EXPECT_DOUBLE_EQ (ed::SectionAt (s, 40.0)->widthLeft, 2.0);
    EXPECT_DOUBLE_EQ (ed::SectionAt (s, 40.0)->widthRight, 0.5); // copied, then kept
    EXPECT_FALSE (ed::SplitSection (s, 100.0, 40.0, 3, error));
    EXPECT_FALSE (ed::ValidateSections (s, 30.0, error)); // a range past the end
}

TEST (EditPathSection, EditsRefuseImpossibleSurfaces)
{
    auto s = One ();
    std::string error;
    EXPECT_FALSE (ed::SetWidth (s, 1, 0.0, 0.0, error));
    EXPECT_FALSE (ed::SetCrossfall (s, 1, 120.0, ed::Fall::Left, error));
    ASSERT_TRUE (ed::SetCrossfall (s, 1, 2.5, ed::Fall::Right, error));
    EXPECT_EQ (s[0].fall, ed::Fall::Right);
    EXPECT_FALSE (ed::DeleteSection (s, 1, error)); // the first range stays
}

TEST (EditPathSection, DraggedEdgeSetsThatSide)
{
    auto s = One ();
    std::string error;
    ASSERT_TRUE (ed::SetWidthFromOffset (s, 1, 1.6, error));
    EXPECT_DOUBLE_EQ (s[0].widthLeft, 1.6);
    ASSERT_TRUE (ed::SetWidthFromOffset (s, 1, -0.9, error));
    EXPECT_DOUBLE_EQ (s[0].widthRight, 0.9);
    EXPECT_DOUBLE_EQ (s[0].widthLeft, 1.6);
    EXPECT_FALSE (ed::SetWidthFromOffset (s, 1, 0.0, error));
}

TEST (EditPathSection, GlobalWidthMoveAndRestation)
{
    auto s = One ();
    std::string error;
    ASSERT_TRUE (ed::SplitSection (s, 100.0, 20.0, 2, error));
    ASSERT_TRUE (ed::SplitSection (s, 100.0, 60.0, 3, error));
    ASSERT_TRUE (ed::SetWidthEverywhere (s, 1.2, 1.2, error));
    for (const auto& x : s)
        EXPECT_DOUBLE_EQ (x.widthLeft + x.widthRight, 2.4);
    EXPECT_FALSE (ed::MoveSection (s, 2, 70.0, error));
    ASSERT_TRUE (ed::MoveSection (s, 2, 30.0, error));
    ed::RestationSections (s, 100.0, 50.0);
    EXPECT_DOUBLE_EQ (s[1].station, 15.0);
    EXPECT_DOUBLE_EQ (s[2].station, 30.0);
    ASSERT_TRUE (ed::DeleteSection (s, 2, error));
    EXPECT_EQ (s.size (), 2u);
}

TEST (EditPathSection, ATaperRunsItsWidthsToTheNextRangeAndTheLastToThePathEnd)
{
    auto s = One ();
    std::string error;
    ASSERT_TRUE (ed::SplitSection (s, 100.0, 40.0, 2, error)) << error;
    ASSERT_TRUE (ed::SetTaper (s, 1, true, 3.0, -1.0, error)) << error;
    ASSERT_TRUE (ed::ValidateSections (s, 100.0, error)) << error;
    EXPECT_NEAR (ed::SectionIn (s, 100.0, 0.0).widthLeft, 1.0, 1e-12);
    EXPECT_NEAR (ed::SectionIn (s, 100.0, 20.0).widthLeft, 2.0, 1e-12);
    EXPECT_NEAR (ed::SectionIn (s, 100.0, 20.0).widthRight, 0.5, 1e-12); // that side keeps its end
    EXPECT_EQ (ed::SectionIn (s, 100.0, 20.0).id, 1u);
    EXPECT_NEAR (ed::SectionIn (s, 100.0, 40.0).widthLeft, 1.0, 1e-12); // the next range steps back
    ASSERT_TRUE (ed::SetTaper (s, 2, true, 0.0, 2.5, error)) << error;
    EXPECT_NEAR (ed::SectionIn (s, 100.0, 70.0).widthLeft, 0.5, 1e-12);
    EXPECT_NEAR (ed::SectionIn (s, 100.0, 100.0).widthRight, 2.5, 1e-12);
    EXPECT_NEAR (ed::SectionIn (s, 100.0, 130.0).widthRight, 2.5, 1e-12); // past the end: clamped
    EXPECT_TRUE (ed::SectionIn ({}, 100.0, 10.0).id == ed::kNoId);
}

TEST (EditPathSection, SplittingATaperKeepsItsWidthsEverywhere)
{
    auto s = One ();
    std::string error;
    ASSERT_TRUE (ed::SetTaper (s, 1, true, 3.0, 1.5, error)) << error;
    const auto before = s;
    ASSERT_TRUE (ed::SplitSection (s, 100.0, 25.0, 2, error)) << error;
    ASSERT_EQ (s.size (), 2u);
    EXPECT_TRUE (s[0].taper && s[1].taper);
    for (double at : { 0.0, 10.0, 25.0, 60.0, 100.0 }) {
        EXPECT_NEAR (ed::SectionIn (s, 100.0, at).widthLeft, ed::SectionIn (before, 100.0, at).widthLeft, 1e-12) << at;
        EXPECT_NEAR (ed::SectionIn (s, 100.0, at).widthRight, ed::SectionIn (before, 100.0, at).widthRight, 1e-12)
            << at;
    }
}

TEST (EditPathSection, ATaperNeedsAnEndWidthAndParallelAgainForgetsIt)
{
    auto s = One ();
    std::string error;
    EXPECT_FALSE (ed::SetTaper (s, 1, true, 0.0, 0.0, error));
    EXPECT_EQ (error, "a path needs a width");
    EXPECT_FALSE (s[0].taper); // refused atomically
    EXPECT_FALSE (ed::SetTaper (s, 9, true, 1.0, 1.0, error));
    ASSERT_TRUE (ed::SetTaper (s, 1, true, 2.0, 2.0, error)) << error;
    ASSERT_TRUE (ed::SetWidth (s, 1, 1.2, -1.0, error)) << error;
    EXPECT_NEAR (s[0].endLeft, 2.0, 1e-12); // a taper's end stays where it was set
    ASSERT_TRUE (ed::SetTaper (s, 1, false, -1.0, -1.0, error)) << error;
    EXPECT_NEAR (s[0].endLeft, 1.2, 1e-12);
    EXPECT_NEAR (s[0].endRight, 0.5, 1e-12);
    ASSERT_TRUE (ed::SetWidth (s, 1, 0.8, 0.9, error)) << error;
    EXPECT_NEAR (s[0].endLeft, 0.8, 1e-12); // a parallel range's end follows its start
    EXPECT_NEAR (ed::SectionIn (s, 100.0, 50.0).widthRight, 0.9, 1e-12);
}
