#include "Edit/EditPolyline.hpp"
#include "Edit/EditPolylineDetail.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <set>

namespace ed = geomsrv::edit;

namespace {
constexpr double kPi = 3.14159265358979323846;

ed::Entity& E (ed::Document& doc, ed::Id id)
{
    return *ed::Find (doc, id);
}

const ed::Entity& E (const ed::Document& doc, ed::Id id)
{
    return *ed::Find (doc, id);
}

void Valid (const ed::Document& doc)
{
    std::string error;
    EXPECT_TRUE (ed::Validate (doc, error)) << error;
}

ed::OffsetOptions Bevel ()
{
    ed::OffsetOptions options;
    options.join = ed::OffsetJoinStyle::Bevel;
    return options;
}
} // namespace

TEST (EditPolylineOffsetCleanup, OpenCrossingKeepsTraversalAndSplitsAttributes)
{
    ed::Document doc;
    const ed::Id source = ed::AddPolyline (doc, { { 0, 0, 0 }, { 4, 4, 10 }, { 0, 4, 10 }, { 4, 0, 30 } }, false);
    for (size_t i = 0; i < 3; ++i) {
        E (doc, source).segments[i].tag = uint32_t (i + 1);
        E (doc, source).segments[i].width = double (i + 1);
    }
    const ed::Entity original = E (doc, source);
    ed::Id copy = 0;
    std::string error;
    ASSERT_TRUE (ed::OffsetEntity (doc, source, -0.5, Bevel (), error, &copy)) << error;
    const auto& made = E (doc, copy);
    ASSERT_EQ (made.segments.size (), 7u);
    const std::vector<uint32_t> expected { 1, 1, 1, 2, 2, 3, 3 };
    for (size_t i = 0; i < expected.size (); ++i) {
        EXPECT_EQ (made.segments[i].tag, expected[i]) << i;
        EXPECT_EQ (made.segments[i].width, double (expected[i])) << i;
    }
    bool repeated = false;
    for (size_t i = 0; i < made.vertices.size () && !repeated; ++i)
        for (size_t j = i + 1; j < made.vertices.size (); ++j)
            if (ed::DistanceXY (made.vertices[i].p, made.vertices[j].p) < 1e-9) {
                repeated = true;
                break;
            }
    EXPECT_TRUE (repeated) << "each crossing passage is explicit in the open traversal";
    EXPECT_EQ (E (doc, source), original);
    Valid (doc);
}

TEST (EditPolylineOffsetCleanup, OpenCrossingRestoresElevationPerPassage)
{
    ed::Document doc;
    const ed::Id source = ed::AddPolyline (doc, { { 0, 0, 0 }, { 4, 4, 10 }, { 0, 4, 10 }, { 4, 0, 30 } }, false);
    ed::Id copy = 0;
    std::string error;
    ASSERT_TRUE (ed::OffsetEntity (doc, source, 0.0, Bevel (), error, &copy)) << error;
    const auto& made = E (doc, copy);
    std::vector<double> crossingZ;
    for (size_t i = 0; i < made.vertices.size (); ++i)
        if (ed::DistanceXY (made.vertices[i].p, { 2, 2, 0 }) < 1e-9)
            crossingZ.push_back (made.vertices[i].p.z);
    ASSERT_EQ (crossingZ.size (), 2u);
    EXPECT_DOUBLE_EQ (crossingZ[0], 5.0);
    EXPECT_DOUBLE_EQ (crossingZ[1], 20.0);
    Valid (doc);
}

TEST (EditPolylineOffsetCleanup, PentagramBecomesOneSupportedOuterRing)
{
    ed::Document doc;
    std::vector<ed::Vec3> points;
    for (int i = 0; i < 5; ++i) {
        const double angle = i * 4.0 * kPi / 5.0;
        points.push_back ({ 5.0 * std::cos (angle), 5.0 * std::sin (angle), 0.0 });
    }
    const ed::Id source = ed::AddPolyline (doc, points, true);
    for (size_t i = 0; i < 5; ++i)
        E (doc, source).segments[i].tag = uint32_t (i + 1);
    const ed::Entity original = E (doc, source);
    ed::Id copy = 0;
    std::string error;
    ASSERT_TRUE (ed::OffsetEntity (doc, source, 0.0, Bevel (), error, &copy)) << error;
    const auto& made = E (doc, copy);
    EXPECT_TRUE (made.closed);
    EXPECT_EQ (made.vertices.size (), 10u);
    EXPECT_GT (std::abs (ed::SignedAreaXY (made.vertices)), 0.0);
    std::set<uint32_t> tags;
    for (const auto& segment : made.segments)
        tags.insert (segment.tag);
    EXPECT_EQ (tags, (std::set<uint32_t> { 1, 2, 3, 4, 5 }));
    for (size_t i = 0; i < made.vertices.size (); ++i)
        for (size_t j = i + 1; j < made.vertices.size (); ++j)
            EXPECT_GT (ed::DistanceXY (made.vertices[i].p, made.vertices[j].p), 1e-6)
                << i << " and " << j << " must not touch after cleanup";
    EXPECT_EQ (E (doc, source), original);
    Valid (doc);
}

TEST (EditPolylineOffsetCleanup, MultipleClosedLobesRefuseWithoutMutationOrIds)
{
    ed::Document doc;
    const ed::Id source = ed::AddPolyline (doc, { { 0, 0, 0 }, { 4, 4, 0 }, { 0, 4, 0 }, { 4, 0, 0 } }, true);
    const auto before = doc.entities;
    const ed::Id nextId = doc.nextId;
    const uint64_t revision = doc.revision;
    ed::Id created = 777;
    std::string error;
    EXPECT_FALSE (ed::OffsetEntity (doc, source, 0.0, Bevel (), error, &created));
    EXPECT_FALSE (error.empty ());
    EXPECT_EQ (created, 777u);
    EXPECT_EQ (doc.entities, before);
    EXPECT_EQ (doc.nextId, nextId);
    EXPECT_EQ (doc.revision, revision);
}

TEST (EditPolylineOffsetCleanup, ElevatedClosedCleanupKeepsElevation)
{
    ed::Document doc;
    std::vector<ed::Vec3> points;
    for (int i = 0; i < 5; ++i) {
        const double angle = i * 4.0 * kPi / 5.0;
        points.push_back ({ 5.0 * std::cos (angle), 5.0 * std::sin (angle), 5.0 });
    }
    const ed::Id source = ed::AddPolyline (doc, points, true);
    ed::Id copy = 0;
    std::string error;
    ASSERT_TRUE (ed::OffsetEntity (doc, source, 0.0, Bevel (), error, &copy)) << error;
    const auto& made = E (doc, copy);
    EXPECT_EQ (made.vertices.size (), 10u);
    for (const auto& vertex : made.vertices)
        EXPECT_DOUBLE_EQ (vertex.p.z, 5.0);
    Valid (doc);
}

TEST (EditPolylineOffsetCleanup, EndpointInsideNonincidentClosedEdgeRefuses)
{
    ed::Document doc;
    const ed::Id source =
        ed::AddPolyline (doc, { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 4, 0 }, { 2, 0, 0 }, { 0, 4, 0 } }, true);
    const auto before = doc.entities;
    const ed::Id nextId = doc.nextId;
    ed::Id created = 777;
    std::string error;
    EXPECT_FALSE (ed::OffsetEntity (doc, source, 0.0, Bevel (), error, &created));
    EXPECT_EQ (created, 777u);
    EXPECT_EQ (doc.entities, before);
    EXPECT_EQ (doc.nextId, nextId);
}

TEST (EditPolylineOffsetCleanup, LongEdgeNearEndpointContactRefuses)
{
    ed::Document doc;
    const ed::Id source =
        ed::AddPolyline (doc, { { 0, 0, 0 }, { 1000, 0, 0 }, { 1000, 10, 0 }, { 0.00005, 0, 0 }, { 0, 10, 0 } }, true);
    const auto before = doc.entities;
    const ed::Id nextId = doc.nextId;
    ed::Id created = 777;
    std::string error;
    EXPECT_FALSE (ed::OffsetEntity (doc, source, 0.0, Bevel (), error, &created));
    EXPECT_EQ (created, 777u);
    EXPECT_EQ (doc.entities, before);
    EXPECT_EQ (doc.nextId, nextId);
}

TEST (EditPolylineOffsetCleanup, OpenCollinearOverlapRefusesWithoutMutation)
{
    ed::Document doc;
    const ed::Id source =
        ed::AddPolyline (doc, { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 2, 0 }, { 1, 0, 0 }, { 3, 0, 0 } }, false);
    E (doc, source).segments[0].tag = 1;
    E (doc, source).segments[3].tag = 4;
    const auto before = doc.entities;
    const ed::Id nextId = doc.nextId;
    ed::Id created = 777;
    std::string error;
    EXPECT_FALSE (ed::OffsetEntity (doc, source, 0.0, Bevel (), error, &created));
    EXPECT_FALSE (error.empty ());
    EXPECT_EQ (created, 777u);
    EXPECT_EQ (doc.entities, before);
    EXPECT_EQ (doc.nextId, nextId);
}

TEST (EditPolylineOffsetCleanup, CollapsedEdgesMergeOnlyWithCompatibleAttributes)
{
    for (const bool compatible : { true, false }) {
        ed::Document doc;
        const ed::Id source =
            ed::AddPolyline (doc, { { 0, 0, 0 }, { 1, 0, 0 }, { 1.0000005, 0, 0 }, { 2, 0, 0 } }, false);
        E (doc, source).segments[0].tag = 9;
        E (doc, source).segments[1].tag = 1;
        E (doc, source).segments[2].tag = compatible ? 1 : 2;
        ed::Entity candidate = E (doc, source);
        const ed::Entity before = candidate;
        std::string error;
        const bool ok = ed::polyline_detail::CleanupOffset (candidate, error);
        if (!compatible) {
            EXPECT_FALSE (ok);
            EXPECT_EQ (candidate, before);
            continue;
        }
        ASSERT_TRUE (ok) << error;
        ASSERT_EQ (candidate.vertices.size (), 3u);
        EXPECT_EQ (candidate.segments[0].tag, 9u);
        EXPECT_EQ (candidate.segments[1].tag, 1u);
        ed::Document result;
        result.nextId = doc.nextId;
        result.entities.push_back (candidate);
        Valid (result);
    }
}
