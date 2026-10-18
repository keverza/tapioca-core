#include "Edit/EditWorkingPlane.hpp"
#include <gtest/gtest.h>
#include <limits>

namespace ed = geomsrv::edit;

TEST (EditWorkingPlane, FaceFramesKeepPickTimeProvenanceAndAuthoredAxes)
{
    ed::Document doc;
    const auto id = ed::AddExtrusion (doc, { { 0, 0, 100 }, { 4, 0, 100 }, { 4, 6, 100 }, { 0, 6, 100 } }, 3);
    std::string error;
    for (ed::Id face : { 0u, 1u, 2u }) {
        ed::WorkingPlane plane;
        ASSERT_TRUE (ed::PlaneFromFace (doc, { id, ed::Part::Face, face }, plane, error)) << error;
        EXPECT_TRUE (ed::ValidPlane (plane));
        EXPECT_FALSE (ed::IsWorldXY (plane));
        EXPECT_EQ (plane.source, ed::PlaneSource::Face);
        EXPECT_EQ (plane.sourceRef, (ed::Ref { id, ed::Part::Face, face }));
        EXPECT_EQ (plane.sourceRevision, doc.revision);
        if (face < 2)
            EXPECT_EQ (plane.origin, (ed::Vec3 { 2, 3, face == 0 ? 100.0 : 103.0 }));
        ed::Vec3 u, v;
        ed::PlaneAxes (ed::SnapPlane (plane), u, v);
        EXPECT_EQ (u, plane.u);
        EXPECT_EQ (v, plane.v);
        EXPECT_NEAR (ed::Distance (ed::ToLocal (plane, ed::ToWorld (plane, { 2, 4, 0 })), { 2, 4, 0 }), 0, 1e-12);
    }
}

TEST (EditWorkingPlane, StaleDegenerateAndNonplanarFacesLeaveThePlaneUntouched)
{
    ed::Document doc;
    const auto id = ed::AddExtrusion (doc, { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 6, 0 }, { 0, 6, 0 } }, 3);
    const auto line = ed::AddPolyline (doc, { { 0, 0, 0 }, { 1, 0, 0 } }, false);
    const auto original = ed::WorldXY (7);
    auto out = original;
    std::string error;
    for (const auto ref : { ed::Ref { id, ed::Part::Face, 999 }, ed::Ref { line, ed::Part::Face, 0 },
                            ed::Ref { id, ed::Part::Entity, 0 }, ed::Ref { 999, ed::Part::Face, 0 } }) {
        EXPECT_FALSE (ed::PlaneFromFace (doc, ref, out, error));
        EXPECT_TRUE (ed::SamePlane (out, original));
        EXPECT_FALSE (error.empty ());
    }
    doc.entities[0].vertices[0].p.z = 1;
    EXPECT_FALSE (ed::PlaneFromFace (doc, { id, ed::Part::Face, 1 }, out, error));
    EXPECT_TRUE (ed::SamePlane (out, original));
    for (auto& vertex : doc.entities[0].vertices)
        vertex.p = {};
    EXPECT_FALSE (ed::PlaneFromFace (doc, { id, ed::Part::Face, 1 }, out, error));
    EXPECT_TRUE (ed::SamePlane (out, original));
    doc.entities[0].vertices.clear ();
    EXPECT_FALSE (ed::PlaneFromFace (doc, { id, ed::Part::Face, 0 }, out, error));
    EXPECT_TRUE (ed::SamePlane (out, original));
}

TEST (EditWorkingPlane, SnapAdapterUsesAuthoredDirectionsInsteadOfRebuildingWorldAxes)
{
    auto plane = ed::WorldXY (7);
    plane.source = ed::PlaneSource::Explicit;
    plane.u = { 0.6, 0.8, 0 }, plane.v = { -0.8, 0.6, 0 };
    ed::SnapQuery q;
    q.plane = ed::SnapPlane (plane);
    q.cursor = plane.origin + plane.u * 4 + plane.v * 0.02;
    q.anchor = plane.origin;
    q.modes = ed::kSnapAxis;
    q.tolerance = 0.1;
    const auto snap = ed::Snap ({}, {}, q);
    ASSERT_TRUE (snap.snapped);
    EXPECT_EQ (snap.kind, ed::SnapKind::Axis);
    EXPECT_NEAR (ed::Distance (snap.point, plane.origin + plane.u * 4), 0, 1e-12);
    // Legacy queries still derive the world axes when no authored tangent was supplied.
    ed::Vec3 u, v;
    ed::PlaneAxes ({}, u, v);
    EXPECT_EQ (u, (ed::Vec3 { 1, 0, 0 }));
    EXPECT_EQ (v, (ed::Vec3 { 0, 1, 0 }));
}

TEST (EditWorkingPlane, WorldXYHasAnExplicitDatumAndSnapAdapter)
{
    const auto plane = ed::WorldXY (103.25);
    ASSERT_TRUE (ed::ValidPlane (plane));
    EXPECT_TRUE (ed::IsWorldXY (plane));
    const auto local = ed::ToLocal (plane, { 4, 6, 105.25 });
    EXPECT_EQ (local, (ed::Vec3 { 4, 6, 2 }));
    EXPECT_EQ (ed::ToWorld (plane, local), (ed::Vec3 { 4, 6, 105.25 }));
    EXPECT_EQ (ed::ProjectToPlane (ed::SnapPlane (plane), { 4, 6, -10 }), (ed::Vec3 { 4, 6, 103.25 }));
}

TEST (EditWorkingPlane, ObliqueFramesRoundTripWithoutBecomingWorldXY)
{
    auto p = ed::WorldXY (12);
    p.source = ed::PlaneSource::Explicit;
    p.u = { 0, 1, 0 }, p.v = { 0, 0, 1 }, p.normal = { 1, 0, 0 };
    ASSERT_TRUE (ed::ValidPlane (p));
    EXPECT_FALSE (ed::IsWorldXY (p));
    const ed::Vec3 world { 7, 8, 9 };
    EXPECT_NEAR (ed::Distance (ed::ToWorld (p, ed::ToLocal (p, world)), world), 0, 1e-12);
    ed::Vec3 hit;
    ASSERT_TRUE (ed::HitPlane (p, { 5, 8, 9 }, { -2, 0, 0 }, hit));
    EXPECT_EQ (hit, (ed::Vec3 { 0, 8, 9 }));
}

TEST (EditWorkingPlane, RayHitsRefuseEdgeOnBehindAndInvalidInputWithoutOutput)
{
    const auto p = ed::WorldXY (5);
    ed::Vec3 out { 1, 2, 3 };
    for (const auto direction : { ed::Vec3 { 1, 0, 0 }, ed::Vec3 { 0, 0, 1 }, ed::Vec3 {} }) {
        EXPECT_FALSE (ed::HitPlane (p, { 0, 0, 10 }, direction, out));
        EXPECT_EQ (out, (ed::Vec3 { 1, 2, 3 }));
    }
    ASSERT_TRUE (ed::HitPlane (p, { 4, 6, 10 }, { 0, 0, -2 }, out));
    EXPECT_EQ (out, (ed::Vec3 { 4, 6, 5 }));
}

TEST (EditWorkingPlane, InvalidOrLeftHandedFramesCannotReplaceAPlane)
{
    const auto original = ed::WorldXY (5);
    auto p = original;
    std::string error;
    for (int i = 0; i < 4; ++i) {
        auto bad = original;
        if (i == 0)
            bad.origin.z = std::numeric_limits<double>::quiet_NaN ();
        if (i == 1)
            bad.u = {};
        if (i == 2)
            bad.v = bad.u;
        if (i == 3)
            bad.normal = { 0, 0, -1 };
        EXPECT_FALSE (ed::ChangePlane (p, bad, false, error));
        EXPECT_TRUE (ed::SamePlane (p, original));
        EXPECT_FALSE (error.empty ());
    }
}

TEST (EditWorkingPlane, APlaneLockRefusesAutomaticButNotExplicitChanges)
{
    auto p = ed::WorldXY (5);
    p.locked = true;
    const auto original = p;
    auto next = ed::WorldXY (12);
    next.locked = true;
    std::string error;
    EXPECT_FALSE (ed::ChangePlane (p, next, true, error));
    EXPECT_TRUE (ed::SamePlane (p, original));
    EXPECT_TRUE (ed::ChangePlane (p, next, false, error));
    EXPECT_TRUE (error.empty ());
    EXPECT_DOUBLE_EQ (p.origin.z, 12);
}
