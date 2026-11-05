#include "Edit/EditPolyline.hpp"
#include "Edit/EditSession.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace ed = geomsrv::edit;

namespace {
constexpr double kPi = 3.14159265358979323846;

void Near (ed::Vec3 p, double x, double y, double z = 0.0)
{
    EXPECT_NEAR (p.x, x, 1e-9);
    EXPECT_NEAR (p.y, y, 1e-9);
    EXPECT_NEAR (p.z, z, 1e-9);
}

void Valid (const ed::Document& doc)
{
    std::string error;
    EXPECT_TRUE (ed::Validate (doc, error)) << error;
}

ed::Entity& E (ed::Document& doc, ed::Id id)
{
    return *ed::Find (doc, id);
}

const ed::Entity& E (const ed::Document& doc, ed::Id id)
{
    return *ed::Find (doc, id);
}

ed::Operation OffsetAction (const ed::ActionDefaults& defaults, const ed::Document& doc,
                            const std::vector<ed::Id>& entities)
{
    std::vector<ed::Ref> refs;
    for (const ed::Id entity : entities)
        refs.push_back ({ entity, ed::Part::Entity, 0 });
    for (const auto& action : ed::PolylineActions (defaults))
        if (action.id == "offset")
            return action.make (doc, refs);
    return {};
}

ed::Id OpenL (ed::Document& doc)
{
    const ed::Id id = ed::AddPolyline (doc, { { 0, 0, 1 }, { 4, 0, 1 }, { 4, 4, 2 } }, false, "edge");
    E (doc, id).segments[0].width = 1.25;
    E (doc, id).segments[0].tag = 11;
    E (doc, id).segments[1].width = 2.5;
    E (doc, id).segments[1].tag = 22;
    return id;
}
} // namespace

TEST (EditPolylineOffset, OpenJoinsKeepEndsAttributesAndInnerCornerSemantics)
{
    for (const ed::OffsetJoinStyle style :
         { ed::OffsetJoinStyle::Miter, ed::OffsetJoinStyle::Bevel, ed::OffsetJoinStyle::Round }) {
        ed::Document doc;
        const ed::Id source = OpenL (doc);
        E (doc, source).vertices[1].lockXY = E (doc, source).vertices[1].lockZ = true;
        const ed::Entity original = E (doc, source);
        ed::OffsetOptions options;
        options.join = style;
        ed::Id copy = 0;
        std::string error;
        ASSERT_TRUE (ed::OffsetEntity (doc, source, -1.0, options, error, &copy)) << error;
        const auto& made = E (doc, copy);
        const size_t expected = style == ed::OffsetJoinStyle::Miter   ? 3u
                                : style == ed::OffsetJoinStyle::Bevel ? 4u
                                                                      : 11u;
        ASSERT_EQ (made.vertices.size (), expected);
        Near (made.vertices.front ().p, 0, -1, 1);
        Near (made.vertices.back ().p, 5, 4, 2); // open ends shift, with no cap or extension
        if (style == ed::OffsetJoinStyle::Miter)
            Near (made.vertices[1].p, 5, -1, 1);
        else {
            Near (made.vertices[1].p, 4, -1, 1);
            if (style == ed::OffsetJoinStyle::Bevel)
                Near (made.vertices[2].p, 5, 0, 1);
            else {
                Near (made.vertices[5].p, 4 + std::sqrt (0.5), -std::sqrt (0.5), 1);
                for (size_t i = 1; i <= 9; ++i)
                    EXPECT_NEAR (ed::DistanceXY (made.vertices[i].p, { 4, 0, 1 }), 1.0, 1e-9);
                Near (made.vertices[9].p, 5, 0, 1);
            }
        }
        EXPECT_EQ (made.segments.front ().tag, 11u);
        EXPECT_EQ (made.segments.back ().tag, 22u);
        EXPECT_DOUBLE_EQ (made.segments.front ().width, 1.25);
        EXPECT_DOUBLE_EQ (made.segments.back ().width, 2.5);
        if (style != ed::OffsetJoinStyle::Miter)
            EXPECT_EQ (made.segments[1].tag, 11u); // inserted join pieces inherit the arriving run
        EXPECT_EQ (made.role, "edge");
        EXPECT_NE (made.id, source);
        for (const auto& vertex : made.vertices) {
            EXPECT_FALSE (vertex.lockXY);
            EXPECT_FALSE (vertex.lockZ);
        }
        EXPECT_EQ (E (doc, source), original);
        Valid (doc);

        ed::Id inner = 0;
        ASSERT_TRUE (ed::OffsetEntity (doc, source, 1.0, options, error, &inner)) << error;
        ASSERT_EQ (E (doc, inner).vertices.size (), 3u); // concave joins intersect for every style
        Near (E (doc, inner).vertices.front ().p, 0, 1, 1);
        Near (E (doc, inner).vertices[1].p, 3, 1, 1);
        Near (E (doc, inner).vertices.back ().p, 3, 4, 2);
        Valid (doc);
    }
}

TEST (EditPolylineOffset, ActionOffsetsEitherWindingWithStyledClosingJoinsAndOneUndo)
{
    for (const ed::OffsetJoinStyle style :
         { ed::OffsetJoinStyle::Miter, ed::OffsetJoinStyle::Bevel, ed::OffsetJoinStyle::Round })
        for (const bool clockwise : { false, true }) {
            ed::Document doc;
            auto points = ed::Rectangle ({ 0, 0, 0 }, { 2, 2, 0 });
            if (clockwise)
                std::reverse (points.begin (), points.end ());
            const ed::Id source = ed::AddPolyline (doc, points, true, "edge");
            for (size_t i = 0; i < E (doc, source).segments.size (); ++i)
                E (doc, source).segments[i].tag = uint32_t (i + 1);
            const ed::Entity original = E (doc, source);
            ed::ActionDefaults defaults;
            defaults.offsetDistance = 0.5;
            defaults.offsetJoinStyle = style;
            ed::Session session (doc);
            ASSERT_TRUE (session.Apply (OffsetAction (defaults, session.Doc (), { source }))) << session.Message ();
            ASSERT_EQ (session.Doc ().entities.size (), 2u);
            const auto& made = session.Doc ().entities[1];
            const size_t expectedVertices = style == ed::OffsetJoinStyle::Miter   ? 4u
                                            : style == ed::OffsetJoinStyle::Bevel ? 8u
                                                                                  : 36u;
            EXPECT_EQ (made.vertices.size (), expectedVertices);
            const double area = ed::SignedAreaXY (made.vertices);
            EXPECT_EQ (area > 0.0, ed::SignedAreaXY (original.vertices) > 0.0);
            const double expectedArea = style == ed::OffsetJoinStyle::Miter   ? 9.0
                                        : style == ed::OffsetJoinStyle::Bevel ? 8.5
                                                                              : 8.0 + 4.0 * std::sin (kPi / 16.0);
            EXPECT_NEAR (std::abs (area), expectedArea, 1e-9); // 8 sampled pieces per quarter-turn
            if (style != ed::OffsetJoinStyle::Miter) {
                EXPECT_EQ (made.segments.front ().tag, original.segments.back ().tag);
                EXPECT_EQ (made.segments[style == ed::OffsetJoinStyle::Round ? 8 : 1].tag,
                           original.segments.front ().tag);
            }
            EXPECT_EQ (made.role, original.role);
            EXPECT_EQ (session.UndoDepth (), 1u);
            Valid (session.Doc ());
            ASSERT_TRUE (session.Undo ());
            ASSERT_EQ (session.Doc ().entities.size (), 1u);
            EXPECT_EQ (session.Doc ().entities.front (), original);
        }
}

TEST (EditPolylineOffset, RoundJoinsHandleNonRightAnglesAndMixedClosedCorners)
{
    ed::Document doc;
    const double diagonal = std::sqrt (0.5);
    const ed::Id source =
        ed::AddPolyline (doc, { { 0, 0, 0 }, { 4, 0, 0 }, { 4 + 4 * diagonal, 4 * diagonal, 0 } }, false);
    ed::OffsetOptions options;
    options.join = ed::OffsetJoinStyle::Round;
    ed::Id copy = 0;
    std::string error;
    ASSERT_TRUE (ed::OffsetEntity (doc, source, -1.0, options, error, &copy)) << error;
    const auto& made = E (doc, copy);
    ASSERT_EQ (made.vertices.size (), 7u); // a 45-degree sweep takes four of the eight quarter pieces
    Near (made.vertices[3].p, 4 + std::cos (3.0 * kPi / 8.0), -std::sin (3.0 * kPi / 8.0));
    for (size_t i = 1; i <= 5; ++i)
        EXPECT_NEAR (ed::DistanceXY (made.vertices[i].p, { 4, 0, 0 }), 1.0, 1e-9);
    Valid (doc);

    ed::Document ring;
    const ed::Id concave =
        ed::AddPolyline (ring, { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 4, 0 }, { 2, 4, 0 }, { 2, 2, 0 }, { 0, 2, 0 } }, true);
    ed::ActionDefaults defaults;
    defaults.offsetDistance = 0.5;
    defaults.offsetJoinStyle = ed::OffsetJoinStyle::Round;
    ed::Session session (ring);
    ASSERT_TRUE (session.Apply (OffsetAction (defaults, session.Doc (), { concave }))) << session.Message ();
    const auto& offset = session.Doc ().entities[1];
    EXPECT_EQ (offset.vertices.size (), 46u); // five outer arcs and one intersecting concave join
    EXPECT_GT (ed::SignedAreaXY (offset.vertices), 0.0);
    EXPECT_TRUE (std::any_of (offset.vertices.begin (), offset.vertices.end (), [] (const ed::Vertex& vertex) {
        return ed::DistanceXY (vertex.p, { 1.5, 2.5, 0 }) < 1e-9;
    }));
    EXPECT_EQ (session.UndoDepth (), 1u);
    Valid (session.Doc ());
}

TEST (EditPolylineOffset, StyledMiterBevelsPastLimitWhileLegacyRefusesAtomically)
{
    ed::Document doc;
    const ed::Id source = ed::AddPolyline (doc, { { 0, 0, 0 }, { 4, 0, 0 }, { 0, 0.1, 0 } }, false);
    ed::OffsetOptions options;
    options.join = ed::OffsetJoinStyle::Miter;
    options.miterLimit = 2.0;
    ed::Id copy = 0;
    std::string error;
    ASSERT_TRUE (ed::OffsetEntity (doc, source, -1.0, options, error, &copy)) << error;
    ASSERT_EQ (E (doc, copy).vertices.size (), 4u);
    Near (E (doc, copy).vertices[1].p, 4, -1);
    EXPECT_NEAR (E (doc, copy).vertices[2].p.x, 4.0 + 0.1 / std::hypot (4.0, 0.1), 1e-9);
    EXPECT_NEAR (E (doc, copy).vertices[2].p.y, 4.0 / std::hypot (4.0, 0.1), 1e-9);
    Valid (doc);

    const auto before = doc.entities;
    const ed::Id nextId = doc.nextId;
    EXPECT_FALSE (ed::OffsetEntity (doc, source, -1.0, error));
    EXPECT_FALSE (error.empty ());
    EXPECT_EQ (doc.entities, before);
    EXPECT_EQ (doc.nextId, nextId);

    ed::Document rightAngle;
    const ed::Id corner = OpenL (rightAngle);
    options.miterLimit = std::sqrt (2.0);
    ASSERT_TRUE (ed::OffsetEntity (rightAngle, corner, -1.0, options, error, &copy)) << error;
    EXPECT_EQ (E (rightAngle, copy).vertices.size (), 3u); // exactly at the limit remains miter
    options.miterLimit = std::sqrt (2.0) - 1.0e-6;
    ASSERT_TRUE (ed::OffsetEntity (rightAngle, corner, -1.0, options, error, &copy)) << error;
    EXPECT_EQ (E (rightAngle, copy).vertices.size (), 4u); // just beyond it falls back to bevel
}

TEST (EditPolylineOffset, CollinearRunsSurviveAndReversalsRefuse)
{
    ed::Document doc;
    const ed::Id collinear = ed::AddPolyline (doc, { { 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 } }, false);
    ed::OffsetOptions options;
    options.join = ed::OffsetJoinStyle::Round;
    ed::Id copy = 0;
    std::string error;
    ASSERT_TRUE (ed::OffsetEntity (doc, collinear, 1.0, options, error, &copy)) << error;
    ASSERT_EQ (E (doc, copy).vertices.size (), 3u);
    Near (E (doc, copy).vertices[0].p, 0, 1);
    Near (E (doc, copy).vertices[1].p, 1, 1);
    Near (E (doc, copy).vertices[2].p, 2, 1);
    Valid (doc);

    const ed::Id reversing = ed::AddPolyline (doc, { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 0, 0 } }, false);
    const auto before = doc.entities;
    const ed::Id nextId = doc.nextId;
    EXPECT_FALSE (ed::OffsetEntity (doc, reversing, 1.0, options, error));
    EXPECT_EQ (doc.entities, before);
    EXPECT_EQ (doc.nextId, nextId);
}

TEST (EditPolylineOffset, CurvesLiveCornersAndInvalidOptionsRefuseWithoutMutation)
{
    ed::Document doc;
    const ed::Id source = OpenL (doc);
    ed::Id copy = 0;
    std::string error;
    ed::OffsetOptions options;
    options.join = ed::OffsetJoinStyle::Bevel;

    E (doc, source).vertices[1].out = { 1, 0, 0 };
    auto before = doc.entities;
    ed::Id nextId = doc.nextId;
    EXPECT_FALSE (ed::OffsetEntity (doc, source, 1.0, options, error, &copy));
    EXPECT_EQ (doc.entities, before);
    EXPECT_EQ (doc.nextId, nextId);
    EXPECT_FALSE (ed::OffsetEntity (doc, source, 1.0, error, &copy)); // legacy no longer treats a curve as a chord
    EXPECT_EQ (doc.entities, before);

    E (doc, source).vertices[1].out = {};
    E (doc, source).vertices[1].radius = 0.5;
    before = doc.entities;
    nextId = doc.nextId;
    EXPECT_FALSE (ed::OffsetEntity (doc, source, 1.0, options, error, &copy));
    EXPECT_EQ (doc.entities, before);
    EXPECT_EQ (doc.nextId, nextId);

    E (doc, source).vertices[1].radius = 0.0;
    before = doc.entities;
    nextId = doc.nextId;
    options.join = ed::OffsetJoinStyle::Miter;
    options.miterLimit = 0.5;
    EXPECT_FALSE (ed::OffsetEntity (doc, source, 1.0, options, error, &copy));
    options.miterLimit = 10.0;
    options.join = static_cast<ed::OffsetJoinStyle> (99);
    EXPECT_FALSE (ed::OffsetEntity (doc, source, 1.0, options, error, &copy));
    EXPECT_FALSE (ed::OffsetEntity (doc, source, std::numeric_limits<double>::quiet_NaN (), options, error, &copy));
    EXPECT_EQ (doc.entities, before);
    EXPECT_EQ (doc.nextId, nextId);
}

TEST (EditPolylineOffset, MultiSelectionActionRollsBackWhenALaterEntityRefuses)
{
    ed::Document doc;
    const ed::Id first = OpenL (doc);
    const ed::Id curved = OpenL (doc);
    E (doc, curved).vertices[1].out = { 1, 0, 0 };
    ed::Session session (doc);
    const auto before = session.Doc ().entities;
    const ed::Id nextId = session.Doc ().nextId;
    const uint64_t revision = session.Doc ().revision;
    ed::ActionDefaults defaults;
    defaults.offsetJoinStyle = ed::OffsetJoinStyle::Bevel;
    EXPECT_FALSE (session.Apply (OffsetAction (defaults, session.Doc (), { first, curved })));
    EXPECT_FALSE (session.Message ().empty ());
    EXPECT_EQ (session.Doc ().entities, before);
    EXPECT_EQ (session.Doc ().nextId, nextId);
    EXPECT_EQ (session.Doc ().revision, revision);
    EXPECT_EQ (session.UndoDepth (), 0u);
}

TEST (EditPolylineOffset, StyledExtrusionProfilesStayFlatAndRefuseCollapse)
{
    ed::Document doc;
    const ed::Id source = ed::AddExtrusion (doc, ed::Rectangle ({ 0, 0, 0 }, { 2, 2, 0 }), 2.5);
    ed::OffsetOptions options;
    options.join = ed::OffsetJoinStyle::Round;
    ed::Id copy = 0;
    std::string error;
    ASSERT_TRUE (ed::OffsetEntity (doc, source, 0.5, options, error, &copy)) << error;
    const auto& made = E (doc, copy);
    EXPECT_EQ (made.kind, ed::Kind::Extrusion);
    EXPECT_DOUBLE_EQ (made.height, 2.5);
    EXPECT_EQ (made.vertices.size (), 36u);
    EXPECT_GT (ed::SignedAreaXY (made.vertices), 0.0);
    EXPECT_NEAR (ed::SignedAreaXY (made.vertices), 8.0 + 4.0 * std::sin (kPi / 16.0), 1e-9);
    for (const auto& vertex : made.vertices)
        EXPECT_DOUBLE_EQ (vertex.p.z, 0.0);
    Valid (doc);

    const auto before = doc.entities;
    const ed::Id nextId = doc.nextId;
    EXPECT_FALSE (ed::OffsetEntity (doc, source, -1.5, options, error));
    EXPECT_EQ (doc.entities, before);
    EXPECT_EQ (doc.nextId, nextId);
}

TEST (EditPolylineOffset, ZeroAndTinyOffsetsDoNotCreateInvalidJoinGeometry)
{
    ed::Document doc;
    const ed::Id source = OpenL (doc);
    const ed::Entity original = E (doc, source);
    ed::OffsetOptions options;
    options.join = ed::OffsetJoinStyle::Bevel;
    ed::Id copy = 0;
    std::string error;
    ASSERT_TRUE (ed::OffsetEntity (doc, source, 0.0, options, error, &copy)) << error;
    ASSERT_EQ (E (doc, copy).vertices.size (), original.vertices.size ());
    for (size_t i = 0; i < original.vertices.size (); ++i)
        EXPECT_EQ (E (doc, copy).vertices[i].p, original.vertices[i].p);
    Valid (doc);

    options.join = ed::OffsetJoinStyle::Round;
    ASSERT_TRUE (ed::OffsetEntity (doc, source, -2.0e-6, options, error, &copy)) << error;
    const auto& tiny = E (doc, copy);
    for (size_t i = 0; i < ed::SegmentCount (tiny); ++i) {
        size_t from = 0, to = 0;
        ed::SegmentEnds (tiny, i, from, to);
        EXPECT_GE (ed::DistanceXY (tiny.vertices[from].p, tiny.vertices[to].p), ed::kMinSegment);
    }
    Valid (doc);
}
