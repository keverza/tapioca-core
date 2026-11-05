#include "Edit/EditArrange.hpp"
#include "Edit/EditEllipse.hpp"
#include "Edit/EditLandscape.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditOrganize.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditPolyline.hpp"
#include "Edit/EditPrimitives.hpp"
#include "Edit/EditSession.hpp"
#include <gtest/gtest.h>
#include <limits>
#include <set>

namespace ed = geomsrv::edit;

namespace {
void Valid (const ed::Document& doc)
{
    std::string why;
    EXPECT_TRUE (ed::Validate (doc, why)) << why;
}

std::set<ed::Id> AllIds (const ed::Document& doc)
{
    std::set<ed::Id> ids;
    for (const auto& e : doc.entities) {
        ids.insert (e.id);
        for (const auto& v : e.vertices)
            ids.insert (v.id);
        for (const auto& s : e.segments)
            ids.insert (s.id);
    }
    for (const auto& p : doc.paths) {
        ids.insert (p.id);
        for (const auto& c : p.points)
            ids.insert (c.id);
        for (const auto& b : p.profile)
            ids.insert (b.id);
        for (const auto& s : p.sections)
            ids.insert (s.id);
    }
    return ids;
}
} // namespace

TEST (EditArrange, CopyTakesEachWholeObjectOnceWhateverPartWasPicked)
{
    ed::Document doc;
    const ed::Id a = ed::AddPolyline (doc, { { 0, 0, 0 }, { 2, 0, 0 }, { 2, 2, 0 } }, false);
    const ed::Id b = ed::AddPolyline (doc, { { 5, 0, 0 }, { 6, 0, 0 } }, false);
    const ed::Id p = ed::AddPath (doc, { { 0, 5, 0 }, { 10, 5, 0 } }, 1.5);
    const auto& first = *ed::Find (doc, a);
    const auto clip = ed::Copy (doc, { { a, ed::Part::Vertex, first.vertices[1].id },
                                       { a, ed::Part::Segment, first.segments[0].id },
                                       { p, ed::Part::PathPoint, ed::FindPath (doc, p)->points[0].id } });
    ASSERT_EQ (clip.entities.size (), 1u);
    EXPECT_EQ (clip.entities[0], first);
    ASSERT_EQ (clip.paths.size (), 1u);
    EXPECT_TRUE (ed::Copy (doc, { { b, ed::Part::LandscapeFace, 0 } }).Empty ());
    ed::Vec3 centre;
    ASSERT_TRUE (ed::Centre (clip, centre));
    EXPECT_DOUBLE_EQ (centre.x, 5.0);
    EXPECT_DOUBLE_EQ (centre.y, 2.5);
}

TEST (EditArrange, PasteAddsMovedCopiesWithFreshIdsAndTheirAttributes)
{
    ed::Document doc;
    const ed::Id a = ed::AddPolyline (doc, { { 0, 0, 1 }, { 2, 0, 1 }, { 2, 2, 1 } }, true, "platform");
    ed::Find (doc, a)->segments[1].width = 0.3;
    ed::Find (doc, a)->vertices[2].radius = 0.25;
    const auto before = AllIds (doc);
    const auto clip = ed::Copy (doc, { { a, ed::Part::Entity, 0 } });
    std::vector<ed::Ref> pasted;
    std::string error;
    ASSERT_TRUE (ed::Paste (doc, clip, { 10, 0, 0 }, pasted, error)) << error;
    ASSERT_TRUE (ed::Paste (doc, clip, { 20, 0, 0 }, pasted, error)) << error;
    ASSERT_EQ (doc.entities.size (), 3u);
    ASSERT_EQ (pasted.size (), 1u);
    EXPECT_EQ (pasted[0].entity, doc.entities[2].id);
    const auto& copy = doc.entities[1];
    EXPECT_EQ (copy.role, "platform");
    EXPECT_TRUE (copy.closed);
    EXPECT_DOUBLE_EQ (copy.segments[1].width, 0.3);
    EXPECT_DOUBLE_EQ (copy.vertices[2].radius, 0.25);
    EXPECT_DOUBLE_EQ (copy.vertices[0].p.x, 10.0);
    EXPECT_DOUBLE_EQ (copy.vertices[0].p.z, 1.0);
    // Every id is new, and none repeats across the two pastes.
    const auto after = AllIds (doc);
    EXPECT_EQ (after.size (), before.size () * 3);
    Valid (doc);
    EXPECT_FALSE (ed::Paste (doc, {}, {}, pasted, error));
}

TEST (EditArrange, PasteAndMirrorKeepEllipseDefinitionsAndFreshStableIds)
{
    ed::Document doc;
    ed::PrimitiveInput input;
    input.kind = ed::Primitive::Ellipse;
    input.first = { 2, 3, 1 };
    input.second = { 6, 3, 1 };
    input.uniform = true;
    std::string error;
    const ed::Id id = ed::AddPrimitive (doc, input, "circle", error);
    ASSERT_NE (id, ed::kNoId) << error;
    const auto original = *ed::Find (doc, id);
    std::vector<ed::Ref> pasted;
    ASSERT_TRUE (ed::Paste (doc, ed::Copy (doc, { { id, ed::Part::Entity, 0 } }), { 10, -4, 2 }, pasted, error))
        << error;
    ASSERT_EQ (pasted.size (), 1u);
    const auto* copy = ed::Find (doc, pasted.front ().entity);
    ASSERT_NE (copy, nullptr);
    ASSERT_TRUE (copy->ellipse.has_value ());
    EXPECT_EQ (copy->ellipse->mode, ed::EllipseMode::Circle);
    EXPECT_EQ (copy->ellipse->center, (ed::Vec3 { 12, -1, 3 }));
    EXPECT_NE (copy->id, original.id);
    for (size_t i = 0; i < copy->vertices.size (); ++i)
        EXPECT_NE (copy->vertices[i].id, original.vertices[i].id);
    EXPECT_TRUE (ed::Validate (doc, error)) << error;

    const std::vector<ed::Id> ids = { original.vertices[0].id, original.vertices[1].id, original.vertices[2].id,
                                      original.vertices[3].id };
    ASSERT_TRUE (ed::Mirror (doc, { { id, ed::Part::Entity, 0 } }, { 0, 0, 0 }, { 0, 1, 0 }, error)) << error;
    const auto* mirrored = ed::Find (doc, id);
    ASSERT_NE (mirrored, nullptr);
    ASSERT_TRUE (mirrored->ellipse.has_value ());
    EXPECT_EQ (mirrored->ellipse->mode, ed::EllipseMode::Circle);
    EXPECT_EQ (mirrored->ellipse->center, (ed::Vec3 { -2, 3, 1 }));
    for (const ed::Id vertex : ids)
        EXPECT_GE (ed::VertexIndex (*mirrored, vertex), 0);
    EXPECT_TRUE (ed::ValidateEllipse (*mirrored, error)) << error;
    EXPECT_TRUE (ed::Validate (doc, error)) << error;
}

TEST (EditArrange, APastedPathKeepsItsOwnReferencesAndDropsOnesToPathsLeftBehind)
{
    ed::Document doc;
    const ed::Id p = ed::AddPath (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 } }, 1.5);
    const ed::Id q = ed::AddPath (doc, { { 20, 0, 0 }, { 30, 0, 0 } }, 1.5);
    ed::Path& path = *ed::FindPath (doc, p);
    const ed::Id corner = path.points[1].id;
    path.profile.back ().anchor = path.points.back ().id;
    path.profile.back ().binding = "slab:1";
    path.profile.front ().crossing = q;
    path.kerbs.push_back ({ corner, ed::kNoId, ed::KerbSide::Left, ed::KerbSide::Left, 0.4 });
    path.kerbs.push_back ({ ed::kNoId, q, ed::KerbSide::Right, ed::KerbSide::Left, 0.6 });

    std::vector<ed::Ref> pasted;
    std::string error;
    ASSERT_TRUE (ed::Paste (doc, ed::Copy (doc, { { p, ed::Part::Entity, 0 } }), { 0, -20, 5 }, pasted, error))
        << error;
    const ed::Path& copy = doc.paths.back ();
    EXPECT_NE (copy.id, p);
    EXPECT_DOUBLE_EQ (copy.points[1].p.y, -20.0);
    EXPECT_DOUBLE_EQ (copy.points[1].p.z, doc.paths[0].points[1].p.z); // heights stay the profile's
    EXPECT_EQ (copy.profile.back ().anchor, copy.points.back ().id);
    EXPECT_TRUE (copy.profile.back ().binding.empty ());
    EXPECT_EQ (copy.profile.front ().crossing, ed::kNoId);
    ASSERT_EQ (copy.kerbs.size (), 1u); // the junction with q stays behind
    EXPECT_EQ (copy.kerbs[0].point, copy.points[1].id);

    // Pasted together, the two keep their junction with each other.
    ASSERT_TRUE (ed::Paste (doc, ed::Copy (doc, { { p, ed::Part::Entity, 0 }, { q, ed::Part::Entity, 0 } }),
                            { 0, 40, 0 }, pasted, error))
        << error;
    ASSERT_EQ (pasted.size (), 2u);
    const ed::Path& p2 = *ed::FindPath (doc, pasted[0].entity);
    EXPECT_EQ (p2.profile.front ().crossing, pasted[1].entity);
    ASSERT_EQ (p2.kerbs.size (), 2u);
    EXPECT_EQ (p2.kerbs[1].other, pasted[1].entity);
}

TEST (EditArrange, PasteInPlaceKeepsCoordinatesAndDropsLandscapeOwnership)
{
    ed::Document doc;
    std::string error;
    ASSERT_TRUE (tapioca::geometry::StartLandscape (doc, { 1 }, error)) << error;
    const ed::Id id = ed::AddPolyline (doc, { { 2, 3, 1 }, { 5, 3, 1 }, { 5, 7, 1 } }, false, "guide");
    ASSERT_TRUE (tapioca::geometry::SetLandscapeSourceRole (doc, id, tapioca::geometry::SourceRole::Guide, error))
        << error;
    const ed::Entity original = *ed::Find (doc, id);
    std::vector<ed::Ref> pasted;
    ASSERT_TRUE (ed::Paste (doc, ed::Copy (doc, { { id, ed::Part::Entity, 0 } }), {}, pasted, error)) << error;
    ASSERT_EQ (pasted.size (), 1u);
    const ed::Entity& copy = *ed::Find (doc, pasted.front ().entity);
    ASSERT_EQ (copy.vertices.size (), original.vertices.size ());
    EXPECT_NE (copy.id, original.id);
    for (size_t i = 0; i < copy.vertices.size (); ++i) {
        EXPECT_EQ (copy.vertices[i].p, original.vertices[i].p);
        EXPECT_EQ (ed::Find (doc, id)->vertices[i].p, original.vertices[i].p);
        EXPECT_NE (copy.vertices[i].id, original.vertices[i].id);
    }
    ASSERT_EQ (doc.landscape->sources.size (), 1u);
    EXPECT_EQ (doc.landscape->sources.front ().entityId, original.id);
    EXPECT_NE (doc.landscape->sources.front ().entityId, copy.id);
    Valid (doc);

    std::string text;
    ASSERT_TRUE (tapioca::geometry::WriteLandscape (doc, tapioca::geometry::LandscapeStorage::Virtual, text, error))
        << error;
    ed::Document loaded;
    ASSERT_TRUE (tapioca::geometry::ReadLandscape (text, loaded, error)) << error;
    const ed::Entity& restored = *ed::Find (loaded, copy.id);
    ASSERT_EQ (restored.vertices.size (), original.vertices.size ());
    for (size_t i = 0; i < restored.vertices.size (); ++i)
        EXPECT_EQ (restored.vertices[i].p, original.vertices[i].p);
    ASSERT_EQ (loaded.landscape->sources.size (), 1u);
    EXPECT_EQ (loaded.landscape->sources.front ().entityId, original.id);
    Valid (loaded);
}

TEST (EditArrange, PasteThroughTheSessionIsOneUndoStep)
{
    ed::Document doc;
    const ed::Id a = ed::AddPolyline (doc, ed::Rectangle ({ 0, 0, 0 }, { 2, 2, 0 }), true);
    ed::Session s (doc);
    const auto clip = ed::Copy (s.Doc (), { { a, ed::Part::Entity, 0 } });
    ASSERT_TRUE (s.Apply ([clip] (ed::Document& d, std::string& error) {
        std::vector<ed::Ref> pasted;
        return ed::Paste (d, clip, { 3, 0, 0 }, pasted, error);
    })) << s.Message ();
    EXPECT_EQ (s.Doc ().entities.size (), 2u);
    ASSERT_TRUE (s.Undo ());
    EXPECT_EQ (s.Doc ().entities.size (), 1u);
}

TEST (EditArrange, RectangularArrayCountsIncludeSourceAndUsesSignedRowMajorSpacing)
{
    ed::Document doc;
    const ed::Id source = ed::AddPolyline (doc, { { 0, 0, 2 }, { 2, 0, 2 }, { 2, 2, 2 } }, false, "guide");
    ed::Find (doc, source)->segments[0].width = 0.4;
    ed::Find (doc, source)->segments[0].tag = 17;
    const ed::Entity original = *ed::Find (doc, source);
    const std::vector<ed::Ref> picked { { source, ed::Part::Vertex, original.vertices[0].id },
                                        { source, ed::Part::Segment, original.segments[0].id } };
    std::vector<ed::Ref> placed;
    std::string error;
    ASSERT_TRUE (ed::RectangularArray (doc, picked, 3, 2, 4.0, -5.0, placed, error)) << error;
    ASSERT_EQ (placed.size (), 6u); // two picked parts still denote one source object
    std::set<ed::Id> objectIds;
    for (size_t i = 0; i < placed.size (); ++i) {
        const ed::Entity& item = *ed::Find (doc, placed[i].entity);
        objectIds.insert (item.id);
        EXPECT_EQ (item.role, "guide");
        EXPECT_EQ (item.vertices[0].p, (ed::Vec3 { 4.0 * (i % 3), -5.0 * (i / 3), 2.0 })) << i;
        EXPECT_EQ (item.segments[0].width, 0.4);
        EXPECT_EQ (item.segments[0].tag, 17u);
        if (i == 0) {
            EXPECT_EQ (item.id, source);
            EXPECT_EQ (item, original);
        }
        else {
            EXPECT_NE (item.id, source);
            for (size_t v = 0; v < item.vertices.size (); ++v)
                EXPECT_NE (item.vertices[v].id, original.vertices[v].id);
        }
    }
    EXPECT_EQ (objectIds.size (), placed.size ());
    Valid (doc);
}

TEST (EditArrange, RectangularArrayKeepsAnalyticEllipseDefinitions)
{
    ed::Document doc;
    ed::PrimitiveInput input;
    input.kind = ed::Primitive::Ellipse;
    input.first = { 2, 3, 1 };
    input.second = { 6, 3, 1 };
    input.uniform = true;
    std::string error;
    const ed::Id source = ed::AddPrimitive (doc, input, "circle", error);
    ASSERT_NE (source, ed::kNoId) << error;
    const ed::Entity original = *ed::Find (doc, source);
    std::vector<ed::Ref> placed;
    ASSERT_TRUE (ed::RectangularArray (doc, { { source, ed::Part::Entity, 0 } }, 2, 2, 10.0, -5.0, placed, error))
        << error;
    ASSERT_EQ (placed.size (), 4u);
    for (size_t i = 0; i < placed.size (); ++i) {
        const ed::Entity& item = *ed::Find (doc, placed[i].entity);
        ASSERT_TRUE (item.ellipse.has_value ());
        EXPECT_EQ (item.ellipse->mode, ed::EllipseMode::Circle);
        EXPECT_EQ (item.ellipse->center, (ed::Vec3 { 2.0 + 10.0 * (i % 2), 3.0 - 5.0 * (i / 2), 1.0 }));
        EXPECT_EQ (item.ellipse->axisU, original.ellipse->axisU);
        EXPECT_EQ (item.ellipse->axisV, original.ellipse->axisV);
    }
    EXPECT_EQ (*ed::Find (doc, source), original);
    Valid (doc);
}

TEST (EditArrange, RectangularArrayCopiesPathsAndCompleteGroupsAsCells)
{
    ed::Document doc;
    const ed::Id entity = ed::AddPolyline (doc, { { 0, 0, 1 }, { 2, 0, 1 } }, false, "kerb");
    const ed::Id path = ed::AddPath (doc, { { 0, 5, 2 }, { 10, 5, 3 } }, 1.5, "walk");
    const ed::Id layer = ed::AddLayer (doc, "Planting");
    std::string error;
    ASSERT_TRUE (
        ed::MoveToLayer (doc, { { entity, ed::Part::Entity, 0 }, { path, ed::Part::Entity, 0 } }, layer, error))
        << error;
    const ed::Id group =
        ed::GroupObjects (doc, { { entity, ed::Part::Entity, 0 }, { path, ed::Part::Entity, 0 } }, "pair", error);
    ASSERT_NE (group, ed::kNoId) << error;
    ed::Path& sourcePath = *ed::FindPath (doc, path);
    sourcePath.profile.back ().anchor = sourcePath.points.back ().id;
    sourcePath.kerbs.push_back ({ sourcePath.points[0].id, ed::kNoId, ed::KerbSide::Left, ed::KerbSide::Left, 0.4 });
    const ed::Path originalPath = sourcePath;

    std::vector<ed::Ref> placed;
    ASSERT_TRUE (ed::RectangularArray (doc, { { entity, ed::Part::Entity, 0 }, { path, ed::Part::Entity, 0 } }, 2, 1,
                                       20.0, 0.0, placed, error))
        << error;
    ASSERT_EQ (placed.size (), 4u);
    EXPECT_EQ (placed[0].entity, path);
    EXPECT_EQ (placed[1].entity, entity);
    const ed::Path& copyPath = *ed::FindPath (doc, placed[2].entity);
    const ed::Entity& copyEntity = *ed::Find (doc, placed[3].entity);
    EXPECT_EQ (copyPath.points[0].p, (ed::Vec3 { 20, 5, originalPath.points[0].p.z }));
    EXPECT_EQ (copyPath.points[1].p, (ed::Vec3 { 30, 5, originalPath.points[1].p.z }));
    EXPECT_EQ (copyPath.profile.back ().anchor, copyPath.points.back ().id);
    ASSERT_EQ (copyPath.kerbs.size (), 1u);
    EXPECT_EQ (copyPath.kerbs[0].point, copyPath.points[0].id);
    EXPECT_EQ (copyPath.role, "walk");
    EXPECT_EQ (copyEntity.role, "kerb");
    ASSERT_NE (copyPath.member.group, ed::kNoId);
    EXPECT_EQ (copyPath.member.group, copyEntity.member.group);
    EXPECT_NE (copyPath.member.group, group);
    EXPECT_EQ (copyPath.member.layer, layer);
    EXPECT_EQ (copyEntity.member.layer, layer);
    EXPECT_EQ (doc.groups.size (), 2u);
    Valid (doc);
}

TEST (EditArrange, RectangularArrayCopiesAreNotLandscapeSources)
{
    ed::Document doc;
    std::string error;
    ASSERT_TRUE (tapioca::geometry::StartLandscape (doc, { 1 }, error)) << error;
    const ed::Id source = ed::AddPolyline (doc, { { 2, 3, 1 }, { 5, 3, 1 }, { 5, 7, 1 } }, false, "guide");
    ASSERT_TRUE (tapioca::geometry::SetLandscapeSourceRole (doc, source, tapioca::geometry::SourceRole::Guide, error))
        << error;
    std::vector<ed::Ref> placed;
    ASSERT_TRUE (ed::RectangularArray (doc, { { source, ed::Part::Entity, 0 } }, 2, 2, 10.0, 10.0, placed, error))
        << error;
    ASSERT_EQ (placed.size (), 4u);
    ASSERT_EQ (doc.landscape->sources.size (), 1u);
    EXPECT_EQ (doc.landscape->sources.front ().entityId, source);
    Valid (doc);
}

TEST (EditArrange, RectangularArrayOneCellIsAnAtomicNoOp)
{
    ed::Document doc;
    const ed::Id source = ed::AddPolyline (doc, { { 0, 0, 0 }, { 2, 0, 0 } }, false);
    const auto entities = doc.entities;
    const ed::Id nextId = doc.nextId;
    const uint64_t revision = doc.revision;
    std::vector<ed::Ref> placed;
    std::string error;
    ASSERT_TRUE (ed::RectangularArray (doc, { { source, ed::Part::Vertex, doc.entities[0].vertices[0].id } }, 1, 1, 0.0,
                                       0.0, placed, error))
        << error;
    ASSERT_EQ (placed.size (), 1u);
    EXPECT_EQ (placed[0], (ed::Ref { source, ed::Part::Entity, ed::kNoId }));
    EXPECT_EQ (doc.entities, entities);
    EXPECT_EQ (doc.nextId, nextId);
    EXPECT_EQ (doc.revision, revision);
}

TEST (EditArrange, RectangularArrayInvalidInputLeavesDocumentIdsAndOutputUntouched)
{
    ed::Document doc;
    const ed::Id source = ed::AddPolyline (doc, { { 0, 0, 0 }, { 2, 0, 0 } }, false);
    const std::vector<ed::Ref> refs { { source, ed::Part::Entity, 0 } };
    const auto entities = doc.entities;
    const ed::Id nextId = doc.nextId;
    const uint64_t revision = doc.revision;
    const std::vector<ed::Ref> sentinel { { 999, ed::Part::Entity, 0 } };
    const double inf = std::numeric_limits<double>::infinity ();
    const double nan = std::numeric_limits<double>::quiet_NaN ();
    const double max = std::numeric_limits<double>::max ();
    struct Case {
        size_t x, y;
        double spacingX, spacingY;
    };
    const std::vector<Case> cases { { 0, 1, 1, 1 },   { 1, 0, 1, 1 },   { 10001, 1, 1, 1 },
                                    { 2, 1, 0, 1 },   { 1, 2, 1, 0 },   { 2, 1, nan, 1 },
                                    { 1, 2, 1, inf }, { 3, 1, max, 1 }, { 2, 1, max, 1 } };
    for (const auto& test : cases) {
        std::vector<ed::Ref> placed = sentinel;
        std::string error;
        EXPECT_FALSE (ed::RectangularArray (doc, refs, test.x, test.y, test.spacingX, test.spacingY, placed, error));
        EXPECT_FALSE (error.empty ());
        EXPECT_EQ (placed, sentinel);
        EXPECT_EQ (doc.entities, entities);
        EXPECT_EQ (doc.nextId, nextId);
        EXPECT_EQ (doc.revision, revision);
    }
    std::vector<ed::Ref> placed = sentinel;
    std::string error;
    EXPECT_FALSE (ed::RectangularArray (doc, {}, 2, 2, 1, 1, placed, error));
    EXPECT_EQ (placed, sentinel);
    EXPECT_EQ (doc.entities, entities);
    EXPECT_EQ (doc.nextId, nextId);

    ed::Document large;
    const ed::Id far = ed::AddPolyline (large, { { max / 2.0, 0, 0 }, { max / 2.0, 1, 0 } }, false);
    const auto largeEntities = large.entities;
    const ed::Id largeNextId = large.nextId;
    placed = sentinel;
    EXPECT_FALSE (ed::RectangularArray (large, { { far, ed::Part::Entity, 0 } }, 2, 1, max, 0, placed, error));
    EXPECT_EQ (placed, sentinel);
    EXPECT_EQ (large.entities, largeEntities);
    EXPECT_EQ (large.nextId, largeNextId);
}

TEST (EditArrange, RectangularArrayBudgetsAndIdExhaustionRefuseBeforeCopying)
{
    ed::Document doc;
    std::vector<ed::Ref> refs;
    for (size_t i = 0; i < 11; ++i)
        refs.push_back ({ ed::AddPolyline (doc, { { double (i), 0, 0 }, { double (i) + 0.5, 0, 0 } }, false),
                          ed::Part::Entity, 0 });
    const auto entities = doc.entities;
    const ed::Id nextId = doc.nextId;
    const std::vector<ed::Ref> sentinel { { 999, ed::Part::Entity, 0 } };
    std::vector<ed::Ref> placed = sentinel;
    std::string error;
    EXPECT_FALSE (ed::RectangularArray (doc, refs, 10000, 1, 2.0, 0.0, placed, error));
    EXPECT_EQ (placed, sentinel);
    EXPECT_EQ (doc.entities, entities);
    EXPECT_EQ (doc.nextId, nextId);

    doc.nextId = std::numeric_limits<ed::Id>::max ();
    EXPECT_FALSE (ed::RectangularArray (doc, { refs.front () }, 2, 1, 2.0, 0.0, placed, error));
    EXPECT_EQ (placed, sentinel);
    EXPECT_EQ (doc.entities, entities);
    EXPECT_EQ (doc.nextId, std::numeric_limits<ed::Id>::max ());

    ed::Document dense;
    std::vector<ed::Vec3> points;
    for (size_t i = 0; i < 51; ++i)
        points.push_back ({ double (i), 0, 0 });
    const ed::Id source = ed::AddPolyline (dense, points, false);
    const auto denseEntities = dense.entities;
    const ed::Id denseNextId = dense.nextId;
    placed = sentinel;
    EXPECT_FALSE (ed::RectangularArray (dense, { { source, ed::Part::Entity, 0 } }, 10000, 1, 100.0, 0, placed, error));
    EXPECT_EQ (placed, sentinel);
    EXPECT_EQ (dense.entities, denseEntities);
    EXPECT_EQ (dense.nextId, denseNextId);
}

TEST (EditArrange, RectangularArrayExactIdCapacityIncludesGroupsAndPaths)
{
    ed::Document base;
    const ed::Id entity = ed::AddPolyline (base, { { 0, 0, 1 }, { 2, 0, 1 } }, false);
    const ed::Id path = ed::AddPath (base, { { 0, 5, 0 }, { 10, 5, 0 } }, 1.5);
    const std::vector<ed::Ref> refs { { path, ed::Part::Entity, 0 }, { entity, ed::Part::Entity, 0 } };
    std::string error;
    ASSERT_NE (ed::GroupObjects (base, refs, "pair", error), ed::kNoId) << error;
    const ed::Clipboard clip = ed::Copy (base, refs);
    ed::Document measured = base;
    std::vector<ed::Ref> pasted;
    ASSERT_TRUE (ed::Paste (measured, clip, { 100, 0, 0 }, pasted, error)) << error;
    const ed::Id required = measured.nextId - base.nextId;
    ASSERT_GT (required, 0u);

    ed::Document exact = base;
    exact.nextId = std::numeric_limits<ed::Id>::max () - required;
    std::vector<ed::Ref> placed;
    ASSERT_TRUE (ed::RectangularArray (exact, refs, 2, 1, 20.0, 0.0, placed, error)) << error;
    EXPECT_EQ (exact.nextId, std::numeric_limits<ed::Id>::max ());
    EXPECT_EQ (placed.size (), 4u);
    Valid (exact);

    ed::Document shortByOne = base;
    shortByOne.nextId = std::numeric_limits<ed::Id>::max () - required + 1;
    const auto entities = shortByOne.entities;
    const auto paths = shortByOne.paths;
    const auto groups = shortByOne.groups;
    const std::vector<ed::Ref> sentinel { { 999, ed::Part::Entity, 0 } };
    placed = sentinel;
    EXPECT_FALSE (ed::RectangularArray (shortByOne, refs, 2, 1, 20.0, 0.0, placed, error));
    EXPECT_EQ (placed, sentinel);
    EXPECT_EQ (shortByOne.entities, entities);
    EXPECT_EQ (shortByOne.paths, paths);
    EXPECT_EQ (shortByOne.groups, groups);
    EXPECT_EQ (shortByOne.nextId, std::numeric_limits<ed::Id>::max () - required + 1);
}

TEST (EditArrange, RectangularArrayThroughTheSessionIsOneUndoStep)
{
    ed::Document doc;
    const ed::Id source = ed::AddPolyline (doc, { { 0, 0, 1 }, { 2, 0, 1 } }, false);
    const std::vector<ed::Ref> refs { { source, ed::Part::Entity, 0 } };
    ed::Session session (doc);
    std::vector<ed::Ref> placed;
    ASSERT_TRUE (session.Apply (
        [&] (ed::Document& candidate, std::string& error) {
            return ed::RectangularArray (candidate, refs, 2, 2, 4.0, 5.0, placed, error);
        },
        false))
        << session.Message ();
    EXPECT_EQ (session.Doc ().entities.size (), 4u);
    EXPECT_EQ (placed.size (), 4u);
    EXPECT_EQ (session.UndoDepth (), 1u);
    ASSERT_TRUE (session.Undo ());
    ASSERT_EQ (session.Doc ().entities.size (), 1u);
    EXPECT_EQ (session.Doc ().entities[0].id, source);
    EXPECT_FALSE (session.CanUndo ());
    ASSERT_TRUE (session.Redo ());
    EXPECT_EQ (session.Doc ().entities.size (), 4u);
    Valid (session.Doc ());
}

TEST (EditArrange, MirrorKeepsARingsWindingAndALinesDirection)
{
    ed::Document doc;
    const ed::Id ring = ed::AddPolyline (doc, { { 0, 0, 1 }, { 2, 0, 1 }, { 2, 1, 1 } }, true);
    const ed::Id line = ed::AddPolyline (doc, { { 0, 3, 0 }, { 2, 4, 0 } }, false);
    const ed::Id box = ed::AddExtrusion (doc, ed::Rectangle ({ 0, 6, 0 }, { 1, 7, 0 }), 2.0);
    ed::Find (doc, line)->vertices[0].out = { 1, 0, 0 };
    const ed::Id corner = ed::Find (doc, ring)->vertices[1].id;
    std::string error;
    // Across the vertical plane x = 5.
    ASSERT_TRUE (ed::Mirror (doc,
                             { { ring, ed::Part::Entity, 0 },
                               { line, ed::Part::Vertex, ed::Find (doc, line)->vertices[1].id },
                               { box, ed::Part::Entity, 0 } },
                             { 5, 0, 0 }, { 0, 1, 0 }, error))
        << error;
    const ed::Entity& r = *ed::Find (doc, ring);
    EXPECT_GT (ed::SignedAreaXY (r.vertices), 0.0);
    const int i = ed::VertexIndex (r, corner);
    ASSERT_GE (i, 0);
    EXPECT_DOUBLE_EQ (r.vertices[size_t (i)].p.x, 8.0);
    EXPECT_DOUBLE_EQ (r.vertices[size_t (i)].p.z, 1.0);
    const ed::Entity& l = *ed::Find (doc, line);
    EXPECT_DOUBLE_EQ (l.vertices[0].p.x, 10.0); // still its start
    EXPECT_DOUBLE_EQ (l.vertices[0].out.x, -1.0);
    EXPECT_GT (ed::SignedAreaXY (ed::Find (doc, box)->vertices), 0.0);
    Valid (doc);
}

TEST (EditArrange, MirrorRefusesALockedVertexOffTheLine)
{
    ed::Document doc;
    const ed::Id e = ed::AddPolyline (doc, { { 0, 0, 0 }, { 2, 0, 0 } }, false);
    ed::Find (doc, e)->vertices[1].lockXY = true;
    std::string error;
    ed::Document copy = doc;
    EXPECT_FALSE (ed::Mirror (copy, { { e, ed::Part::Entity, 0 } }, { 0, 0, 0 }, { 0, 1, 0 }, error));
    // On the line it does not move, so it may stay locked.
    EXPECT_TRUE (ed::Mirror (doc, { { e, ed::Part::Entity, 0 } }, { 0, 0, 0 }, { 1, 0, 0 }, error)) << error;
    EXPECT_FALSE (ed::Mirror (doc, { { e, ed::Part::Entity, 0 } }, { 0, 0, 0 }, { 0, 0, 1 }, error));
}

TEST (EditArrange, AMirroredPathTradesItsSides)
{
    ed::Document doc;
    const ed::Id p = ed::AddPath (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 } }, 2.0);
    ed::Path& path = *ed::FindPath (doc, p);
    path.sections[0].widthLeft = 2.0;
    path.sections[0].endLeft = 2.0;
    path.sections[0].fall = ed::Fall::Left;
    path.kerbs.push_back ({ path.points[1].id, ed::kNoId, ed::KerbSide::Right, ed::KerbSide::Left, 0.5 });
    path.kerbs.push_back ({ path.points[0].id, ed::kNoId, ed::KerbSide::Start, ed::KerbSide::Left, 0.2 });
    std::string error;
    ASSERT_TRUE (ed::Mirror (doc, { { p, ed::Part::Entity, 0 } }, { 0, 0, 0 }, { 1, 0, 0 }, error)) << error;
    const ed::Path& m = *ed::FindPath (doc, p);
    EXPECT_DOUBLE_EQ (m.points[2].p.y, -10.0);
    EXPECT_DOUBLE_EQ (m.sections[0].widthLeft, 1.0);
    EXPECT_DOUBLE_EQ (m.sections[0].widthRight, 2.0);
    EXPECT_DOUBLE_EQ (m.sections[0].endRight, 2.0);
    EXPECT_EQ (m.sections[0].fall, ed::Fall::Right);
    EXPECT_EQ (m.kerbs[0].side, ed::KerbSide::Left);
    EXPECT_EQ (m.kerbs[1].side, ed::KerbSide::Start);
    Valid (doc);
}
