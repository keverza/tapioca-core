// ArchViz/SlabSlices: a massing slab's floors as slices. A floor too many or too few
// is a wrong gross area that the picture does not show, so the floor rules are pinned
// here -- the storey rule against the feasibility figures' own cases -- and so is the
// slice's outline, holes and arcs included.

#include "ArchViz/SlabSlices.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace slab = geomsrv::archviz::slabslices;
namespace so = geomsrv::archviz::storysliceoverlay;
using geomsrv::archviz::ProjectStoreys;

namespace {

size_t Count (double bottom, double height, const slab::Rule& rule, const std::vector<double>& levels)
{
    std::string problem;
    return slab::Floors (bottom, bottom + height, rule, levels, problem).size ();
}

slab::Rule Rule (slab::Cut cut)
{
    slab::Rule rule;
    rule.cut = cut;
    return rule;
}

ProjectStoreys Storeys (const std::vector<double>& levels)
{
    ProjectStoreys storeys;
    for (size_t i = 0; i < levels.size (); ++i) {
        storeys.levels.push_back (levels[i]);
        storeys.names.push_back ("S" + std::to_string (i));
        storeys.indices.push_back (int (i) - 1); // a basement, then the ground floor as 0
    }
    return storeys;
}

slab::Slab Block (double x0, double y0, double x1, double y1, double bottom, double top)
{
    slab::Slab block;
    block.guid = "11111111-2222-3333-4444-555555555555";
    block.id = "A-01";
    block.bottom = bottom;
    block.top = top;
    block.outer.xy = { x0, y0, x1, y0, x1, y1, x0, y1 };
    return block;
}

// The walls of a body between `z0` and `z1`, each ring of x, y corners extruded -- what
// a plane between them cuts. The caps lie in the planes the slices never cut.
geomsrv::Mesh Walls (const std::vector<std::vector<double>>& rings, double z0, double z1)
{
    geomsrv::Mesh mesh;
    mesh.guid = "11111111-2222-3333-4444-555555555555";
    for (const std::vector<double>& ring : rings) {
        const size_t n = ring.size () / 2;
        for (size_t i = 0; i < n; ++i) {
            const size_t j = (i + 1) % n;
            const uint32_t base = uint32_t (mesh.vertices.size () / 3);
            for (const double z : { z0, z1 }) {
                mesh.vertices.insert (mesh.vertices.end (), { ring[i * 2], ring[i * 2 + 1], z });
                mesh.vertices.insert (mesh.vertices.end (), { ring[j * 2], ring[j * 2 + 1], z });
            }
            // (i,z0) (j,z0) (i,z1) (j,z1)
            mesh.triangles.insert (mesh.triangles.end (), { base, base + 1, base + 3, base, base + 3, base + 2 });
        }
    }
    for (size_t v = 0; v + 2 < mesh.vertices.size (); v += 3)
        mesh.bounds.Expand (mesh.vertices[v], mesh.vertices[v + 1], mesh.vertices[v + 2]);
    return mesh;
}

} // namespace

// ⚠️ THE FEASIBILITY FIGURES' OWN CASES (massingcalc.story_floors): from the storey the
// bottom sits in, successive storey heights, the last one repeated above the top storey.
TEST (SlabSlices, TheStoreyRuleCountsLikeTheFeasibilityFigures)
{
    const slab::Rule storeys = Rule (slab::Cut::Storeys);
    const std::vector<double> levels = { -3.0, 0.0, 3.0, 7.0 };
    EXPECT_EQ (Count (-3.0, 5.6, storeys, levels), 2u);
    EXPECT_EQ (Count (3.0, 10.49, storeys, levels), 2u);
    EXPECT_EQ (Count (3.0, 10.5, storeys, levels), 3u);
    // A short COMPLETE storey is a floor; a short remainder under the top is not.
    EXPECT_EQ (Count (0.0, 4.4, storeys, { 0.0, 2.0, 5.0 }), 1u);
}

TEST (SlabSlices, TheStoreyRuleStartsAtTheBottomAndStepsByTheStoreys)
{
    std::string problem;
    const std::vector<slab::Floor> floors =
        slab::Floors (0.5, 9.5, Rule (slab::Cut::Storeys), { 0.0, 3.0, 6.5 }, problem);
    ASSERT_EQ (floors.size (), 3u) << problem;
    EXPECT_DOUBLE_EQ (floors[0].base, 0.5);
    EXPECT_DOUBLE_EQ (floors[0].height, 3.0);
    EXPECT_DOUBLE_EQ (floors[1].base, 3.5);
    EXPECT_DOUBLE_EQ (floors[1].height, 3.5);
    EXPECT_DOUBLE_EQ (floors[2].base, 7.0);
    EXPECT_NEAR (floors[2].height, 2.5, 1e-12); // 3.5 repeated, cut to 2.5 by the top: counted
    EXPECT_TRUE (problem.empty ());
}

TEST (SlabSlices, TheStoreyRuleNeedsTwoRisingStoreys)
{
    std::string problem;
    EXPECT_TRUE (slab::Floors (0.0, 6.0, Rule (slab::Cut::Storeys), { 0.0 }, problem).empty ());
    EXPECT_FALSE (problem.empty ());
    EXPECT_TRUE (slab::Floors (0.0, 6.0, Rule (slab::Cut::Storeys), { 0.0, 3.0, 3.0 }, problem).empty ());
    EXPECT_FALSE (problem.empty ());
}

TEST (SlabSlices, TheStoreyLevelRuleCutsAtTheLevelsInsideTheSlab)
{
    std::string problem;
    // Levels 0, 3, 6, 9; slab 1..8: cut at 3 (complete) and 6 (2 m to the top: dropped).
    std::vector<slab::Floor> floors =
        slab::Floors (1.0, 8.0, Rule (slab::Cut::StoreyLevels), { 0.0, 3.0, 6.0, 9.0 }, problem);
    ASSERT_EQ (floors.size (), 1u);
    EXPECT_DOUBLE_EQ (floors[0].base, 3.0);
    EXPECT_DOUBLE_EQ (floors[0].height, 3.0);
    floors = slab::Floors (0.0, 9.0, Rule (slab::Cut::StoreyLevels), { 0.0, 3.0, 6.0, 9.0 }, problem);
    ASSERT_EQ (floors.size (), 3u); // 9 is the roof, not a floor
    EXPECT_DOUBLE_EQ (floors[2].base, 6.0);
}

// massingcalc.fixed_floors' cases: whole floors, and a remainder from 2.5 m.
TEST (SlabSlices, TheStepRuleCountsLikeTheFeasibilityFigures)
{
    slab::Rule step = Rule (slab::Cut::Step);
    step.stepMetres = 3.1;
    EXPECT_EQ (Count (0.0, 5.59, step, {}), 1u);
    EXPECT_EQ (Count (0.0, 5.6, step, {}), 2u);
    EXPECT_EQ (Count (0.0, 2.49, step, {}), 0u);
    EXPECT_EQ (Count (0.0, 2.5, step, {}), 1u);
    EXPECT_EQ (Count (0.0, 9.3, step, {}), 3u);
    step.minTopMetres = 0.0;
    EXPECT_EQ (Count (0.0, 5.59, step, {}), 2u);
}

TEST (SlabSlices, TheLevelRuleCutsWhereItIsToldInsideTheSlab)
{
    slab::Rule levels = Rule (slab::Cut::Levels);
    levels.levels = { 7.0, 1.0, 4.0, 1.0 + 1e-9, 20.0, -5.0 };
    std::string problem;
    const std::vector<slab::Floor> floors = slab::Floors (0.0, 9.0, levels, {}, problem);
    ASSERT_EQ (floors.size (), 3u); // 1, 4, 7: sorted, one duplicate, two outside
    EXPECT_DOUBLE_EQ (floors[0].base, 1.0);
    EXPECT_DOUBLE_EQ (floors[0].height, 3.0);
    EXPECT_DOUBLE_EQ (floors[2].height, 2.0); // to the top, however low: it was asked for
    levels.levels = { 20.0 };
    EXPECT_TRUE (slab::Floors (0.0, 9.0, levels, {}, problem).empty ());
    EXPECT_FALSE (problem.empty ());
}

TEST (SlabSlices, AFlatSlabHasNoFloors)
{
    std::string problem;
    EXPECT_TRUE (slab::Floors (3.0, 3.0, Rule (slab::Cut::Step), {}, problem).empty ());
    EXPECT_EQ (problem, "the slab has no height");
}

// One slice per floor, at its base plus the offset, named for the slab and its floor,
// in the storey its cut lies in; the area is the outline's less the holes.
TEST (SlabSlices, EachFloorIsASliceOfTheOutlineLessItsHoles)
{
    slab::Slab block = Block (0, 0, 20, 10, 0.0, 9.0);
    block.holes.push_back ({ { 2, 2, 2, 4, 4, 4, 4, 2 }, {} }); // wound the other way: still a hole
    slab::Rule rule = Rule (slab::Cut::Step);
    rule.stepMetres = 3.0;
    rule.offsetMetres = 1.1;
    std::vector<so::Slice> slices;
    const slab::Summary summary = slab::SliceSlab (block, rule, Storeys ({ -3.0, 0.0, 3.0, 6.0 }), slices);
    ASSERT_EQ (slices.size (), 3u);
    EXPECT_DOUBLE_EQ (summary.footprintM2, 200.0);
    EXPECT_DOUBLE_EQ (summary.sliceAreaM2, 196.0);
    EXPECT_DOUBLE_EQ (summary.areaM2, 588.0);
    EXPECT_TRUE (summary.problem.empty ());
    EXPECT_DOUBLE_EQ (slices[1].z, 4.1);
    EXPECT_EQ (slices[1].name, "A-01 F2");
    EXPECT_EQ (slices[1].storey, 1); // the storey at 3.0
    EXPECT_EQ (slices[0].storey, 0);
    ASSERT_EQ (slices[1].chains.size (), 2u);
    EXPECT_TRUE (slices[1].chains[1].closed);
    EXPECT_DOUBLE_EQ (slices[1].areaM2, 196.0);
}

TEST (SlabSlices, TheCutStaysInsideTheSlab)
{
    slab::Rule rule = Rule (slab::Cut::Levels);
    rule.levels = { 8.5 };
    rule.offsetMetres = 1.0;
    std::vector<so::Slice> slices;
    slab::SliceSlab (Block (0, 0, 1, 1, 0.0, 9.0), rule, Storeys ({ 0.0, 3.0 }), slices);
    ASSERT_EQ (slices.size (), 1u);
    EXPECT_DOUBLE_EQ (slices[0].z, 9.0);
}

// ⚠️ A CIRCLE IS TWO NODES AND TWO HALF-TURN ARCS in Archicad's polygon memo. Read as
// its chords it is a line with no area; tessellated it is the disc.
TEST (SlabSlices, AnArcedOutlineIsItsArcNotItsChord)
{
    slab::Slab disc;
    disc.bottom = 0.0;
    disc.top = 3.0;
    disc.outer.xy = { -5.0, 0.0, 5.0, 0.0 };
    disc.outer.arcs = { 3.14159265358979, 3.14159265358979 };
    slab::Rule rule = Rule (slab::Cut::Step);
    std::vector<so::Slice> slices;
    const slab::Summary summary = slab::SliceSlab (disc, rule, Storeys ({ 0.0, 3.0 }), slices);
    ASSERT_EQ (slices.size (), 1u);
    EXPECT_NEAR (summary.footprintM2, 3.14159265358979 * 25.0, 0.05);
    EXPECT_EQ (slices[0].name, "Slab F1");
}

TEST (SlabSlices, AnOutlineWithoutAreaIsSaidNotSliced)
{
    slab::Slab line;
    line.bottom = 0.0;
    line.top = 3.0;
    line.outer.xy = { 0.0, 0.0, 5.0, 0.0 };
    std::vector<so::Slice> slices;
    const slab::Summary summary = slab::SliceSlab (line, Rule (slab::Cut::Step), Storeys ({ 0.0 }), slices);
    EXPECT_TRUE (slices.empty ());
    EXPECT_FALSE (summary.problem.empty ());
}

// The slices go to the same layer builder as the model's storeys: each slab's floor is a
// fill, an outline per contour and one label.
TEST (SlabSlices, TheSlicesDrawAsTheStoreySliceLayer)
{
    slab::Slab block = Block (0, 0, 20, 10, 0.0, 6.0);
    std::vector<so::Slice> slices;
    slab::SliceSlab (block, Rule (slab::Cut::Storeys), Storeys ({ 0.0, 3.0 }), slices);
    ASSERT_EQ (slices.size (), 2u);
    so::Controls controls;
    controls.labelName = true;
    const so::Built built = so::BuildLayer (slices, controls);
    EXPECT_EQ (built.slices, 2u);
    EXPECT_DOUBLE_EQ (built.areaM2, 400.0);
    EXPECT_EQ (built.layer.meshes.size (), 2u);
    EXPECT_EQ (built.layer.polylines.size (), 2u);
    ASSERT_EQ (built.layer.texts.size (), 2u);
    EXPECT_EQ (built.layer.texts[1].text, "A-01 F2  200.0 m\xC2\xB2");
    EXPECT_DOUBLE_EQ (built.layer.texts[1].at[2], 3.0);
}

// ⚠️ THE USER, 2026-10-01: a subtraction from a massing slab did not change its slices --
// they were its polygon, and a solid element operation lives only in the 3D model. Cut
// from the body, a courtyard subtracted through it is a HOLE in every floor (the storey
// union would have filled it: it takes every ring as a region).
TEST (SlabSlices, ABodysSubtractedCourtyardIsAHoleInEveryFloor)
{
    const slab::Slab block = Block (0.0, 0.0, 20.0, 10.0, 0.0, 9.0);
    const geomsrv::Mesh body = Walls (
        { { 0.0, 0.0, 20.0, 0.0, 20.0, 10.0, 0.0, 10.0 }, { 8.0, 3.0, 12.0, 3.0, 12.0, 7.0, 8.0, 7.0 } }, 0.0, 9.0);
    slab::Rule rule = Rule (slab::Cut::Step);
    rule.stepMetres = 3.0;
    std::vector<so::Slice> slices;
    const slab::Summary summary = slab::SliceBody (block, body, rule, Storeys ({ 0.0, 3.0, 6.0 }), slices);

    ASSERT_EQ (slices.size (), 3u);
    for (const so::Slice& slice : slices) {
        EXPECT_NEAR (slice.areaM2, 200.0 - 16.0, 1e-6);
        EXPECT_EQ (slice.chains.size (), 2u) << "the outline and the courtyard";
    }
    EXPECT_TRUE (summary.body);
    EXPECT_NEAR (summary.areaM2, 3.0 * 184.0, 1e-6);
    EXPECT_NEAR (summary.footprintM2, 200.0, 1e-6) << "the footprint stays the polygon's";
    EXPECT_EQ (slices[1].name, "A-01 F2");
    EXPECT_NEAR (summary.floors[2].areaM2, 184.0, 1e-6);
}

// The top floor subtracted away: the slab's record still reaches 9 m, its body 6 m. That
// floor has no slice and is said, and the others are whole.
TEST (SlabSlices, AFloorTheOperationsRemovedHasNoSlice)
{
    const slab::Slab block = Block (0.0, 0.0, 20.0, 10.0, 0.0, 9.0);
    const geomsrv::Mesh body = Walls ({ { 0.0, 0.0, 20.0, 0.0, 20.0, 10.0, 0.0, 10.0 } }, 0.0, 6.0);
    slab::Rule rule = Rule (slab::Cut::Step);
    rule.stepMetres = 3.0;
    std::vector<so::Slice> slices;
    const slab::Summary summary = slab::SliceBody (block, body, rule, Storeys ({ 0.0, 3.0, 6.0 }), slices);

    EXPECT_EQ (summary.floors.size (), 3u);
    ASSERT_EQ (slices.size (), 2u);
    EXPECT_NEAR (summary.areaM2, 400.0, 1e-6);
    EXPECT_EQ (summary.floors[2].areaM2, 0.0);
    EXPECT_NE (summary.problem.find ("1 floor(s) with no cross-section"), std::string::npos) << summary.problem;
}

// The polygon path names each floor's area too, so a reader need not know which path cut.
TEST (SlabSlices, EveryFloorCarriesItsArea)
{
    const slab::Slab block = Block (0.0, 0.0, 10.0, 10.0, 0.0, 6.0);
    slab::Rule rule = Rule (slab::Cut::Step);
    rule.stepMetres = 3.0;
    std::vector<so::Slice> slices;
    const slab::Summary summary = slab::SliceSlab (block, rule, Storeys ({ 0.0, 3.0 }), slices);
    ASSERT_EQ (summary.floors.size (), 2u);
    EXPECT_NEAR (summary.floors[0].areaM2, 100.0, 1e-9);
    EXPECT_NEAR (summary.floors[1].areaM2, 100.0, 1e-9);
    EXPECT_FALSE (summary.body);
}

// ⚠️ THE USER, 2026-10-01: "If Storey has no name do not reuse previous number, increment as
// it is a new storey" -- every slice above the top storey read the top storey's number. A
// floor above the project's top storey is a storey of its own, one below its lowest too;
// within them, each slice floor now advances even inside the same project story.
TEST (SlabSlices, AFloorAboveTheTopStoreyIsAStoreyOfItsOwn)
{
    const ProjectStoreys five = Storeys ({ 0.0, 3.0, 6.0, 9.0, 12.1 }); // numbered -1 to 3
    EXPECT_EQ (slab::StoreysAt (five, { 0.0, 3.0, 6.0, 9.0, 12.1, 15.2, 18.3, 21.4 }),
               (std::vector<int> { -1, 0, 1, 2, 3, 4, 5, 6 }));
    EXPECT_EQ (slab::StoreysAt (five, { 0.0, 1.5 }), (std::vector<int> { -1, 0 })) << "one project story, two floors";
    const ProjectStoreys two = Storeys ({ 3.0, 6.0 }); // numbered -1 and 0
    EXPECT_EQ (slab::StoreysAt (two, { 0.0, 1.5, 3.0, 6.0 }), (std::vector<int> { -3, -2, -1, 0 }))
        << "below the lowest, numbered down from it";
    EXPECT_EQ (slab::StoreysAt (ProjectStoreys (), { 0.0, 3.0 }), (std::vector<int> { 0, 1 }));
}

TEST (SlabSlices, SliceNumbersAdvanceInsideOneTallProjectStoryAndWithoutProjectLevels)
{
    ProjectStoreys storeys;
    storeys.levels = { 0, 30 };
    storeys.indices = { 0, 1 };
    slab::Rule rule = Rule (slab::Cut::Step);
    rule.stepMetres = 3;
    const auto block = Block (0, 0, 10, 10, 0, 15);
    for (const auto& levels : { storeys, ProjectStoreys {} }) {
        std::vector<so::Slice> slices;
        const auto summary = slab::SliceSlab (block, rule, levels, slices);
        ASSERT_EQ (slices.size (), 5u) << summary.problem;
        for (size_t i = 0; i < slices.size (); ++i)
            EXPECT_EQ (slices[i].storey, int (i));
        const auto body = Walls ({ block.outer.xy }, 0, 15);
        slices.clear ();
        slab::SliceBody (block, body, rule, levels, slices);
        ASSERT_EQ (slices.size (), 5u);
        for (size_t i = 0; i < slices.size (); ++i)
            EXPECT_EQ (slices[i].storey, int (i));
    }
}

// The user's case through the slicing: eight floors over five storeys, the three above the
// top one numbered on from it -- in the hover readout's "Storey" row.
TEST (SlabSlices, TheSlicesAboveTheTopStoreyCountOn)
{
    std::vector<so::Slice> slices;
    const slab::Summary summary = slab::SliceSlab (Block (0, 0, 20, 20, 0.0, 24.5), Rule (slab::Cut::Storeys),
                                                   Storeys ({ 0.0, 3.0, 6.0, 9.0, 12.1 }), slices);
    ASSERT_EQ (slices.size (), 8u) << summary.problem;
    for (size_t k = 0; k < slices.size (); ++k)
        EXPECT_EQ (slices[k].storey, int (k) - 1) << "floor " << k + 1;
}
