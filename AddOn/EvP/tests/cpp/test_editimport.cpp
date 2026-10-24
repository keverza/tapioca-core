#include "Edit/EditImport.hpp"
#include "Edit/EditShape.hpp"
#include <gtest/gtest.h>
#include <cmath>

namespace ed = geomsrv::edit;

namespace {
constexpr double kPi = 3.14159265358979323846;

// Every sampled point of every segment, for comparing against the true curve.
std::vector<ed::Vec3> Samples (const ed::Entity& e)
{
    std::vector<ed::Vec3> out;
    for (size_t i = 0; i < e.segments.size (); ++i)
        for (const auto& p : ed::SegmentPoints (e, i))
            out.push_back (p);
    return out;
}
} // namespace

TEST (EditImport, AStraightRunKeepsItsPointsAndNoHandles)
{
    ed::Document doc;
    std::string error;
    const auto id =
        ed::AddImportedCurve (doc, { { { 0, 0, 2 }, { 4, 0, 2 }, { 4, 3, 2 } }, {}, false }, "imported", error);
    ASSERT_NE (id, ed::kNoId) << error;
    const auto& e = *ed::Find (doc, id);
    EXPECT_FALSE (e.closed);
    ASSERT_EQ (e.vertices.size (), 3u);
    for (const auto& v : e.vertices) {
        EXPECT_EQ (v.in, ed::Vec3 {});
        EXPECT_EQ (v.out, ed::Vec3 {});
    }
    EXPECT_EQ (e.role, "imported");
    EXPECT_EQ (ed::AddImportedCurve (doc, { { { 1, 1, 0 } }, {}, false }, "", error), ed::kNoId);
}

TEST (EditImport, AnArcSegmentBulgesTheWayItTurnsAndStaysOnItsCircle)
{
    for (const double angle : { kPi, -kPi, kPi * 0.5, kPi * 1.5 }) {
        ed::Document doc;
        std::string error;
        const auto id =
            ed::AddImportedCurve (doc, { { { 0, 0, 0 }, { 2, 0, 0 }, { 2, 5, 0 } }, { angle, 0.0 }, false }, "", error);
        ASSERT_NE (id, ed::kNoId) << error;
        const auto& e = *ed::Find (doc, id);
        // The arc's own circle: centre (L/2)/tan(angle/2) along the chord's left normal.
        const double cy = 1.0 / std::tan (angle * 0.5), radius = std::hypot (1.0, cy);
        size_t onArc = 0;
        for (size_t i = 0; i < e.segments.size (); ++i) {
            size_t from = 0, to = 0;
            ed::SegmentEnds (e, i, from, to);
            if (e.vertices[from].p.x > 2.0 - 1e-12 && e.vertices[to].p.y > 1.0)
                continue; // the straight leg up
            for (const auto& p : ed::SegmentPoints (e, i)) {
                EXPECT_NEAR (std::hypot (p.x - 1.0, p.y - cy), radius, ed::kImportArcError * radius) << angle;
                ++onArc;
            }
        }
        EXPECT_GT (onArc, 4u);
        const auto samples = Samples (e);
        double lowest = 0.0, highest = 0.0;
        for (const auto& p : samples)
            if (p.x > 0.0 && p.x < 2.0)
                lowest = std::min (lowest, p.y), highest = std::max (highest, p.y);
        // Counter-clockwise from (0, 0) to (2, 0) runs below the chord, to the circle's lowest point;
        // clockwise above it, to its highest.
        if (angle > 0)
            EXPECT_NEAR (lowest, cy - radius, 2 * ed::kImportArcError * radius) << angle;
        else
            EXPECT_NEAR (highest, cy + radius, 2 * ed::kImportArcError * radius) << angle;
        // A quarter turn at most per piece.
        EXPECT_EQ (e.vertices.size (), 3u + size_t (std::ceil (std::abs (angle) / (kPi / 2) - 1e-9)) - 1);
    }
}

TEST (EditImport, AClosedOutlineWhoseLastSegmentIsAnArcClosesSmoothlyOnItsFirstPoint)
{
    ed::Document doc;
    std::string error;
    // A 2 x 2 square whose closing side (0, 2) -> (0, 0) is a half circle out to the left.
    const auto id = ed::AddImportedCurve (
        doc, { { { 0, 0, 0 }, { 2, 0, 0 }, { 2, 2, 0 }, { 0, 2, 0 }, { 0, 0, 0 } }, { 0, 0, 0, kPi }, true }, "",
        error);
    ASSERT_NE (id, ed::kNoId) << error;
    const auto& e = *ed::Find (doc, id);
    EXPECT_TRUE (e.closed);
    ASSERT_EQ (e.vertices.size (), 5u) << "four corners, and the half circle's middle";
    EXPECT_NE (e.vertices[0].in, ed::Vec3 {}) << "the arc arrives at the first point with its handle";
    for (const auto& p : Samples (e))
        if (p.x < -1e-9)
            EXPECT_NEAR (std::hypot (p.x, p.y - 1.0), 1.0, ed::kImportArcError);
}

TEST (EditImport, ACircleAndARotatedEllipticArcLieOnTheirCurves)
{
    ed::Document doc;
    std::string error;
    ed::ImportedEllipse circle;
    circle.centre = { 5, 5, 1 };
    circle.radius = 3;
    const auto id = ed::AddImportedEllipse (doc, circle, "", error);
    ASSERT_NE (id, ed::kNoId) << error;
    const auto& c = *ed::Find (doc, id);
    EXPECT_TRUE (c.closed);
    EXPECT_EQ (c.vertices.size (), 4u);
    for (const auto& p : Samples (c)) {
        EXPECT_NEAR (std::hypot (p.x - 5, p.y - 5), 3.0, ed::kImportArcError * 3.0);
        EXPECT_DOUBLE_EQ (p.z, 1.0);
    }
    ed::ImportedEllipse arc;
    arc.radius = 4;
    arc.ratio = 0.5;
    arc.axisAngle = kPi / 6;
    arc.whole = false;
    arc.from = 0.0;
    arc.to = kPi; // half of it, from the main axis's end round to its other end
    const auto aid = ed::AddImportedEllipse (doc, arc, "", error);
    ASSERT_NE (aid, ed::kNoId) << error;
    const auto& a = *ed::Find (doc, aid);
    EXPECT_FALSE (a.closed);
    ASSERT_EQ (a.vertices.size (), 3u);
    EXPECT_NEAR (a.vertices.front ().p.x, 4 * std::cos (kPi / 6), 1e-12);
    EXPECT_NEAR (a.vertices.back ().p.x, -4 * std::cos (kPi / 6), 1e-12);
    for (const auto& p : Samples (a)) {
        // Back in the ellipse's own frame: (u / 4)^2 + (v / 2)^2 = 1.
        const double u = p.x * std::cos (kPi / 6) + p.y * std::sin (kPi / 6);
        const double v = -p.x * std::sin (kPi / 6) + p.y * std::cos (kPi / 6);
        EXPECT_NEAR (std::hypot (u / 4, v / 2), 1.0, ed::kImportArcError);
    }
    ed::ImportedEllipse flat = circle;
    flat.ratio = 0;
    EXPECT_EQ (ed::AddImportedEllipse (doc, flat, "", error), ed::kNoId);
}
