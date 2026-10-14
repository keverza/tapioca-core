#include "Edit/EditDocument.hpp"
#include <gtest/gtest.h>

namespace ed = geomsrv::edit;

TEST (EditDocument, PolylineGetsStableUniqueIds)
{
    ed::Document doc;
    const ed::Id open = ed::AddPolyline (doc, { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 3, 0 } }, false);
    const ed::Id ring = ed::AddPolyline (doc, { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } }, true);
    std::string error;
    ASSERT_TRUE (ed::Validate (doc, error)) << error;
    EXPECT_EQ (ed::Find (doc, open)->segments.size (), 2u);
    EXPECT_EQ (ed::Find (doc, ring)->segments.size (), 3u);
    EXPECT_NE (open, ring);
    EXPECT_EQ (doc.revision, 2u);
}

TEST (EditDocument, ExtrusionIsCounterClockwiseAndFlat)
{
    ed::Document doc;
    // Clockwise input at mixed heights: stored CCW at the first point's height.
    const ed::Id box = ed::AddExtrusion (doc, { { 0, 0, 2 }, { 0, 3, 5 }, { 4, 3, 2 }, { 4, 0, 2 } }, 3.0);
    const ed::Entity& e = *ed::Find (doc, box);
    EXPECT_GT (ed::SignedAreaXY (e.vertices), 0.0);
    for (const auto& v : e.vertices)
        EXPECT_DOUBLE_EQ (v.p.z, 2.0);
    EXPECT_EQ (ed::FaceCount (e), 6u);
    std::string error;
    ASSERT_TRUE (ed::Validate (doc, error)) << error;
}

TEST (EditDocument, FaceNormalsPointOutward)
{
    ed::Document doc;
    const ed::Id box = ed::AddExtrusion (doc, { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 3, 0 }, { 0, 3, 0 } }, 3.0);
    const ed::Entity& e = *ed::Find (doc, box);
    ed::Vec3 n;
    ASSERT_TRUE (ed::FaceNormal (e, 1, n));
    EXPECT_DOUBLE_EQ (n.z, 1.0);
    ASSERT_TRUE (ed::FaceNormal (e, 2, n)); // side over (0,0)-(4,0): outside is -Y
    EXPECT_NEAR (n.y, -1.0, 1e-12);
    ed::Vec3 centre;
    ASSERT_TRUE (ed::PointOf (doc, { box, ed::Part::Face, 1 }, centre));
    EXPECT_NEAR (centre.x, 2.0, 1e-12);
    EXPECT_NEAR (centre.z, 3.0, 1e-12);
}

TEST (EditDocument, ValidateRefusesBrokenEntities)
{
    ed::Document doc;
    const ed::Id line = ed::AddPolyline (doc, { { 0, 0, 0 }, { 1, 0, 0 } }, false);
    std::string error;
    ed::Document dup = doc;
    ed::Find (dup, line)->vertices[1].p = { 0, 0, 0 };
    EXPECT_FALSE (ed::Validate (dup, error));
    EXPECT_NE (error.find ("zero length"), std::string::npos);

    ed::Document shared = doc;
    ed::Find (shared, line)->vertices[1].id = ed::Find (shared, line)->vertices[0].id;
    EXPECT_FALSE (ed::Validate (shared, error));

    ed::Document stale = doc;
    ed::Find (stale, line)->segments.clear ();
    EXPECT_FALSE (ed::Validate (stale, error));
}

TEST (EditDocument, NormalizeKeepsExistingSegments)
{
    ed::Document doc;
    const ed::Id line = ed::AddPolyline (doc, { { 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 } }, false);
    ed::Entity& e = *ed::Find (doc, line);
    e.segments[0].width = 1.5;
    const ed::Id kept = e.segments[0].id;
    e.vertices.push_back (ed::Vertex { ed::kNoId, { 3, 0, 0 } });
    ed::Normalize (doc, e);
    EXPECT_EQ (e.segments.size (), 3u);
    EXPECT_EQ (e.segments[0].id, kept);
    EXPECT_DOUBLE_EQ (e.segments[0].width, 1.5);
    std::string error;
    EXPECT_TRUE (ed::Validate (doc, error)) << error;
}

TEST (EditDocument, RefsResolveOnlyWhileTheirTargetExists)
{
    ed::Document doc;
    const ed::Id line = ed::AddPolyline (doc, { { 0, 0, 0 }, { 2, 0, 0 } }, false);
    const ed::Entity& e = *ed::Find (doc, line);
    const ed::Ref seg { line, ed::Part::Segment, e.segments[0].id };
    ed::Vec3 mid;
    ASSERT_TRUE (ed::PointOf (doc, seg, mid));
    EXPECT_DOUBLE_EQ (mid.x, 1.0);
    EXPECT_FALSE (ed::Exists (doc, { line, ed::Part::Vertex, 9999 }));
    EXPECT_FALSE (ed::Exists (doc, { line, ed::Part::Face, 0 })); // polylines have no faces
}
