// The roof as shared-editor content (Edit/EditRoof.hpp) and its adapter to the generator,
// Mesh and Bake (ArchViz/RoofSlopeDocument.hpp): the document is the authority, the
// generator's Data is derived from it, and edits go through the one edit::Session history.
#include "ArchViz/RoofSlopeBake.hpp"
#include "ArchViz/RoofSlopeDocument.hpp"
#include "Edit/EditSession.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <string>
#include <vector>

namespace rs = geomsrv::archviz::roofslope;
namespace rb = geomsrv::archviz::roofslopebake;
namespace edit = geomsrv::edit;
namespace {
rs::Data Plan (std::vector<rs::Ring> rings)
{
    rs::Data d;
    std::string error;
    EXPECT_TRUE (rs::Plan (rings, 5, 2, 200, d, error)) << error;
    return d;
}
edit::Document Seeded (const rs::Data& data)
{
    edit::Document doc;
    std::string error;
    EXPECT_TRUE (rs::Seed (doc, data, error)) << error;
    return doc;
}
double Area (const rs::Data& d)
{
    double sum = 0;
    for (const auto& t : d.triangles)
        sum += t.area;
    return sum;
}
std::vector<std::vector<rs::Ring>> Fixtures ()
{
    rs::Ring rotated { { 0, 0 }, { 25, 0 }, { 25, 16 }, { 0, 16 } };
    for (auto& p : rotated) {
        const double x = p.x, y = p.y;
        p = { 500000 + x * std::cos (0.455) - y * std::sin (0.455),
              6000000 + x * std::sin (0.455) + y * std::cos (0.455) };
    }
    return {
        { { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } } },
        { { { 0, 0 }, { 30, 0 }, { 30, 12 }, { 16, 12 }, { 16, 24 }, { 0, 24 } },
          { { 4, 4 }, { 4, 8 }, { 8, 8 }, { 8, 4 } } },
        { rotated },
        { { { 0, 0 }, { 20, 0 }, { 20, 10 }, { 0, 10 } }, { { 0, 20 }, { 20, 20 }, { 20, 30 }, { 0, 30 } } },
        { { { 0, 0 }, { 15, 0 }, { 12, 10 }, { 0, 10 } } },
    };
}
} // namespace

TEST (EditRoof, SeedThenReadReproducesTheGeneratedRoofAndItsBakeRecipe)
{
    size_t baked = 0;
    for (const auto& rings : Fixtures ()) {
        const auto original = Plan (rings);
        const auto doc = Seeded (original);
        ASSERT_TRUE (doc.roof.has_value ());
        std::string error;
        EXPECT_TRUE (edit::Validate (doc, error)) << error;
        rs::Data read;
        ASSERT_TRUE (rs::Read (doc, read, error)) << error;
        ASSERT_EQ (read.cells.size (), original.cells.size ());
        for (size_t i = 0; i < read.cells.size (); ++i)
            for (int k = 0; k < 4; ++k)
                EXPECT_NEAR (read.cells[i].box[k], original.cells[i].box[k], 1e-6);
        EXPECT_NEAR (Area (read), Area (original), 1e-6);
        EXPECT_EQ (read.triangles.size (), original.triangles.size ());
        EXPECT_NEAR (read.ridgeZ, original.ridgeZ, 1e-12);
        EXPECT_NEAR (read.bearing, original.bearing, 1e-12);
        evp::nodegraph::json::JsonValue a, b;
        size_t roofsA = 0, levelsA = 0, roofsB = 0, levelsB = 0;
        // Bake refuses some generated plans (a drain clipped out of the roof): the round trip
        // must give the same verdict and reason, and the same recipe when it bakes.
        std::string whyA, whyB;
        const bool bakesA = rb::Transaction (original, -1, 1.5, "", a, roofsA, levelsA, whyA);
        const bool bakesB = rb::Transaction (read, -1, 1.5, "", b, roofsB, levelsB, whyB);
        EXPECT_EQ (bakesB, bakesA);
        EXPECT_EQ (whyB, whyA);
        EXPECT_EQ (roofsB, roofsA);
        EXPECT_EQ (levelsB, levelsA);
        baked += bakesA;
    }
    EXPECT_GE (baked, 3u) << "most fixtures bake, so the recipe comparison is not vacuous";
}
TEST (EditRoof, DisconnectedWingsAndOpeningsBecomeFootprintEntitiesWithTheRoofRole)
{
    const auto doc = Seeded (Plan ({ { { 0, 0 }, { 20, 0 }, { 20, 10 }, { 0, 10 } },
                                     { { 0, 20 }, { 20, 20 }, { 20, 30 }, { 0, 30 } },
                                     { { 2, 22 }, { 2, 24 }, { 4, 24 }, { 4, 22 } } }));
    ASSERT_EQ (doc.roof->footprints.size (), 2u);
    size_t contours = 0;
    for (edit::Id id : doc.roof->footprints) {
        const auto* e = edit::Find (doc, id);
        ASSERT_NE (e, nullptr);
        EXPECT_EQ (e->role, edit::kRoofFootprintRole);
        EXPECT_TRUE (e->closed);
        contours += edit::ContourCount (*e);
    }
    EXPECT_EQ (contours, 3u) << "the opening stays with the wing that holds it";
}
TEST (EditRoof, LocksAndHeightsTravelWithStableIdsThroughTheSessionHistory)
{
    auto data = Plan ({ { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } } });
    data.nodes[data.cells[0].drain].lockZ = true;
    edit::Session session (Seeded (data));
    session.SetValidator (rs::Check);
    const edit::Id drain = session.Doc ().roof->cells[0].drain;
    const edit::Id corner = session.Doc ().roof->cells[0].rim[0];
    EXPECT_TRUE (edit::FindRoofNode (*session.Doc ().roof, drain)->lockZ);
    const double z = edit::FindRoofNode (*session.Doc ().roof, corner)->p.z;
    ASSERT_TRUE (session.Apply ([&] (edit::Document& doc, std::string&) {
        edit::FindRoofNode (*doc.roof, corner)->p.z = z + 0.05;
        return true;
    })) << session.Message ();
    rs::Data read;
    std::vector<edit::Id> ids;
    std::string error;
    ASSERT_TRUE (rs::Read (session.Doc (), read, error, &ids)) << error;
    const auto at = std::find (ids.begin (), ids.end (), corner) - ids.begin ();
    EXPECT_NEAR (read.nodes[at].z, z + 0.05, 1e-12);
    ASSERT_TRUE (session.Undo ());
    EXPECT_NEAR (edit::FindRoofNode (*session.Doc ().roof, corner)->p.z, z, 1e-12);
    EXPECT_TRUE (edit::FindRoofNode (*session.Doc ().roof, drain)->lockZ);
}
TEST (EditRoof, AnInvalidRoofEditIsRefusedWithItsReasonAndTheLastValidStateKept)
{
    edit::Session session (Seeded (Plan ({ { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } } })));
    session.SetValidator (rs::Check);
    const auto before = session.Doc ();
    const edit::Id drain = before.roof->cells[0].drain;
    // A drain above its rim no longer drains its cell.
    EXPECT_FALSE (session.Apply ([&] (edit::Document& doc, std::string&) {
        edit::FindRoofNode (*doc.roof, drain)->p.z = 50;
        return true;
    }));
    EXPECT_NE (session.Message ().find ("lower than every perimeter point"), std::string::npos) << session.Message ();
    EXPECT_TRUE (edit::SameAuthoredContent (session.Doc (), before));
    // Growing the footprint past the drainage cells would leave roof undrained.
    const edit::Id footprint = before.roof->footprints[0];
    EXPECT_FALSE (session.Apply ([&] (edit::Document& doc, std::string&) {
        for (auto& v : edit::Find (doc, footprint)->vertices)
            if (v.p.x > 5)
                v.p.x += 1;
        return true;
    }));
    EXPECT_NE (session.Message ().find ("overlap, gap or lost opening"), std::string::npos) << session.Message ();
    EXPECT_TRUE (edit::SameAuthoredContent (session.Doc (), before));
    // A footprint edit the cells still cover is accepted and re-clips the faces.
    ASSERT_TRUE (session.Apply ([&] (edit::Document& doc, std::string&) {
        auto& v = edit::Find (doc, footprint)->vertices;
        for (auto& p : v)
            if (p.p.x > 5 && p.p.y > 5)
                p.p.x -= 1;
        return true;
    })) << session.Message ();
    rs::Data read;
    std::string error;
    ASSERT_TRUE (rs::Read (session.Doc (), read, error)) << error;
    EXPECT_NEAR (Area (read), 95, 1e-6);
}
TEST (EditRoof, StructuralValidationNamesTheBrokenRecord)
{
    auto doc = Seeded (Plan ({ { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } } }));
    std::string error;
    auto broken = doc;
    edit::FindRoofNode (*broken.roof, broken.roof->cells[0].drain)->p.z = broken.roof->datumZ - 1;
    EXPECT_FALSE (edit::Validate (broken, error));
    EXPECT_NE (error.find ("below the roof datum"), std::string::npos) << error;
    broken = doc;
    broken.roof->cells[0].rim[1] = broken.roof->cells[0].rim[0];
    EXPECT_FALSE (edit::Validate (broken, error));
    EXPECT_NE (error.find ("rim node"), std::string::npos) << error;
    broken = doc;
    broken.roof->nodes.push_back ({ edit::NewId (broken), edit::RoofNodeKind::Corner, { 1, 1, 6 } });
    EXPECT_FALSE (edit::Validate (broken, error));
    EXPECT_NE (error.find ("belongs to no cell"), std::string::npos) << error;
    broken = doc;
    broken.roof->nodes[0].id = broken.roof->footprints[0];
    EXPECT_FALSE (edit::Validate (broken, error)) << "roof ids share the document's id space";
    broken = doc;
    edit::Find (broken, broken.roof->footprints[0])->role = "platform";
    EXPECT_FALSE (edit::Validate (broken, error));
    EXPECT_FALSE (edit::SameAuthoredContent (broken, doc));
}
TEST (EditRoof, ReadRefusesShapesTheRectanglePresetCannotMeshYet)
{
    auto doc = Seeded (Plan ({ { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } } }));
    std::string error;
    rs::Data read;
    auto curved = doc;
    edit::Find (curved, curved.roof->footprints[0])->vertices[1].radius = 1;
    EXPECT_FALSE (rs::Read (curved, read, error));
    EXPECT_NE (error.find ("Curved"), std::string::npos) << error;
    auto skewed = doc;
    auto& n = *edit::FindRoofNode (*skewed.roof, skewed.roof->cells[0].rim[1]);
    n.p.y += 0.5;
    EXPECT_FALSE (rs::Read (skewed, read, error));
    EXPECT_NE (error.find ("freeform facets are not supported yet"), std::string::npos) << error;
}
TEST (EditRoof, SeedRefusesADrainSharedByTwoCellsAndLeavesTheDocumentUntouched)
{
    auto data = Plan ({ { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } } });
    data.cells.push_back (data.cells[0]);
    edit::Document doc;
    std::string error;
    EXPECT_FALSE (rs::Seed (doc, data, error));
    EXPECT_NE (error.find ("more than one cell"), std::string::npos) << error;
    EXPECT_FALSE (doc.roof.has_value ());
    EXPECT_TRUE (doc.entities.empty ());
}
