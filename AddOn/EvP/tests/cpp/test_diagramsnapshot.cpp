#include "ArchViz/DiagramSnapshot.hpp"
#include "Geometry/Primitives.hpp"
#include <gtest/gtest.h>
#include <algorithm>

namespace dg = geomsrv::archviz::diagram;
namespace ms = geomsrv::archviz::massingslices;
namespace meta = geomsrv::metadata;
namespace {
// Fixtures: massing slabs as the extractor hands them over, 10 x 10 m unless stated.
ms::Input Slab (const char* guid, const char* id, double bottom, double top, double x = 0, double width = 10)
{
    ms::Input input;
    input.slab.guid = guid;
    input.slab.bottom = bottom;
    input.slab.top = top;
    input.slab.outer.xy = { x, 0, x + width, 0, x + width, 10, x, 10 };
    if (*id) {
        meta::Property property;
        property.key = "massing.buildingId";
        property.value = meta::Value::Text (id);
        meta::SetProperty (input.metadata, property);
    }
    geomsrv::Mesh body;
    std::string error;
    EXPECT_TRUE (
        geomsrv::engine::MakeBox ({ x + width / 2, 5, (bottom + top) / 2 }, width, 10, top - bottom, body, error));
    input.facadeBody = std::make_shared<const geomsrv::Mesh> (std::move (body));
    return input;
}
geomsrv::archviz::ProjectStoreys Stories (const std::vector<double>& levels, const std::vector<int>& floorIds)
{
    geomsrv::archviz::ProjectStoreys s;
    for (size_t i = 0; i < levels.size (); ++i) {
        s.levels.push_back (levels[i]);
        s.names.push_back ("S" + std::to_string (i));
        s.indices.push_back (int (i));
        if (i < floorIds.size ())
            s.floorIds.push_back (floorIds[i]);
    }
    return s;
}
std::shared_ptr<const ms::Result> AtStories (const std::vector<ms::Input>& slabs,
                                             const geomsrv::archviz::ProjectStoreys& stories)
{
    auto result = std::make_shared<ms::Result> ();
    std::string error;
    EXPECT_TRUE (ms::BuildStories (slabs, stories, nullptr, *result, error)) << error;
    return result;
}
std::vector<const dg::Entity*> At (const dg::Scene& scene, dg::Level level)
{
    std::vector<const dg::Entity*> out;
    for (const auto& e : scene.entities)
        if (e.id.level == level)
            out.push_back (&e);
    return out;
}
const dg::StoreyBinding* ByFloor (const dg::StoreyBindings& b, const std::string& building, int floorId)
{
    auto found = b.byBuilding.find (building);
    if (found == b.byBuilding.end ())
        return nullptr;
    for (const auto& binding : found->second)
        if (binding.source.persistentId == dg::StoreyPersistentId (floorId))
            return &binding;
    return nullptr;
}
} // namespace

TEST (DiagramSnapshot, MissingSlicesBuildsNothing)
{
    const auto snapshot = dg::Capture (nullptr, {}, {});
    EXPECT_FALSE (snapshot.status.compatible);
    ASSERT_EQ (snapshot.status.failures.size (), 1u);
    EXPECT_EQ (snapshot.status.failures[0], dg::SnapshotIssue::MissingSlices);
    dg::StoreyBindings bindings;
    const auto built = dg::BuildScene (snapshot, bindings);
    EXPECT_FALSE (built.scene);
    EXPECT_FALSE (built.notes.empty ());
}

TEST (DiagramSnapshot, RegularBuildingOnProjectStories)
{
    const auto stories = Stories ({ 0, 3, 6 }, { 101, 102, 103 });
    const auto snapshot = dg::Capture (AtStories ({ Slab ("a", "Tower", 0, 9) }, stories), stories, {});
    ASSERT_TRUE (snapshot.status.compatible);
    EXPECT_EQ (snapshot.domain, dg::StoreyDomain::ProjectStories);
    EXPECT_TRUE (snapshot.status.warnings.empty ());
    dg::StoreyBindings bindings;
    const auto built = dg::BuildScene (snapshot, bindings);
    ASSERT_TRUE (built.scene);
    const auto buildings = At (*built.scene, dg::Level::Building);
    const auto storeys = At (*built.scene, dg::Level::Storey);
    ASSERT_EQ (buildings.size (), 1u);
    EXPECT_EQ (buildings[0]->id.key, "building:Tower");
    ASSERT_EQ (storeys.size (), 3u);
    EXPECT_EQ (built.shells.size (), 3u);
    EXPECT_EQ (built.representedGuids, (std::vector<std::string> { "a" }));
    for (int floorId : { 101, 102, 103 })
        EXPECT_TRUE (ByFloor (bindings, "building:Tower", floorId)) << floorId;
    for (const auto* s : storeys) {
        EXPECT_FALSE (s->planned);
        EXPECT_EQ (s->parent, buildings[0]->id);
        EXPECT_NEAR (s->boundsMax[2] - s->boundsMin[2], 3.0, 1e-6);
        EXPECT_NEAR (s->boundsMin[0], 0.0, 1e-6);
        EXPECT_NEAR (s->boundsMax[0], 10.0, 1e-6);
    }
    EXPECT_NEAR (buildings[0]->boundsMax[2], 9.0, 1e-6);

    // End to end: the shells explode and pick at their displaced pose.
    dg::LayoutRequest request;
    request.rules = dg::DefaultLayoutRules ();
    const auto plan = dg::Plan (*built.scene, request);
    const auto frame = dg::Evaluate (plan, {}, 1.0); // top storey: z 6..9 -> 12..15
    auto hit = dg::Pick (frame, plan, built.shells, { 5, -50, 13.5 }, { 0, 1, 0 });
    ASSERT_TRUE (hit);
    EXPECT_EQ (hit->id, bindings.byBuilding.at ("building:Tower")[2].id);
    EXPECT_NEAR (hit->point[1], 0.0, 1e-6);
}

TEST (DiagramSnapshot, SharedAuthoredIdIsOneBuildingMissingIdIsItsOwn)
{
    const auto stories = Stories ({ 0, 3 }, { 1, 2 });
    const auto snapshot = dg::Capture (
        AtStories ({ Slab ("a", "Tower", 0, 6), Slab ("b", "Tower", 0, 6, 10), Slab ("c", "", 0, 3, 40) }, stories),
        stories, {});
    ASSERT_TRUE (snapshot.status.compatible);
    dg::StoreyBindings bindings;
    const auto built = dg::BuildScene (snapshot, bindings);
    ASSERT_TRUE (built.scene);
    const auto buildings = At (*built.scene, dg::Level::Building);
    ASSERT_EQ (buildings.size (), 2u);
    EXPECT_EQ (buildings[0]->id.key, "building:Tower");
    EXPECT_EQ (buildings[1]->id.key, "slab:c");
    // Both Tower slabs make one storey per story, 20 m wide.
    EXPECT_EQ (bindings.byBuilding.at ("building:Tower").size (), 2u);
    for (const auto* s : At (*built.scene, dg::Level::Storey))
        if (s->parent == buildings[0]->id)
            EXPECT_NEAR (s->boundsMax[0] - s->boundsMin[0], 20.0, 1e-6);
    EXPECT_EQ (built.representedGuids, (std::vector<std::string> { "a", "b", "c" }));
}

TEST (DiagramSnapshot, MassingOwnFloorsWarnAndBindByPosition)
{
    auto result = std::make_shared<ms::Result> ();
    std::string error;
    ASSERT_TRUE (ms::Build ({ Slab ("a", "Tower", 0, 9) }, {}, nullptr, *result, error)) << error;
    ASSERT_FALSE (result->followsStories);
    const auto snapshot = dg::Capture (result, {}, {});
    ASSERT_TRUE (snapshot.status.compatible);
    EXPECT_EQ (snapshot.domain, dg::StoreyDomain::MassingFloors);
    EXPECT_FALSE (snapshot.status.warnings.empty ());
    dg::StoreyBindings bindings;
    const auto built = dg::BuildScene (snapshot, bindings);
    ASSERT_TRUE (built.scene);
    EXPECT_FALSE (At (*built.scene, dg::Level::Storey).empty ());
    for (const auto& binding : bindings.byBuilding.at ("building:Tower"))
        EXPECT_TRUE (binding.source.persistentId.empty ());
}

TEST (DiagramSnapshot, IncompleteOrUnownedSourcesAreRefused)
{
    const auto stories = Stories ({ 0, 3 }, { 1, 2 });
    const auto good = AtStories ({ Slab ("a", "Tower", 0, 6) }, stories);
    auto partial = std::make_shared<ms::Result> (*good);
    partial->complete = false;
    auto snapshot = dg::Capture (partial, stories, {});
    EXPECT_FALSE (snapshot.status.compatible);
    EXPECT_NE (std::find (snapshot.status.failures.begin (), snapshot.status.failures.end (),
                          dg::SnapshotIssue::IncompleteSlices),
               snapshot.status.failures.end ());

    auto unowned = std::make_shared<ms::Result> (*good);
    unowned->buildingSurfaces.clear ();
    snapshot = dg::Capture (unowned, stories, {});
    EXPECT_FALSE (snapshot.status.compatible);
    EXPECT_EQ (snapshot.status.affected, (std::vector<std::string> { "a" }));
    dg::StoreyBindings bindings;
    EXPECT_FALSE (dg::BuildScene (snapshot, bindings).scene);
    EXPECT_TRUE (bindings.byBuilding.empty ()); // a refused snapshot never rebinds
}

TEST (DiagramSnapshot, OneStoreyAtTwoElevationsIsADomainMismatch)
{
    const auto stories = Stories ({ 0, 3 }, { 1, 2 });
    auto mixed = std::make_shared<ms::Result> (
        *AtStories ({ Slab ("a", "Tower", 0, 6), Slab ("b", "Tower", 0, 6, 10) }, stories));
    for (auto& row : mixed->rows)
        if (row.guid == "b" && row.story == 1)
            row.z += 0.5;
    const auto snapshot = dg::Capture (mixed, stories, {});
    EXPECT_FALSE (snapshot.status.compatible);
    EXPECT_EQ (snapshot.status.affected, (std::vector<std::string> { "building:Tower" }));
}

TEST (DiagramSnapshot, InsertedStoryKeepsOtherStoreysAndDropsTheDeleted)
{
    const auto before = Stories ({ 0, 3, 6, 9 }, { 101, 102, 103, 105 });
    dg::StoreyBindings bindings;
    const auto first =
        dg::BuildScene (dg::Capture (AtStories ({ Slab ("a", "Tower", 0, 12) }, before), before, {}), bindings);
    ASSERT_TRUE (first.scene);
    const dg::EntityId id102 = ByFloor (bindings, "building:Tower", 102)->id;
    const dg::EntityId id103 = ByFloor (bindings, "building:Tower", 103)->id;
    const dg::EntityId id105 = ByFloor (bindings, "building:Tower", 105)->id;
    // Story 104 inserted at z 3; 102 and 103 move up a level; 105 deleted.
    const auto after = Stories ({ 0, 3, 6, 9 }, { 101, 104, 102, 103 });
    const auto second =
        dg::BuildScene (dg::Capture (AtStories ({ Slab ("a", "Tower", 0, 12) }, after), after, {}), bindings);
    ASSERT_TRUE (second.scene);
    EXPECT_EQ (ByFloor (bindings, "building:Tower", 102)->id, id102);
    EXPECT_EQ (ByFloor (bindings, "building:Tower", 103)->id, id103);
    EXPECT_NEAR (ByFloor (bindings, "building:Tower", 102)->source.z, 6.0, 1e-6);
    ASSERT_TRUE (ByFloor (bindings, "building:Tower", 104));
    EXPECT_EQ (second.dropped, (std::vector<dg::EntityId> { id105 }));
}

TEST (DiagramSnapshot, VanishedBuildingDropsItsBindings)
{
    const auto stories = Stories ({ 0, 3 }, { 1, 2 });
    dg::StoreyBindings bindings;
    dg::BuildScene (
        dg::Capture (AtStories ({ Slab ("a", "A", 0, 6), Slab ("b", "B", 0, 6, 30) }, stories), stories, {}), bindings);
    ASSERT_EQ (bindings.byBuilding.size (), 2u);
    const auto built =
        dg::BuildScene (dg::Capture (AtStories ({ Slab ("a", "A", 0, 6) }, stories), stories, {}), bindings);
    EXPECT_EQ (bindings.byBuilding.size (), 1u);
    EXPECT_EQ (built.dropped.size (), 2u);
}

TEST (DiagramSnapshot, SameSnapshotSameScene)
{
    const auto stories = Stories ({ 0, 3, 6 }, { 1, 2, 3 });
    const auto snapshot = dg::Capture (
        AtStories ({ Slab ("b", "B", 0, 9, 30), Slab ("a", "A", 0, 6), Slab ("c", "", 0, 3, 60) }, stories), stories,
        {});
    dg::StoreyBindings one, two;
    const auto a = dg::BuildScene (snapshot, one);
    const auto b = dg::BuildScene (snapshot, two);
    ASSERT_TRUE (a.scene && b.scene);
    ASSERT_EQ (a.scene->entities.size (), b.scene->entities.size ());
    for (size_t i = 0; i < a.scene->entities.size (); ++i) {
        EXPECT_EQ (a.scene->entities[i].id, b.scene->entities[i].id);
        EXPECT_EQ (a.scene->entities[i].boundsMin, b.scene->entities[i].boundsMin);
        EXPECT_EQ (a.scene->entities[i].boundsMax, b.scene->entities[i].boundsMax);
    }
    ASSERT_EQ (a.shells.size (), b.shells.size ());
    for (size_t i = 0; i < a.shells.size (); ++i)
        EXPECT_EQ (a.shells[i].positions, b.shells[i].positions);
}
