#include "Edit/EditPolyline.hpp"
#include "Edit/EditSession.hpp"
#include <gtest/gtest.h>
#include <set>

namespace ed = geomsrv::edit;

namespace {
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

const ed::Entity* Other (const ed::Document& doc, const std::set<ed::Id>& excluded)
{
    for (const auto& e : doc.entities)
        if (!excluded.count (e.id))
            return &e;
    return nullptr;
}

ed::Document OpenWithCutters (ed::Id& entity, ed::Id& first, ed::Id& second)
{
    ed::Document doc;
    entity = ed::AddPolyline (doc, { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 4, 1 }, { 10, 4, 2 } }, false, "edge");
    first = ed::AddPolyline (doc, { { 2, -1, 0 }, { 2, 5, 0 } }, false);
    second = ed::AddPolyline (doc, { { 8, -1, 0 }, { 8, 5, 0 } }, false);
    return doc;
}

ed::Document RingWithCutters (ed::Id& ring, ed::Id& first, ed::Id& second, bool atVertices = false)
{
    ed::Document doc;
    ring = ed::AddPolyline (doc, ed::Rectangle ({ 0, 0, 0 }, { 10, 10, 0 }), true, "edge");
    const double a = atVertices ? 0.0 : 2.0, b = atVertices ? 10.0 : 8.0;
    first = ed::AddPolyline (doc, { { a, -1, 0 }, { a, 11, 0 } }, false);
    second = ed::AddPolyline (doc, { { b, -1, 0 }, { b, 11, 0 } }, false);
    return doc;
}
} // namespace

TEST (EditPolylineTrim, OpenMiddleSpanPreviewsAndCommitsTheSameGeometryWithOneUndo)
{
    ed::Id source = 0, first = 0, second = 0;
    ed::Session session (OpenWithCutters (source, first, second));
    const auto original = E (session.Doc (), source);
    const ed::Id hovered = original.segments[2].id;
    ed::TrimSpan span;
    std::string error;
    ASSERT_TRUE (ed::FindTrimSpan (session.Doc (), source, hovered, { 7, 4, 0 }, span, error)) << error;
    ASSERT_EQ (span.removed.size (), 4u);
    Near (span.removed[0], 2, 0);
    Near (span.removed[1], 4, 0);
    Near (span.removed[2], 4, 4, 1);
    Near (span.removed[3], 8, 4, 1.6666666666666667);

    ASSERT_TRUE (session.Apply ([&] (ed::Document& doc, std::string& why) {
        return ed::TrimBetweenCrossings (doc, source, hovered, { 7, 4, 0 }, why);
    })) << session.Message ();
    EXPECT_EQ (session.UndoDepth (), 1u);
    ASSERT_EQ (session.Doc ().entities.size (), 4u);
    const auto& prefix = E (session.Doc (), source);
    const auto* suffix = Other (session.Doc (), { source, first, second });
    ASSERT_NE (suffix, nullptr);
    ASSERT_EQ (prefix.vertices.size (), 2u);
    ASSERT_EQ (suffix->vertices.size (), 2u);
    Near (prefix.vertices[0].p, 0, 0);
    Near (prefix.vertices[1].p, 2, 0);
    Near (suffix->vertices[0].p, 8, 4, 1.6666666666666667);
    Near (suffix->vertices[1].p, 10, 4, 2);
    EXPECT_EQ (prefix.vertices.front ().id, original.vertices.front ().id);
    EXPECT_EQ (suffix->vertices.back ().id, original.vertices.back ().id);
    EXPECT_EQ (prefix.segments.front ().id, original.segments.front ().id);
    EXPECT_NE (suffix->segments.back ().id, original.segments.back ().id); // the surviving second half is new
    EXPECT_EQ (prefix.role, "edge");
    EXPECT_EQ (suffix->role, "edge");
    Valid (session.Doc ());

    ASSERT_TRUE (session.Undo ());
    EXPECT_EQ (session.Doc ().entities.size (), 3u);
    EXPECT_EQ (E (session.Doc (), source), original);
    Valid (session.Doc ());
}

TEST (EditPolylineTrim, OpenEndSpansNeedOnlyOneCrossingAndKeepTheSourceId)
{
    for (const bool atStart : { true, false }) {
        ed::Document doc;
        const ed::Id source = ed::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 } }, false);
        const ed::Id cutter = ed::AddPolyline (doc, { { 2, -1, 0 }, { 2, 1, 0 } }, false);
        const auto original = E (doc, source);
        const ed::Id hovered = original.segments.front ().id;
        std::string error;
        ASSERT_TRUE (ed::TrimBetweenCrossings (doc, source, hovered, { atStart ? 1.0 : 9.0, 0, 0 }, error)) << error;
        ASSERT_EQ (doc.entities.size (), 2u);
        ASSERT_NE (ed::Find (doc, source), nullptr);
        ASSERT_NE (ed::Find (doc, cutter), nullptr);
        const auto& kept = E (doc, source);
        ASSERT_EQ (kept.vertices.size (), 2u);
        Near (kept.vertices[0].p, atStart ? 2 : 0, 0);
        Near (kept.vertices[1].p, atStart ? 10 : 2, 0);
        EXPECT_EQ (kept.vertices[atStart ? 1 : 0].id, original.vertices[atStart ? 1 : 0].id);
        if (atStart)
            EXPECT_NE (kept.segments.front ().id, original.segments.front ().id);
        else
            EXPECT_EQ (kept.segments.front ().id, original.segments.front ().id);
        Valid (doc);
    }
}

TEST (EditPolylineTrim, ClosedRingChoosesMiddleAndWrapSpansDeterministically)
{
    for (const bool wrap : { false, true }) {
        ed::Id ring = 0, first = 0, second = 0;
        ed::Document doc = RingWithCutters (ring, first, second);
        const auto original = E (doc, ring);
        const ed::Id hovered = original.segments.front ().id;
        ed::TrimSpan span;
        std::string error;
        ASSERT_TRUE (ed::FindTrimSpan (doc, ring, hovered, { wrap ? 1.0 : 5.0, 0, 0 }, span, error)) << error;
        ASSERT_EQ (span.removed.size (), wrap ? 4u : 2u);
        if (wrap) {
            Near (span.removed.front (), 2, 10);
            Near (span.removed[1], 0, 10);
            Near (span.removed[2], 0, 0);
            Near (span.removed.back (), 2, 0);
        }
        else {
            Near (span.removed.front (), 2, 0);
            Near (span.removed.back (), 8, 0);
        }

        ASSERT_TRUE (ed::TrimBetweenCrossings (doc, ring, hovered, { wrap ? 1.0 : 5.0, 0, 0 }, error)) << error;
        ASSERT_EQ (doc.entities.size (), 3u);
        const auto& kept = E (doc, ring);
        EXPECT_FALSE (kept.closed);
        EXPECT_EQ (kept.vertices.size (), wrap ? 4u : 6u);
        for (size_t i = 0; i < original.vertices.size (); ++i) {
            const bool removed = wrap && (i == 0 || i == 3);
            EXPECT_EQ (ed::VertexIndex (kept, original.vertices[i].id) >= 0, !removed) << i;
        }
        Valid (doc);
    }
}

TEST (EditPolylineTrim, RingVertexCrossingsAreDeduplicated)
{
    ed::Id ring = 0, first = 0, second = 0;
    ed::Document doc = RingWithCutters (ring, first, second, true);
    const ed::Id hovered = E (doc, ring).segments[1].id;
    ed::TrimSpan span;
    std::string error;
    ASSERT_TRUE (ed::FindTrimSpan (doc, ring, hovered, { 10, 5, 0 }, span, error)) << error;
    ASSERT_EQ (span.removed.size (), 2u);
    Near (span.removed.front (), 10, 0);
    Near (span.removed.back (), 10, 10);
    ASSERT_TRUE (ed::TrimBetweenCrossings (doc, ring, hovered, { 10, 5, 0 }, error)) << error;
    EXPECT_EQ (E (doc, ring).vertices.size (), 4u);
    Valid (doc);
}

TEST (EditPolylineTrim, ClosedWrapCutsOnOneSegmentInsertInOriginalTraversalOrder)
{
    ed::Document doc;
    const ed::Id ring = ed::AddPolyline (doc, ed::Rectangle ({ 0, 0, 0 }, { 10, 10, 0 }), true);
    ed::AddPolyline (doc, { { 2, -1, 0 }, { 2, 1, 0 } }, false);
    ed::AddPolyline (doc, { { 8, -1, 0 }, { 8, 1, 0 } }, false);
    const ed::Id top = E (doc, ring).segments[2].id;
    ed::TrimSpan span;
    std::string error;
    ASSERT_TRUE (ed::FindTrimSpan (doc, ring, top, { 5, 10, 0 }, span, error)) << error;
    ASSERT_EQ (span.removed.size (), 6u);
    ASSERT_TRUE (ed::TrimBetweenCrossings (doc, ring, top, { 5, 10, 0 }, error)) << error;
    const auto& kept = E (doc, ring);
    ASSERT_EQ (kept.vertices.size (), 2u);
    Near (kept.vertices.front ().p, 2, 0);
    Near (kept.vertices.back ().p, 8, 0);
    Valid (doc);
}

TEST (EditPolylineTrim, OpenLockedEndpointsAndNearVertexCutsRefuseWithoutMutation)
{
    for (const bool atStart : { true, false })
        for (const bool lockZ : { false, true }) {
            ed::Document doc;
            const ed::Id source = ed::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 } }, false);
            ed::AddPolyline (doc, { { 2, -1, 0 }, { 2, 1, 0 } }, false);
            auto& endpoint = E (doc, source).vertices[atStart ? 0 : 1];
            if (lockZ)
                endpoint.lockZ = true;
            else
                endpoint.lockXY = true;
            const auto before = doc.entities;
            std::string error;
            ed::TrimSpan span;
            EXPECT_FALSE (ed::FindTrimSpan (doc, source, E (doc, source).segments.front ().id,
                                            { atStart ? 1.0 : 9.0, 0, 0 }, span, error));
            EXPECT_FALSE (ed::TrimBetweenCrossings (doc, source, E (doc, source).segments.front ().id,
                                                    { atStart ? 1.0 : 9.0, 0, 0 }, error));
            EXPECT_EQ (doc.entities, before);
        }

    for (const double crossing : { 5.0e-7, 5.0 + 5.0e-7 }) {
        ed::Document doc;
        const ed::Id source = ed::AddPolyline (doc, { { 0, 0, 0 }, { 5, 0, 0 }, { 10, 0, 0 } }, false);
        ed::AddPolyline (doc, { { crossing, -1, 0 }, { crossing, 1, 0 } }, false);
        const auto before = doc.entities;
        std::string error;
        ed::TrimSpan span;
        EXPECT_FALSE (ed::FindTrimSpan (doc, source, E (doc, source).segments.back ().id, { 9, 0, 0 }, span, error));
        EXPECT_FALSE (ed::TrimBetweenCrossings (doc, source, E (doc, source).segments.back ().id, { 9, 0, 0 }, error));
        EXPECT_EQ (doc.entities, before);
        Valid (doc);
    }
}

TEST (EditPolylineTrim, VerticalLegsRefuseAndLargeCoordinatesKeepCutIdentity)
{
    ed::Document doc;
    const ed::Id vertical = ed::AddPolyline (doc, { { 0, 0, 0 }, { 5, 0, 0 }, { 5, 0, 10 }, { 10, 0, 10 } }, false);
    ed::AddPolyline (doc, { { 5, -1, 0 }, { 5, 1, 0 } }, false);
    const auto verticalBefore = doc.entities;
    std::string error;
    ed::TrimSpan span;
    EXPECT_FALSE (ed::FindTrimSpan (doc, vertical, E (doc, vertical).segments.back ().id, { 8, 0, 10 }, span, error));
    EXPECT_FALSE (ed::TrimBetweenCrossings (doc, vertical, E (doc, vertical).segments.back ().id, { 8, 0, 10 }, error));
    EXPECT_EQ (doc.entities, verticalBefore);

    const ed::Id large = ed::AddPolyline (doc, { { 0, 0, 0 }, { 1000000, 0, 0 }, { 2000000, 0, 0 } }, false);
    const ed::Id originalMiddle = E (doc, large).vertices[1].id;
    ed::AddPolyline (doc, { { 1000000.0005, -1, 0 }, { 1000000.0005, 1, 0 } }, false);
    ASSERT_TRUE (ed::FindTrimSpan (doc, large, E (doc, large).segments.back ().id, { 1500000, 0, 0 }, span, error))
        << error;
    ASSERT_TRUE (ed::TrimBetweenCrossings (doc, large, E (doc, large).segments.back ().id, { 1500000, 0, 0 }, error))
        << error;
    const auto& kept = E (doc, large);
    ASSERT_EQ (kept.vertices.size (), 3u);
    EXPECT_DOUBLE_EQ (span.removed.front ().x, kept.vertices.back ().p.x);
    EXPECT_NEAR (kept.vertices.back ().p.x, 1000000.0005, 1e-7);
    EXPECT_NE (kept.vertices.back ().id, originalMiddle);
    Valid (doc);
}

TEST (EditPolylineTrim, CoincidentVerticesAtDifferentStationsKeepTheirTraversalIdentity)
{
    ed::Document doc;
    const ed::Id source =
        ed::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 5, 0, 0 }, { 5, -5, 0 } }, false);
    ed::AddPolyline (doc, { { 5, -1, 0 }, { 5, 1, 0 } }, false);
    const ed::Id firstSegment = E (doc, source).segments.front ().id;
    ed::TrimSpan span;
    std::string error;
    ASSERT_TRUE (ed::FindTrimSpan (doc, source, firstSegment, { 2, 0, 0 }, span, error)) << error;
    ASSERT_EQ (span.removed.size (), 2u);
    Near (span.removed.front (), 0, 0);
    Near (span.removed.back (), 5, 0);
    ASSERT_TRUE (ed::TrimBetweenCrossings (doc, source, firstSegment, { 2, 0, 0 }, error)) << error;
    const auto& kept = E (doc, source);
    ASSERT_EQ (kept.vertices.size (), 5u);
    Near (kept.vertices[0].p, 5, 0);
    Near (kept.vertices[1].p, 10, 0);
    Near (kept.vertices[2].p, 10, 10);
    Near (kept.vertices[3].p, 5, 0);
    Near (kept.vertices[4].p, 5, -5);
    Valid (doc);
}

TEST (EditPolylineTrim, TooShortSurvivorBetweenCloseCrossingsRefusesDuringPreview)
{
    ed::Document doc;
    const ed::Id ring = ed::AddPolyline (doc, ed::Rectangle ({ 0, 0, 0 }, { 10, 10, 0 }), true);
    ed::AddPolyline (doc, { { 5, -1, 0 }, { 5, 1, 0 } }, false);
    ed::AddPolyline (doc, { { 5.0000005, -1, 0 }, { 5.0000005, 1, 0 } }, false);
    const ed::Id top = E (doc, ring).segments[2].id;
    const auto before = doc.entities;
    std::string error;
    ed::TrimSpan span;
    EXPECT_FALSE (ed::FindTrimSpan (doc, ring, top, { 5, 10, 0 }, span, error));
    EXPECT_FALSE (ed::TrimBetweenCrossings (doc, ring, top, { 5, 10, 0 }, error));
    EXPECT_EQ (doc.entities, before);
    Valid (doc);
}

TEST (EditPolylineTrim, DistantCurvesDoNotDisableStraightTrim)
{
    ed::Id source = 0, first = 0, second = 0;
    ed::Document doc = OpenWithCutters (source, first, second);
    const ed::Id distant = ed::AddPolyline (doc, { { 100, 100, 0 }, { 101, 100, 0 } }, false);
    E (doc, distant).vertices[0].out = { 0, 1, 0 };
    std::string error;
    ASSERT_TRUE (ed::TrimBetweenCrossings (doc, source, E (doc, source).segments[2].id, { 7, 4, 0 }, error)) << error;
    EXPECT_EQ (E (doc, source).vertices.size (), 2u);
    Valid (doc);
}

TEST (EditPolylineTrim, MissingCrossingsCurvesAndLockedRemovedVerticesRefuseWithoutMutation)
{
    ed::Document doc;
    const ed::Id source = ed::AddPolyline (doc, { { 0, 0, 0 }, { 4, 0, 0 } }, false);
    ed::AddPolyline (doc, { { 8, -1, 0 }, { 8, 1, 0 } }, false);
    const ed::Id hovered = E (doc, source).segments.front ().id;
    const auto before = doc.entities;
    const ed::Id nextId = doc.nextId;
    std::string error;
    ed::TrimSpan span;
    EXPECT_FALSE (ed::FindTrimSpan (doc, source, hovered, { 2, 0, 0 }, span, error));
    EXPECT_FALSE (ed::TrimBetweenCrossings (doc, source, hovered, { 2, 0, 0 }, error));
    EXPECT_EQ (doc.entities, before);
    EXPECT_EQ (doc.nextId, nextId);

    ed::Id ring = 0, first = 0, second = 0;
    doc = RingWithCutters (ring, first, second);
    // Shorten one cutter so the ring has only one crossing.
    E (doc, first).vertices[0].p = { 2, -1, 0 };
    E (doc, first).vertices[1].p = { 2, 1, 0 };
    E (doc, second).vertices[0].p = { 80, -1, 0 };
    E (doc, second).vertices[1].p = { 80, 1, 0 };
    EXPECT_FALSE (ed::TrimBetweenCrossings (doc, ring, E (doc, ring).segments.front ().id, { 5, 0, 0 }, error));

    ed::Id curvedSource = 0;
    doc = OpenWithCutters (curvedSource, first, second);
    E (doc, curvedSource).vertices[0].out = { 1, 0, 0 };
    EXPECT_FALSE (
        ed::TrimBetweenCrossings (doc, curvedSource, E (doc, curvedSource).segments[1].id, { 4, 2, 0 }, error));

    ed::Id locked = 0;
    doc = OpenWithCutters (locked, first, second);
    E (doc, locked).vertices[1].lockZ = true;
    const auto lockedBefore = doc.entities;
    EXPECT_FALSE (ed::FindTrimSpan (doc, locked, E (doc, locked).segments[1].id, { 4, 2, 0 }, span, error));
    EXPECT_FALSE (ed::TrimBetweenCrossings (doc, locked, E (doc, locked).segments[1].id, { 4, 2, 0 }, error));
    EXPECT_EQ (doc.entities, lockedBefore);

    ed::Id curvedCutterSource = 0;
    doc = OpenWithCutters (curvedCutterSource, first, second);
    E (doc, first).vertices[0].out = { 0, 1, 0 };
    const auto curvedCutterBefore = doc.entities;
    EXPECT_FALSE (ed::TrimBetweenCrossings (doc, curvedCutterSource, E (doc, curvedCutterSource).segments[1].id,
                                            { 4, 2, 0 }, error));
    EXPECT_EQ (doc.entities, curvedCutterBefore);
}
