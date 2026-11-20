// tapioca-dev #130: polar arrays -- whole objects repeated about a vertical axis.
#include "Edit/EditArrange.hpp"
#include "Edit/EditEllipse.hpp"
#include "Edit/EditPath.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditPolyline.hpp"
#include "Edit/EditPrimitives.hpp"
#include "Edit/EditSession.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <set>

namespace ed = geomsrv::edit;

namespace {
constexpr double kPi = 3.14159265358979323846;

void Near (ed::Vec3 a, ed::Vec3 b, const char* what)
{
    EXPECT_NEAR (a.x, b.x, 1e-9) << what;
    EXPECT_NEAR (a.y, b.y, 1e-9) << what;
    EXPECT_NEAR (a.z, b.z, 1e-9) << what;
}

void Valid (const ed::Document& doc)
{
    std::string why;
    EXPECT_TRUE (ed::Validate (doc, why)) << why;
}
} // namespace

TEST (EditArrayPolar, AFullTurnSpreadsTheCountWithNoCopyBackOnTheSource130)
{
    EXPECT_DOUBLE_EQ (ed::PolarStep (6, 2.0 * kPi), kPi / 3.0);
    EXPECT_DOUBLE_EQ (ed::PolarStep (6, -2.0 * kPi), -kPi / 3.0);
    EXPECT_DOUBLE_EQ (ed::PolarStep (5, kPi), kPi / 4.0) << "a part turn ends on the sweep";
    EXPECT_DOUBLE_EQ (ed::PolarStep (1, kPi), 0.0);

    ed::Document doc;
    const ed::Id source = ed::AddPolyline (doc, { { 10, 0, 2 }, { 12, 0, 2 } }, false, "bench");
    std::vector<ed::Ref> placed;
    std::string error;
    ASSERT_TRUE (
        ed::PolarArray (doc, { { source, ed::Part::Entity, 0 } }, 4, { 0, 0, 99 }, 2.0 * kPi, true, placed, error))
        << error;
    ASSERT_EQ (placed.size (), 4u);
    EXPECT_EQ (placed[0].entity, source);
    std::set<std::pair<long, long>> starts;
    for (size_t k = 0; k < placed.size (); ++k) {
        const auto& item = *ed::Find (doc, placed[k].entity);
        const double a = kPi / 2.0 * double (k);
        Near (item.vertices[0].p, { 10 * std::cos (a), 10 * std::sin (a), 2 }, "start, turned and level");
        Near (item.vertices[1].p, { 12 * std::cos (a), 12 * std::sin (a), 2 }, "end, turned with the copy");
        EXPECT_EQ (item.role, "bench");
        starts.insert ({ std::lround (item.vertices[0].p.x), std::lround (item.vertices[0].p.y) });
    }
    EXPECT_EQ (starts.size (), 4u) << "no two items coincide";
    Valid (doc);
}

TEST (EditArrayPolar, APartTurnEndsOnItsSweepAndUnrotatedCopiesOnlyMove130)
{
    ed::Document doc;
    // A 2 x 1 box centred at (10, 0).
    const ed::Id source = ed::AddPolyline (doc, ed::Rectangle ({ 9, -0.5, 0 }, { 11, 0.5, 0 }), true);
    std::vector<ed::Ref> placed;
    std::string error;
    ASSERT_TRUE (ed::PolarArray (doc, { { source, ed::Part::Entity, 0 } }, 3, {}, -kPi / 2.0, false, placed, error))
        << error;
    ASSERT_EQ (placed.size (), 3u);
    const auto& original = *ed::Find (doc, source);
    for (size_t k = 1; k < 3; ++k) {
        const auto& item = *ed::Find (doc, placed[k].entity);
        const double a = -kPi / 4.0 * double (k);
        const ed::Vec3 offset { 10 * std::cos (a) - 10, 10 * std::sin (a), 0 };
        for (size_t v = 0; v < item.vertices.size (); ++v)
            Near (item.vertices[v].p, original.vertices[v].p + offset, "moved, not turned");
    }
    // The last copy sits on the sweep's end: straight down the -Y axis.
    const auto& last = *ed::Find (doc, placed[2].entity);
    double cx = 0, cy = 0;
    for (const auto& v : last.vertices)
        cx += v.p.x / 4.0, cy += v.p.y / 4.0;
    EXPECT_NEAR (cx, 0.0, 1e-9);
    EXPECT_NEAR (cy, -10.0, 1e-9);
    Valid (doc);
}

TEST (EditArrayPolar, CirclesAndPathsTurnWholeAndKeepTheirDefinitions130)
{
    ed::Document doc;
    ed::PrimitiveInput input;
    input.kind = ed::Primitive::Ellipse;
    input.first = { 8, 0, 1 };
    input.second = { 12, 2, 1 };
    std::string error;
    const ed::Id ellipse = ed::AddPrimitive (doc, input, "bed", error);
    ASSERT_NE (ellipse, ed::kNoId) << error;
    const ed::Id path = ed::AddPath (doc, { { 5, 0, 0 }, { 15, 0, 0 }, { 15, 6, 0 } }, 1.5, "walk");
    const ed::Path originalPath = *ed::FindPath (doc, path);
    const auto originalEllipse = *ed::Find (doc, ellipse)->ellipse;

    std::vector<ed::Ref> placed;
    ASSERT_TRUE (ed::PolarArray (doc, { { ellipse, ed::Part::Entity, 0 }, { path, ed::Part::Entity, 0 } }, 3, {}, kPi,
                                 true, placed, error))
        << error;
    ASSERT_EQ (placed.size (), 6u);
    EXPECT_EQ (placed[0].entity, path) << "paths before entities, source first";
    EXPECT_EQ (placed[1].entity, ellipse);
    for (size_t k = 1; k < 3; ++k) {
        const double a = kPi / 2.0 * double (k);
        const auto& copyPath = *ed::FindPath (doc, placed[2 * k].entity);
        ASSERT_EQ (copyPath.points.size (), originalPath.points.size ());
        for (size_t i = 0; i < copyPath.points.size (); ++i)
            Near (copyPath.points[i].p, ed::TurnInPlan (originalPath.points[i].p, {}, a), "path point");
        EXPECT_EQ (copyPath.profile.size (), originalPath.profile.size ()) << "the profile follows by station";
        EXPECT_EQ (copyPath.role, "walk");

        const auto& copy = *ed::Find (doc, placed[2 * k + 1].entity);
        ASSERT_TRUE (copy.ellipse.has_value ());
        Near (copy.ellipse->center, ed::TurnInPlan (originalEllipse.center, {}, a), "ellipse centre");
        Near (copy.ellipse->axisU, ed::TurnInPlan (originalEllipse.axisU, {}, a), "ellipse axis turns with it");
        EXPECT_TRUE (ed::ValidateEllipse (copy, error)) << error;
    }
    Valid (doc);
}

TEST (EditArrayPolar, ALockedSourceVertexPinsTheSourceNotItsCopies130)
{
    ed::Document doc;
    const ed::Id source = ed::AddPolyline (doc, { { 3, 0, 0 }, { 4, 0, 0 } }, false);
    ed::Find (doc, source)->vertices[0].lockXY = true;
    std::vector<ed::Ref> placed;
    std::string error;
    ASSERT_TRUE (ed::PolarArray (doc, { { source, ed::Part::Entity, 0 } }, 2, {}, kPi, true, placed, error)) << error;
    Near (ed::Find (doc, placed[1].entity)->vertices[0].p, { -3, 0, 0 }, "the copy turned");
    Near (ed::Find (doc, source)->vertices[0].p, { 3, 0, 0 }, "the source stayed");
}

TEST (EditArrayPolar, InvalidInputLeavesTheDocumentIdsAndOutputUntouched130)
{
    ed::Document doc;
    const ed::Id source = ed::AddPolyline (doc, { { 1, 0, 0 }, { 2, 0, 0 } }, false);
    const std::vector<ed::Ref> refs { { source, ed::Part::Entity, 0 } };
    const auto entities = doc.entities;
    const ed::Id nextId = doc.nextId;
    const std::vector<ed::Ref> sentinel { { 999, ed::Part::Entity, 0 } };
    const double inf = std::numeric_limits<double>::infinity ();
    const double nan = std::numeric_limits<double>::quiet_NaN ();
    struct Case {
        size_t count;
        ed::Vec3 centre;
        double sweep;
    };
    const std::vector<Case> cases { { 0, {}, kPi },
                                    { 10001, {}, kPi },
                                    { 3, {}, 0.0 },
                                    { 3, {}, nan },
                                    { 3, {}, inf },
                                    { 3, {}, 2.0 * kPi + 0.1 },
                                    { 3, { nan, 0, 0 }, kPi },
                                    { 3, { 0, inf, 0 }, kPi } };
    for (const auto& test : cases) {
        std::vector<ed::Ref> placed = sentinel;
        std::string error;
        EXPECT_FALSE (ed::PolarArray (doc, refs, test.count, test.centre, test.sweep, true, placed, error));
        EXPECT_FALSE (error.empty ());
        EXPECT_EQ (placed, sentinel);
        EXPECT_EQ (doc.entities, entities);
        EXPECT_EQ (doc.nextId, nextId);
    }
    std::vector<ed::Ref> placed = sentinel;
    std::string error;
    EXPECT_FALSE (ed::PolarArray (doc, {}, 3, {}, kPi, true, placed, error));
    EXPECT_EQ (placed, sentinel);

    doc.nextId = std::numeric_limits<ed::Id>::max ();
    EXPECT_FALSE (ed::PolarArray (doc, refs, 3, {}, kPi, true, placed, error));
    EXPECT_EQ (placed, sentinel);
    EXPECT_EQ (doc.entities, entities);

    // One item is a no-op that names the source.
    ed::Document one;
    const ed::Id lone = ed::AddPolyline (one, { { 1, 0, 0 }, { 2, 0, 0 } }, false);
    const auto before = one.entities;
    ASSERT_TRUE (ed::PolarArray (one, { { lone, ed::Part::Entity, 0 } }, 1, {}, 0.0, true, placed, error)) << error;
    ASSERT_EQ (placed.size (), 1u);
    EXPECT_EQ (one.entities, before);
}

TEST (EditArrayPolar, ThroughTheSessionIsOneUndoStep130)
{
    ed::Document doc;
    const ed::Id source = ed::AddPolyline (doc, { { 5, 0, 1 }, { 6, 0, 1 } }, false);
    ed::Session session (doc);
    std::vector<ed::Ref> placed;
    ASSERT_TRUE (session.Apply (
        [&] (ed::Document& candidate, std::string& error) {
            return ed::PolarArray (candidate, { { source, ed::Part::Entity, 0 } }, 8, {}, 2.0 * kPi, true, placed,
                                   error);
        },
        false))
        << session.Message ();
    EXPECT_EQ (session.Doc ().entities.size (), 8u);
    EXPECT_EQ (session.UndoDepth (), 1u);
    ASSERT_TRUE (session.Undo ());
    EXPECT_EQ (session.Doc ().entities.size (), 1u);
    ASSERT_TRUE (session.Redo ());
    EXPECT_EQ (session.Doc ().entities.size (), 8u);
}
