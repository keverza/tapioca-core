// Residential fixtures for the planting space fit (#278 Stage 2): small box
// scenes run through the real room measure (SunStudyCrownClearance, the
// study's occluders) and the size classes (PlantingSizeFit), one question each:
// a narrow bed, an open courtyard, an inclined host, a podium beside a tower,
// and a bed with a hole. The balcony overhang is in test_sunstudyreceptor.cpp
// and test_plantingscheme.cpp.

#include "ArchViz/PlantingHudModel.hpp"
#include "ArchViz/PlantingScheme.hpp"
#include "ArchViz/PlantingSizeFit.hpp"
#include "ArchViz/PlantingZones.hpp"
#include "SunStudy/SunStudyCrownClearance.hpp"
#include "SunStudy/SunStudyKernels.hpp"
#include "SunStudy/SunStudyOccluders.hpp"

#include "MeshFixtures.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using namespace geomsrv::archviz::planting;
using evp::sunstudy::CrownClearance;
using evp::sunstudy::CrownClearanceOptions;

namespace {

struct Sample {
    double x, y, z;
};

// Measures `samples` (all standing on `host`) against `meshes`, and returns a
// site in full sun with that room.
SchemeSite Measure (std::vector<geomsrv::Mesh> meshes, const std::string& host, const std::vector<Sample>& samples)
{
    static uint64_t id = 27800;
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    snapshot->id = ++id;
    snapshot->meshes = std::move (meshes);
    const auto occluders = evp::sunstudy::BuildSunStudyOccluders (
        *snapshot, evp::sunstudy::kernels::ResolveElementRoles (*snapshot, {}, {}, {}));
    SchemeSite site;
    site.layerHeight = 0.5f;
    for (const Sample& s : samples) {
        site.x.push_back (s.x);
        site.y.push_back (s.y);
        site.groundZ.push_back (s.z);
        site.cls.push_back (2); // full sun: these fixtures are about space
        site.host.push_back (0);
        site.area.push_back (0.25);
    }
    CrownClearanceOptions options;
    options.maxParallel = 1;
    std::string error;
    EXPECT_TRUE (MeasureCrownClearance (*occluders, site.x, site.y, site.groundZ, site.host, { host }, options,
                                        site.clearance, error))
        << error;
    return site;
}

PlantingHud Hud ()
{
    PlantingHud hud;
    hud.deepShade = false;
    return hud;
}

SizeClass Class (const char* name, float spread, float height)
{
    SizeClass c;
    c.name = name;
    c.spread = spread;
    c.height = height;
    return c;
}

} // namespace

// A 1.2 m bed between two 3 m walls: a 1.0 m shrub fits, a 1.5 m one does not.
TEST (PlantingFixtures, NarrowBedHoldsOnlyANarrowCrown)
{
    const SchemeSite site = Measure ({ evptest::MakeBox ("ground", -5, -5, -0.3, 10.2, 10, 0.3),
                                       evptest::MakeBox ("wallW", -0.3, -5, 0, 0.3, 10, 3),
                                       evptest::MakeBox ("wallE", 1.2, -5, 0, 0.3, 10, 3) },
                                     "ground", { { 0.6, 0.0, 0.0 } });
    EXPECT_NEAR (site.clearance.FreeAt (0, 1), 0.6, 1e-4);
    const std::vector<SizeClass> classes { Class ("Narrow", 1.0f, 1.0f), Class ("Small", 1.5f, 1.5f) };
    const SizeFitResult fit = EvaluateSizeClasses (Hud (), site, classes, 0.0);
    EXPECT_EQ (fit.ReasonAt (0, 1, 2), SizeReason::Narrow);
    EXPECT_EQ (fit.ReasonAt (0, 0, 2), SizeReason::Fits);
    EXPECT_EQ (fit.largest[0], 0);
    // A margin takes it away again: 0.5 + 0.15 > 0.6.
    EXPECT_EQ (EvaluateSizeClasses (Hud (), site, classes, 0.15).largest[0], -1);
}

// A 20 x 20 m courtyard inside 12 m buildings: the centre holds a 10 m tree
// crown; a 15 m tall one rises above the roofs, where nothing is in reach.
TEST (PlantingFixtures, OpenCourtyardHoldsALargeTreeAtItsCentre)
{
    const SchemeSite site =
        Measure ({ evptest::MakeBox ("yard", 0, 0, -0.3, 20, 20, 0.3), evptest::MakeBox ("N", -5, 20, 0, 30, 5, 12),
                   evptest::MakeBox ("S", -5, -5, 0, 30, 5, 12), evptest::MakeBox ("W", -5, 0, 0, 5, 20, 12),
                   evptest::MakeBox ("E", 20, 0, 0, 5, 20, 12) },
                 "yard", { { 10, 10, 0 }, { 3, 10, 0 } });
    const std::vector<SizeClass> classes = DefaultSizeClasses ();
    const SizeFitResult fit = EvaluateSizeClasses (Hud (), site, classes, 0.0);
    EXPECT_EQ (fit.largest[0], 2) << "Large at the centre: 10 m to every facade";
    EXPECT_EQ (fit.ReasonAt (1, 2, 3), SizeReason::Narrow) << "3 m from the west facade";
    EXPECT_EQ (fit.largest[1], 1) << "Medium (2 m radius) still fits there";
    EXPECT_LT (site.clearance.overhead[0], 0.0f) << "open sky";
}

// A host sloping 30 %: horizontal rays skip the sample's own host, so the slope
// is not a wall; a separate retaining wall still is.
TEST (PlantingFixtures, AnInclinedHostIsNotAWall)
{
    geomsrv::Mesh slope;
    slope.guid = "slope";
    slope.vertices = { 0, 0, 0, 10, 0, 3, 10, 10, 3, 0, 10, 0 };
    slope.triangles = { 0, 1, 2, 0, 2, 3 };
    slope.triMaterial = { 0, 0 };
    evptest::RecomputeBounds (slope);
    const SchemeSite site =
        Measure ({ slope, evptest::MakeBox ("retaining", 9, 0, 0, 0.3, 10, 4) }, "slope", { { 5, 5, 1.5 } });
    EXPECT_NEAR (site.clearance.FreeAt (0, 0), 4.0, 1e-4) << "the retaining wall, not the slope 0.5 m uphill";
    EXPECT_EQ (EvaluateSizeClasses (Hud (), site, DefaultSizeClasses (), 0.0).largest[0], 1) << "Medium, r 2 m";
}

// A podium roof at 4 m beside a 30 m tower: the podium is the host; the tower
// limits the crown at every level a tree reaches; the podium's own edge does
// not.
TEST (PlantingFixtures, APodiumBesideATower)
{
    const SchemeSite site =
        Measure ({ evptest::MakeBox ("podium", 0, 0, 0, 20, 20, 4), evptest::MakeBox ("tower", 12, 0, 4, 8, 20, 26) },
                 "podium", { { 9, 10, 4 }, { 4, 10, 4 } });
    EXPECT_NEAR (site.clearance.FreeAt (0, 3), 3.0, 1e-4) << "the tower, 3 m east, at 1.5 m";
    EXPECT_NEAR (site.clearance.FreeAt (1, 3), 8.0, 1e-4);
    const SizeFitResult fit = EvaluateSizeClasses (Hud (), site, DefaultSizeClasses (), 0.0);
    EXPECT_EQ (fit.largest[0], 1) << "Medium by the tower";
    EXPECT_EQ (fit.largest[1], 2) << "Large 8 m from the tower: the podium's own edges are not walls";
}

// A bed with a hole (a tree pit grate): the hole's samples are not the bed's,
// and a no-go strip along it keeps crowns off the grate.
TEST (PlantingFixtures, ABedWithAHole)
{
    SchemeSite site = Measure ({ evptest::MakeBox ("ground", 0, 0, -0.3, 10, 10, 0.3) }, "ground",
                               { { 2, 5, 0 }, { 5, 5, 0 }, { 8, 5, 0 } });
    PlantingZone bed;
    bed.rings = { { 0, 0, 10, 0, 10, 10, 0, 10 }, { 4, 4, 6, 4, 6, 6, 4, 6 } };
    const SchemeSite inBed = RestrictToZone (site, bed);
    ASSERT_EQ (inBed.Size (), 2u) << "the sample in the hole is not in the bed";
    EXPECT_EQ (inBed.index, (std::vector<size_t> { 0, 2 }));
    PlantingZone grate;
    grate.role = ZoneRole::NoGo;
    grate.rings = { { 4, 4, 6, 4, 6, 6, 4, 6 } };
    NoGoDistances ({ bed, grate }, site.x, site.y, site.noGoDistance, site.noGoAllowance);
    const SchemeSite kept = RestrictToZone (site, bed);
    const SizeFitResult fit = EvaluateSizeClasses (Hud (), kept, DefaultSizeClasses (), 0.0);
    EXPECT_EQ (fit.ReasonAt (0, 2, 3), SizeReason::NoGo) << "Large's 5 m crown would reach the grate 2 m away";
    EXPECT_EQ (fit.ReasonAt (0, 1, 3), SizeReason::Fits) << "Medium's 2 m crown just touches it: allowed";
    EXPECT_EQ (fit.largest[0], 1);
}
