// The sun path's depth-tested scene geometry (#179): which analysis form draws
// what, that the highlighted day is drawn whole with only its slice bold, that
// runs and surfaces never bridge a missing step, and that colours are display
// sRGB linearised for the scene's sRGB target.

#include "ArchViz/SunPathLabels.hpp"
#include "ArchViz/SunPathScene.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

using namespace geomsrv::archviz;

namespace {

SunPathMoment Moment (int month, int day, int minute, float azimuth)
{
    SunPathMoment m;
    m.valid = true;
    m.year = 2028;
    m.month = month;
    m.day = day;
    m.minute = minute;
    m.step = minute / 60;
    m.direction[0] = std::cos (azimuth) * 0.6f;
    m.direction[1] = std::sin (azimuth) * 0.6f;
    m.direction[2] = 0.8f;
    return m;
}

// A traced day with one sample per hour across `fromHour`..`toHour`, in one run.
SunPathBandDay Day (int month, int day, int fromHour, int toHour, bool discrete = false)
{
    SunPathBandDay result;
    result.year = 2028;
    result.month = month;
    result.day = day;
    result.discrete = discrete;
    result.runs.emplace_back ();
    for (int hour = fromHour; hour <= toHour; ++hour)
        result.runs.back ().push_back (Moment (month, day, hour * 60, float (hour) * 0.2f + float (day) * 0.01f));
    return result;
}

SunPathBand Band (std::vector<SunPathBandDay> days)
{
    SunPathBand band;
    for (const auto& day : days)
        for (const auto& run : day.runs)
            band.steps += run.size ();
    band.days = std::move (days);
    return band;
}

SunPathFrame Frame ()
{
    SunPathFrame frame;
    frame.centre[0] = 10.0;
    frame.centre[1] = -5.0;
    frame.radius = 20.0;
    return frame;
}

size_t Segments (const GhPreviewBucket& bucket)
{
    return bucket.lineVertices.size () / 6;
}

size_t SegmentsOf (const GhPreviewBucket& bucket, uint32_t rgba)
{
    size_t count = 0;
    for (size_t i = 0; i < bucket.lineVertices.size (); i += 6)
        count += bucket.lineVertices[i].rgba == rgba ? 1 : 0;
    return count;
}

// The builder's subdivision: pieces of at most kSunPathMaxPieceRadians.
size_t PiecesBetween (const SunPathMoment& a, const SunPathMoment& b)
{
    double dot = 0.0, la = 0.0, lb = 0.0;
    for (int i = 0; i < 3; ++i) {
        dot += double (a.direction[i]) * b.direction[i];
        la += double (a.direction[i]) * a.direction[i];
        lb += double (b.direction[i]) * b.direction[i];
    }
    const double angle = std::acos (std::clamp (dot / std::sqrt (la * lb), -1.0, 1.0));
    return size_t (std::clamp (int (std::ceil (angle / kSunPathMaxPieceRadians)), 1, 256));
}

size_t PiecesOf (const SunPathBandDay& day, int fromMinute = 0, int toMinute = 1440)
{
    size_t pieces = 0;
    for (const auto& run : day.runs)
        for (size_t i = 1; i < run.size (); ++i)
            if (run[i - 1].minute >= fromMinute && run[i].minute < toMinute)
                pieces += PiecesBetween (run[i - 1], run[i]);
    return pieces;
}

uint32_t Amber (int alpha)
{
    return SunPathDisplayRgba (255, 184, 28, alpha);
}

} // namespace

TEST (SunPathScene, FormFollowsTheShownAnalysis)
{
    EXPECT_EQ (SunPathPreviewFormOf ({}, false, false), SunPathPreviewForm::None);
    const auto band = Band ({ Day (3, 21, 8, 16) });
    EXPECT_EQ (SunPathPreviewFormOf (band, false, false), SunPathPreviewForm::Day);
    EXPECT_EQ (SunPathPreviewFormOf (band, true, false), SunPathPreviewForm::EveryDay);
    EXPECT_EQ (SunPathPreviewFormOf (band, true, true), SunPathPreviewForm::MonthlyPreview);
}

TEST (SunPathScene, DisplayColoursAreLinearisedForTheSrgbTargetButAlphaIsNot)
{
    EXPECT_EQ (SunPathDisplayRgba (255, 255, 255, 7), 0xFFFFFF07u);
    EXPECT_EQ (SunPathDisplayRgba (0, 0, 0, 255), 0x000000FFu);
    const uint32_t mid = SunPathDisplayRgba (188, 188, 188, 128);
    EXPECT_NEAR (double (mid >> 24), 128.0, 1.0); // sRGB 188 is linear one half
    EXPECT_EQ (mid & 0xFFu, 128u);
    // The analysis yellow keeps a yellow hue rather than collapsing to grey.
    const uint32_t yellow = SunPathDisplayRgba (255, 214, 64, 40);
    EXPECT_EQ (yellow >> 24, 255u);
    EXPECT_GT ((yellow >> 16) & 0xFFu, 150u);
    EXPECT_LT ((yellow >> 8) & 0xFFu, 20u);
}

TEST (SunPathScene, PointsSitOnTheDisplayDome)
{
    const auto frame = Frame ();
    const float up[3] = { 0.0f, 0.0f, 1.0f };
    float at[3];
    SunPathWorld (frame, up, at);
    EXPECT_FLOAT_EQ (at[0], 10.0f);
    EXPECT_FLOAT_EQ (at[1], -5.0f);
    EXPECT_FLOAT_EQ (at[2], 20.0f);
}

TEST (SunPathScene, DayFormDrawsItsArcWithSunsOnlyForDiscreteSteps)
{
    SunPathSceneGeometry continuous;
    AppendSunPathAnalysis (Frame (), Band ({ Day (3, 21, 8, 12) }), SunPathPreviewForm::Day, {}, continuous);
    EXPECT_EQ (Segments (continuous.arcs.depthTested), PiecesOf (Day (3, 21, 8, 12)));
    EXPECT_GT (Segments (continuous.arcs.depthTested), 4u) << "drawn as a curve, not four chords";
    EXPECT_TRUE (continuous.arcs.translucent.Empty ());
    EXPECT_TRUE (continuous.bold.depthTested.Empty ()) << "a slice over the whole day adds no bold line";
    EXPECT_EQ (continuous.surfaceQuads, 0u);

    SunPathSceneGeometry discrete;
    AppendSunPathAnalysis (Frame (), Band ({ Day (3, 21, 8, 12, true) }), SunPathPreviewForm::Day, {}, discrete);
    EXPECT_EQ (Segments (discrete.arcs.depthTested), PiecesOf (Day (3, 21, 8, 12, true)));
    EXPECT_FALSE (discrete.arcs.translucent.meshIndices.empty ()) << "five step suns";
}

TEST (SunPathScene, HighlightedDayIsDrawnWholeAndOnlyItsSliceIsBold)
{
    // The reported fault: the inactive line ran to the day's end but the
    // selected one stopped at the slice. The chosen day is now amber over its
    // whole length; only the slice is bold.
    const auto band = Band ({ Day (6, 20, 6, 20), Day (6, 21, 6, 20), Day (6, 22, 6, 20) });
    SunPathAnalysisView view;
    view.month = 6;
    view.day = 21;
    view.sliceFrom = 9 * 60;
    view.sliceTo = 12 * 60 + 1;
    SunPathSceneGeometry scene;
    AppendSunPathAnalysis (Frame (), band, SunPathPreviewForm::EveryDay, view, scene);
    EXPECT_EQ (SegmentsOf (scene.arcs.depthTested, Amber (255)), PiecesOf (band.days[1])) << "06:00 to 20:00";
    EXPECT_EQ (Segments (scene.bold.depthTested), PiecesOf (band.days[1], 9 * 60, 12 * 60 + 1))
        << "only 09-10, 10-11, 11-12";
    // The amber day is the last thing in its bucket, so no other day paints over it.
    ASSERT_GE (scene.arcs.depthTested.lineVertices.size (), 6u);
    EXPECT_EQ (scene.arcs.depthTested.lineVertices.back ().rgba, Amber (255));
}

TEST (SunPathScene, EveryDaySurfaceJoinsMatchingStepsAndLeavesHolesWhereADayLacksOne)
{
    // The middle day has no sun before 09:00: no quad may invent one.
    const auto band = Band ({ Day (1, 1, 8, 12), Day (1, 2, 9, 12), Day (1, 3, 8, 12) });
    SunPathSceneGeometry scene;
    AppendSunPathAnalysis (Frame (), band, SunPathPreviewForm::EveryDay, {}, scene);
    EXPECT_EQ (scene.surfaceStride, 1u);
    EXPECT_EQ (scene.surfaceQuads, 2u * (3u + 3u)) << "only 09-12 on both rows, each split in two halves";
    EXPECT_EQ (scene.arcs.translucent.meshIndices.size (), scene.surfaceQuads * 6);
    EXPECT_TRUE (scene.arcs.depthTested.meshIndices.empty ()) << "the surface never writes depth";
    // The first and last day are the boundary lines.
    EXPECT_EQ (Segments (scene.arcs.depthTested), PiecesOf (band.days[0]) + PiecesOf (band.days[2]));
}

TEST (SunPathScene, LongEveryDayPeriodBoundsItsSurfaceRowsAndSaysSo)
{
    std::vector<SunPathBandDay> days;
    for (int d = 0; d < 365; ++d)
        days.push_back (Day (d / 31 + 1, d % 31 + 1, 8, 16));
    const auto band = Band (std::move (days));
    SunPathSceneGeometry scene;
    AppendSunPathAnalysis (Frame (), band, SunPathPreviewForm::EveryDay, {}, scene);
    EXPECT_EQ (scene.surfaceStride, 5u);
    EXPECT_LE (scene.surfaceQuads, 2u * 74u * 8u);
    const auto caption = SunPathSceneCaption (SunPathPreviewForm::EveryDay, band, scene, false);
    EXPECT_NE (caption.find ("365 traced days"), std::string::npos);
    EXPECT_NE (caption.find ("joined every 5 traced days"), std::string::npos);
    // A line where each month starts, plus the two boundary days.
    EXPECT_EQ (scene.dayLines, 11u + 2u);
}

TEST (SunPathScene, RunsAreNeverBridgedAcrossAHorizonGap)
{
    auto day = Day (3, 21, 6, 8);
    day.runs.emplace_back ();
    for (int hour = 16; hour <= 18; ++hour)
        day.runs.back ().push_back (Moment (3, 21, hour * 60, float (hour)));
    SunPathSceneGeometry scene;
    AppendSunPathAnalysis (Frame (), Band ({ day }), SunPathPreviewForm::Day, {}, scene);
    EXPECT_EQ (Segments (scene.arcs.depthTested), PiecesOf (day)) << "no 08:00 -> 16:00 segment";
    SunPathBandDay bridged = day; // what a bridge would have drawn
    bridged.runs[0].insert (bridged.runs[0].end (), day.runs[1].begin (), day.runs[1].end ());
    bridged.runs.pop_back ();
    EXPECT_LT (Segments (scene.arcs.depthTested), PiecesOf (bridged));
}

TEST (SunPathScene, MonthlyPreviewDrawsEachRepresentativeDayAndHighlightsTheMonth)
{
    const auto band = Band ({ Day (1, 15, 9, 15), Day (2, 15, 9, 15), Day (3, 15, 9, 15) });
    SunPathAnalysisView view;
    view.month = 2; // a whole-month frame: no single day
    SunPathSceneGeometry scene;
    AppendSunPathAnalysis (Frame (), band, SunPathPreviewForm::MonthlyPreview, view, scene);
    EXPECT_EQ (scene.dayLines, 3u);
    // The highlighted month is bold amber over its whole length: the month
    // player's cue was barely visible as a 2 px line.
    EXPECT_EQ (SegmentsOf (scene.bold.depthTested, Amber (255)), PiecesOf (band.days[1]));
    EXPECT_EQ (Segments (scene.bold.depthTested), PiecesOf (band.days[1]));
    EXPECT_EQ (Segments (scene.arcs.depthTested), PiecesOf (band.days[0]) + PiecesOf (band.days[2]));
    EXPECT_EQ (scene.surfaceQuads, 24u) << "two rows of six steps, each split in two halves";
    // February owns the half of each row on its side: its own span, amber, not
    // the two whole rows (about two months of sky) it used to light.
    size_t amber = 0, yellow = 0;
    for (const auto& vertex : scene.arcs.translucent.meshVertices) {
        amber += vertex.rgba == Amber (130) ? 1 : 0;
        yellow += vertex.rgba == SunPathDisplayRgba (255, 214, 64, 30) ? 1 : 0;
    }
    EXPECT_EQ (amber, 12u * 4u);
    EXPECT_EQ (yellow, 12u * 4u);
    const auto caption = SunPathSceneCaption (SunPathPreviewForm::MonthlyPreview, band, scene, true);
    EXPECT_NE (caption.find ("one representative day per month"), std::string::npos);
    EXPECT_NE (caption.find ("calendar weights"), std::string::npos);
}

TEST (SunPathScene, CompassAndSelectedSun)
{
    const auto frame = Frame ();
    SunPathSceneGeometry scene;
    AppendSunPathCompass (frame, 0.0, scene);
    EXPECT_EQ (Segments (scene.fine.depthTested), 3u * 720u + 31u);
    ASSERT_EQ (Segments (scene.arcs.depthTested), 1u) << "north alone is red and wider";
    const auto& north = scene.arcs.depthTested.lineVertices[0];
    EXPECT_EQ (north.rgba, SunPathDisplayRgba (220, 60, 60, 255));
    EXPECT_FLOAT_EQ (north.z, 0.0f) << "on the ground plane";
    EXPECT_GT (north.x, 10.0f) << "north at +X when northRadians is 0";

    const float direction[3] = { 0.0f, 0.6f, 0.8f };
    AppendSunPathSun (frame, direction, scene);
    ASSERT_FALSE (scene.arcs.depthTested.meshVertices.empty ());
    double centre[3] = {};
    for (const auto& vertex : scene.arcs.depthTested.meshVertices) {
        centre[0] += vertex.x;
        centre[1] += vertex.y;
        centre[2] += vertex.z;
        EXPECT_NE (vertex.nx * vertex.nx + vertex.ny * vertex.ny + vertex.nz * vertex.nz, 0.0f) << "shaded";
    }
    const double n = double (scene.arcs.depthTested.meshVertices.size ());
    EXPECT_NEAR (centre[0] / n, 10.0, 0.05);
    EXPECT_NEAR (centre[1] / n, -5.0 + 12.0, 0.05);
    EXPECT_NEAR (centre[2] / n, 16.0, 0.6) << "pole-weighted UV sphere centroid";
}

TEST (SunPathScene, ArcPiecesFollowTheDomeAndEndExactlyOnTheStoredDirections)
{
    const auto frame = Frame ();
    const float a[3] = { 1.0f, 0.0f, 0.0f };
    const float b[3] = { std::cos (0.1745f), std::sin (0.1745f), 0.0f }; // ten degrees
    GhPreviewBucket bucket;
    AppendSunPathArcPiece (frame, a, b, 0xFFFFFFFFu, bucket);
    const size_t pieces = Segments (bucket);
    EXPECT_EQ (pieces, 34u); // ceil (10 / 0.3)
    float worldA[3], worldB[3];
    SunPathWorld (frame, a, worldA);
    SunPathWorld (frame, b, worldB);
    const auto& first = bucket.lineVertices.front ();
    EXPECT_FLOAT_EQ (first.x, worldA[0]);
    EXPECT_FLOAT_EQ (first.y, worldA[1]);
    // Each segment's six corners name its two ends; the last segment ends at b.
    const auto& last = bucket.lineVertices[bucket.lineVertices.size () - 6];
    EXPECT_NEAR (last.ox, worldB[0], 1e-4);
    EXPECT_NEAR (last.oy, worldB[1], 1e-4);
    for (const auto& vertex : bucket.lineVertices) {
        const double r =
            std::hypot (vertex.x - frame.centre[0], vertex.y - frame.centre[1], vertex.z - frame.centre[2]);
        EXPECT_NEAR (r, frame.radius, 1e-3) << "every piece lies on the dome, not on the chord";
    }
    GhPreviewBucket same;
    AppendSunPathArcPiece (frame, a, a, 0xFFFFFFFFu, same);
    EXPECT_TRUE (same.lineVertices.empty ()) << "a zero-length hop draws nothing";
}

// #182: the sunpath's words are projected at draw time and hidden by depth.
namespace {
// Row-vector clip = (0.01x, 0.01y, 0.01z + 0.3, 1), as SunPathGui's fixture.
constexpr float kLabelMatrix[16] = { 0.01f, 0, 0, 0, 0, 0.01f, 0, 0, 0, 0, 0.01f, 0, 0, 0, 0.3f, 1 };
} // namespace

TEST (SunPathLabels, ProjectedAtTheAnchorHiddenByDepthAndKeptInPlace)
{
    SunPathLabelSpec spec;
    spec.anchor[0] = 10.0;
    spec.anchor[1] = 20.0;
    spec.anchor[2] = 5.0;
    spec.text = "N";
    const auto labels = ProjectSunPathLabels ({ spec }, kLabelMatrix, 800, 600, 1.0f);
    ASSERT_EQ (labels.size (), 1u);
    const auto& label = labels[0];
    EXPECT_FLOAT_EQ (label.depthAnchor.x, 440.0f);
    EXPECT_FLOAT_EQ (label.depthAnchor.y, 240.0f);
    EXPECT_NEAR (label.anchorDepth, 0.35f, 1e-6f);
    EXPECT_TRUE (label.hideWhenOccluded);
    EXPECT_FALSE (label.fadeWhenOccluded);
    EXPECT_TRUE (label.allowOverlap) << "a ring label must not be displaced off its ring";
    EXPECT_FLOAT_EQ (label.anchor.x, 440.0f);
    EXPECT_LT (label.anchor.y, 240.0f) << "a centred label is lifted to sit on its anchor";
    EXPECT_FLOAT_EQ (label.rotationRadians, 0.0f);
}

TEST (SunPathLabels, CompassTextFollowsTheProjectedTangentAndEdgeOnOrBehindIsDropped)
{
    SunPathLabelSpec along;
    along.text = "E";
    along.tangent[0] = 0.01;
    SunPathLabelSpec up = along;
    up.tangent[0] = 0.0;
    up.tangent[1] = 0.01; // +y world is up the screen
    SunPathLabelSpec edgeOn = along;
    edgeOn.tangent[0] = 0.0;
    edgeOn.tangent[2] = 0.01; // depth only: no screen direction
    SunPathLabelSpec behind = along;
    behind.anchor[2] = -40.0; // clip z < 0
    const auto labels = ProjectSunPathLabels ({ along, up, edgeOn, behind }, kLabelMatrix, 800, 600, 1.0f);
    ASSERT_EQ (labels.size (), 2u);
    EXPECT_NEAR (labels[0].rotationRadians, 0.0f, 1e-6f);
    EXPECT_NEAR (labels[1].rotationRadians, -1.5707963f, 1e-5f);
}

TEST (SunPathLabels, TooltipLinesSampleDepthAtTheSunAndScaleWithDpi)
{
    SunPathLabelSpec line;
    line.anchor[0] = 10.0;
    line.anchor[1] = 20.0;
    line.anchor[2] = 5.0;
    line.text = "Sun: 2028-02-29  10:05";
    line.centered = false;
    line.offsetX = 10.0f;
    line.offsetY = 14.0f;
    line.panel = true;
    const auto labels = ProjectSunPathLabels ({ line }, kLabelMatrix, 800, 600, 2.0f);
    ASSERT_EQ (labels.size (), 1u);
    EXPECT_FLOAT_EQ (labels[0].anchor.x, 460.0f);
    EXPECT_FLOAT_EQ (labels[0].anchor.y, 268.0f);
    EXPECT_FLOAT_EQ (labels[0].depthAnchor.x, 440.0f) << "depth is read at the sun, not under the plate";
    EXPECT_FLOAT_EQ (labels[0].fontSize, 26.0f);
    EXPECT_TRUE (labels[0].backgroundPanel);
    EXPECT_TRUE (ProjectSunPathLabels ({ line }, nullptr, 800, 600, 1.0f).empty ());
}

TEST (SunPathScene, SurfaceHalvesMeetAtTheRowMiddleOnTheDome)
{
    // Two traced days: the halves share their inner edge, which lies on the
    // dome between the two exact rows.
    const auto band = Band ({ Day (1, 15, 10, 11), Day (2, 15, 10, 11) });
    SunPathSceneGeometry scene;
    AppendSunPathAnalysis (Frame (), band, SunPathPreviewForm::MonthlyPreview, {}, scene);
    ASSERT_EQ (scene.surfaceQuads, 2u);
    const auto& v = scene.arcs.translucent.meshVertices;
    ASSERT_EQ (v.size (), 8u);
    // First half: corners 2/3 are at t = 0.5; second half: corners 0/1 at t = 0.5.
    EXPECT_FLOAT_EQ (v[3].x, v[4].x);
    EXPECT_FLOAT_EQ (v[3].y, v[4].y);
    EXPECT_FLOAT_EQ (v[2].z, v[5].z);
    const auto frame = Frame ();
    for (const auto& vertex : v)
        EXPECT_NEAR (std::hypot (vertex.x - frame.centre[0], vertex.y - frame.centre[1], vertex.z - frame.centre[2]),
                     frame.radius, 1e-3);
}
