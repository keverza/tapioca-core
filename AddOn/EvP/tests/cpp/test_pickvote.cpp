// ArchViz/PickVote — which element a click selects.
//
// Every case here is a symptom somebody actually reported while clicking on a
// building in Archicad. The point of the file is that the next one costs a test
// rather than a build, a sync and a restart.

#include <gtest/gtest.h>

#include "ArchViz/PickVote.hpp"

#include <array>
#include <vector>

using geomsrv::archviz::PlanPickReadback;
using geomsrv::archviz::ResolvePickId;
using geomsrv::archviz::ResolvePickIdAt;

namespace {

constexpr uint32_t kSize = 8;

// A block filled with `fill`, so a test only has to say what differs.
std::array<uint32_t, kSize * kSize> Block (uint32_t fill)
{
    std::array<uint32_t, kSize * kSize> ids {};
    ids.fill (fill);
    return ids;
}

void Set (std::array<uint32_t, kSize * kSize>& ids, uint32_t x, uint32_t y, uint32_t id)
{
    ids[y * kSize + x] = id;
}

// The four texels straddling the exact centre of an even-sized block.
void SetCentre (std::array<uint32_t, kSize * kSize>& ids, uint32_t id)
{
    const uint32_t mid = kSize / 2;
    Set (ids, mid - 1, mid - 1, id);
    Set (ids, mid, mid - 1, id);
    Set (ids, mid - 1, mid, id);
    Set (ids, mid, mid, id);
}

}   // namespace

TEST (PickVote, EmptyBlockIsBackground)
{
    const auto ids = Block (0);
    EXPECT_EQ (ResolvePickId (ids.data (), kSize), 0u);
}

TEST (PickVote, ASingleFilledElementWins)
{
    const auto ids = Block (77);
    EXPECT_EQ (ResolvePickId (ids.data (), kSize), 77u);
}

TEST (PickVote, TheCentreBeatsAMajorityAroundIt)
{
    // ⚠️ THE REPORTED "SELECTS THE OBJECT BEHIND". A door handle in front of a
    // wall occupies four texels; the wall occupies the other sixty. A majority
    // vote hands the user the wall every time, and the user was pointing exactly
    // at the handle.
    auto ids = Block (1);      // the wall
    SetCentre (ids, 2);        // the handle, dead centre
    EXPECT_EQ (ResolvePickId (ids.data (), kSize), 2u);
}

TEST (PickVote, ANearMissFallsBackToTheSurroundings)
{
    // The centre is background -- a sliver between two surfaces, or a narrow
    // miss on a thin railing. Without the fallback a click there selects
    // nothing, which is what makes a railing feel unclickable.
    auto ids = Block (0);
    Set (ids, 2, 3, 9);
    Set (ids, 2, 4, 9);
    EXPECT_EQ (ResolvePickId (ids.data (), kSize), 9u);
}

TEST (PickVote, TheFallbackPrefersTheNEARERElementNotTheBIGGERone)
{
    // ⚠️ THIS IS THE ONE A FLAT COUNT GETS WRONG, and it is why the fallback is
    // distance-weighted. A big surface parked in the corner covers more texels
    // than a small one two texels off centre -- but the user's cursor is next to
    // the small one, and a rescue that hands them the far thing is not a rescue.
    auto ids = Block (0);

    // The small, NEAR element: two texels just off centre.
    Set (ids, 3, 2, 5);
    Set (ids, 4, 2, 5);

    // The large, FAR element: a 3x3 patch jammed into the top-left corner --
    // more than twice the texel count.
    for (uint32_t y = 0; y < 3; ++y)
        for (uint32_t x = 0; x < 3; ++x)
            Set (ids, x, y, 6);

    EXPECT_EQ (ResolvePickId (ids.data (), kSize), 5u);
}

TEST (PickVote, TheCentreBlockIsDecidedByMajorityNotByScanOrder)
{
    // ⚠️ TAKING THE FIRST NON-ZERO IN SCAN ORDER BIASED EVERY TIE UP AND TO THE
    // LEFT. On a boundary between two surfaces that is a repeatable wrong answer
    // rather than a coin toss, so it reads as the pick being crooked. Three of
    // the four centre texels are element 4; it must win over the one at the
    // top-left that scan order would have picked.
    auto ids = Block (0);
    const uint32_t mid = kSize / 2;
    Set (ids, mid - 1, mid - 1, 3);   // first in scan order
    Set (ids, mid, mid - 1, 4);
    Set (ids, mid - 1, mid, 4);
    Set (ids, mid, mid, 4);
    EXPECT_EQ (ResolvePickId (ids.data (), kSize), 4u);
}

TEST (PickVote, BackgroundNeverWinsEvenWhenItDominates)
{
    // Id 0 is "the sky". A single element on an otherwise empty block must be
    // selectable -- background is not a candidate, however much of the block it
    // covers.
    auto ids = Block (0);
    Set (ids, 5, 5, 42);
    EXPECT_EQ (ResolvePickId (ids.data (), kSize), 42u);
}

TEST (PickVote, SurvivesDegenerateSizes)
{
    // These run on the render thread, where a wrong answer is a misselection and
    // an assert is a dead Archicad.
    const uint32_t one[1] = {7};
    EXPECT_EQ (ResolvePickId (one, 1), 7u);
    EXPECT_EQ (ResolvePickId (nullptr, kSize), 0u);
    EXPECT_EQ (ResolvePickId (one, 0), 0u);
}

TEST (PickVote, AnOddSizeUsesItsTrueCentreTexel)
{
    // The id G-buffer reads back an ODD block so the cursor lands on a texel
    // CENTRE rather than between four. That single texel must decide.
    constexpr uint32_t kOdd = 5;
    std::array<uint32_t, kOdd * kOdd> ids {};
    ids.fill (1);
    ids[2 * kOdd + 2] = 8;   // the true centre
    EXPECT_EQ (ResolvePickId (ids.data (), kOdd), 8u);
}

// ---------------------------------------------------------------------------
// The off-centre form. ⚠️ EVERY CASE HERE IS A CLICK NEAR THE FRAME'S EDGE
// (PLAT-RE136): the readback box is clamped to the viewport, so the cursor stops
// being at the block's middle exactly when it is closest to a border. Voting
// around the middle there hands back the element a few pixels INBOARD of where
// the user clicked -- which is invisible in the centre of the frame and
// consistently wrong at its edges.

TEST (PickVote, TheCursorDecidesEvenWhenItIsNotAtTheBlockMiddle)
{
    // A click two pixels from the left edge: the box was clamped, so the cursor
    // sits at column 2 of a 9-wide block. The element under the CURSOR must win
    // over the one sitting at the block's geometric middle.
    constexpr uint32_t kW = 9;
    constexpr uint32_t kH = 9;
    std::array<uint32_t, kW * kH> ids {};
    ids.fill (0);
    ids[4 * kW + 4] = 1;   // the block's middle -- NOT where the user pointed
    ids[4 * kW + 2] = 2;   // under the cursor
    EXPECT_EQ (ResolvePickIdAt (ids.data (), kW, kH, 2.0f, 4.0f), 2u);
}

TEST (PickVote, TheOffCentreFallbackMeasuresFromTheCursorToo)
{
    // Nothing under the cursor, and the two candidates are on opposite sides of
    // it. The nearer one wins -- measured from the CURSOR, not from the middle,
    // which would reverse this answer.
    constexpr uint32_t kW = 9;
    constexpr uint32_t kH = 9;
    std::array<uint32_t, kW * kH> ids {};
    ids.fill (0);
    ids[4 * kW + 1] = 7;   // one texel from the cursor at column 2
    ids[4 * kW + 5] = 8;   // one texel from the block's middle at column 4
    EXPECT_EQ (ResolvePickIdAt (ids.data (), kW, kH, 2.0f, 4.0f), 7u);
}

TEST (PickVote, ANonSquareBlockIsResolvedFromItsOwnWidth)
{
    // A viewport shorter than the readback box (a collapsed splitter, a one-row
    // overlay) yields a block that is wider than it is tall. Walking it as if it
    // were square reads past the end of the row and votes on the next one.
    constexpr uint32_t kW = 9;
    constexpr uint32_t kH = 3;
    std::array<uint32_t, kW * kH> ids {};
    ids.fill (0);
    ids[1 * kW + 6] = 11;   // under the cursor
    ids[2 * kW + 0] = 12;   // far corner
    EXPECT_EQ (ResolvePickIdAt (ids.data (), kW, kH, 6.0f, 1.0f), 11u);
}

TEST (PickVote, TheOffCentreFormSurvivesDegenerateInput)
{
    const uint32_t one[1] = {7};
    EXPECT_EQ (ResolvePickIdAt (one, 1, 1, 0.0f, 0.0f), 7u);
    EXPECT_EQ (ResolvePickIdAt (nullptr, 9, 9, 4.0f, 4.0f), 0u);
    EXPECT_EQ (ResolvePickIdAt (one, 0, 1, 0.0f, 0.0f), 0u);
    // A centre outside the block: clamped by nothing, answered by the nearest
    // texel to it, which is the honest reading of "the cursor is over there".
    EXPECT_EQ (ResolvePickIdAt (one, 1, 1, 40.0f, 40.0f), 7u);
}

// ---- the readback box, which decides a click as much as the vote does -------

TEST (PickReadback, TheCursorIsTheBoXesMiddleAwayFromAnyEdge)
{
    // ⚠️ THE PAIRING IS THE POINT: whatever box is chosen, `centre` must address
    // the cursor's own texel INSIDE it. Away from an edge that is the middle.
    const auto plan = PlanPickReadback (500, 400, 1920, 1080, 8);
    ASSERT_TRUE (plan.valid);
    EXPECT_EQ (plan.width, 8u);
    EXPECT_EQ (plan.height, 8u);
    EXPECT_EQ (plan.minX, 496u);
    EXPECT_EQ (plan.minY, 396u);
    EXPECT_FLOAT_EQ (plan.centreX, 4.0f);
    EXPECT_FLOAT_EQ (plan.centreY, 4.0f);
    EXPECT_EQ (plan.minX + uint32_t (plan.centreX), 500u);
    EXPECT_EQ (plan.minY + uint32_t (plan.centreY), 400u);
}

TEST (PickReadback, AClickNearTheEdgeSlidesTheBoxAndMOVESTheCursorInIt)
{
    // The reported "picking drifts near the edges". The box cannot be centred on
    // a cursor three pixels from the left, so it is clamped -- and if the vote is
    // then told the cursor is at the middle, it answers about a point five pixels
    // away from where the user is pointing.
    const auto plan = PlanPickReadback (3, 2, 1920, 1080, 8);
    ASSERT_TRUE (plan.valid);
    EXPECT_EQ (plan.minX, 0u);
    EXPECT_EQ (plan.minY, 0u);
    EXPECT_FLOAT_EQ (plan.centreX, 3.0f);
    EXPECT_FLOAT_EQ (plan.centreY, 2.0f);
}

TEST (PickReadback, TheFarEdgeClampsWithoutRunningOffTheTarget)
{
    constexpr uint32_t kW = 100;
    constexpr uint32_t kH = 50;
    const auto plan = PlanPickReadback (99, 49, kW, kH, 8);
    ASSERT_TRUE (plan.valid);
    // The box must stay wholly inside the target -- a src box past the edge is a
    // rejected copy, i.e. a click that silently resolves to the previous one.
    EXPECT_LE (plan.minX + plan.width, kW);
    EXPECT_LE (plan.minY + plan.height, kH);
    EXPECT_EQ (plan.minX, 92u);
    EXPECT_EQ (plan.minY, 42u);
    // And the cursor still addresses its own texel.
    EXPECT_EQ (plan.minX + uint32_t (plan.centreX), 99u);
    EXPECT_EQ (plan.minY + uint32_t (plan.centreY), 49u);
}

TEST (PickReadback, ATargetSmallerThanTheBoxShrinksTheBoxRatherThanFailing)
{
    // A viewport mid-resize, or a collapsed splitter. Refusing to pick here would
    // read as picking dying whenever the palette is dragged small.
    const auto plan = PlanPickReadback (2, 1, 5, 3, 8);
    ASSERT_TRUE (plan.valid);
    EXPECT_EQ (plan.width, 5u);
    EXPECT_EQ (plan.height, 3u);
    EXPECT_EQ (plan.minX, 0u);
    EXPECT_EQ (plan.minY, 0u);
    EXPECT_FLOAT_EQ (plan.centreX, 2.0f);
    EXPECT_FLOAT_EQ (plan.centreY, 1.0f);
}

TEST (PickReadback, OutsideTheTargetIsNotAPick)
{
    // ⚠️ INVALID, NOT CLAMPED. Clamping a cursor that is off the viewport would
    // manufacture a pick at the nearest edge pixel -- selecting something the
    // user never pointed at, which is worse than answering nothing.
    EXPECT_FALSE (PlanPickReadback (-1, 10, 800, 600, 8).valid);
    EXPECT_FALSE (PlanPickReadback (10, -1, 800, 600, 8).valid);
    EXPECT_FALSE (PlanPickReadback (800, 10, 800, 600, 8).valid);
    EXPECT_FALSE (PlanPickReadback (10, 600, 800, 600, 8).valid);
    EXPECT_FALSE (PlanPickReadback (0, 0, 0, 600, 8).valid);
    EXPECT_FALSE (PlanPickReadback (0, 0, 800, 0, 8).valid);
    EXPECT_FALSE (PlanPickReadback (0, 0, 800, 600, 0).valid);
}

TEST (PickReadback, ThePlanAndTheVoteAgreeOnWhichTexelTheCursorIsOn)
{
    // The two halves, joined -- the thing neither test alone can catch. An id is
    // written at the cursor's pixel in a full-size id buffer near the corner; the
    // box is planned, the buffer sampled through it exactly as Poll does, and the
    // vote must return that id and not the large surface filling the rest.
    constexpr uint32_t kW = 64;
    constexpr uint32_t kH = 64;
    constexpr int32_t kCursorX = 1;
    constexpr int32_t kCursorY = 60;
    std::vector<uint32_t> target (size_t (kW) * kH, 77u);   // a wall everywhere
    target[size_t (kCursorY) * kW + kCursorX] = 5u;         // a railing under the cursor

    const auto plan = PlanPickReadback (kCursorX, kCursorY, kW, kH, 8);
    ASSERT_TRUE (plan.valid);

    std::vector<uint32_t> box (size_t (plan.width) * plan.height, 0u);
    for (uint32_t y = 0; y < plan.height; ++y) {
        for (uint32_t x = 0; x < plan.width; ++x)
            box[y * plan.width + x] = target[size_t (plan.minY + y) * kW + (plan.minX + x)];
    }

    EXPECT_EQ (ResolvePickIdAt (box.data (), plan.width, plan.height, plan.centreX,
                                plan.centreY),
               5u);
}
