// The Plan view's floor designs as the project keeps them: JSON beside the stairwells, read back
// as written (to the millimetre), refused when malformed; Save writes them, Reset reads them.
#include "ArchViz/FloorSchemeDesigns.hpp"
#include "ArchViz/HudBuildingPlan.hpp"
#include "ArchViz/HudMetadata.hpp"
#include "Metadata/TapiocaMetadata.hpp"
#include <gtest/gtest.h>

namespace fs = geomsrv::archviz::floorscheme;
namespace fe = geomsrv::archviz::floorscheme::edit;
namespace bp = geomsrv::archviz::buildingplan;
namespace meta = geomsrv::metadata;
namespace hudmeta = geomsrv::archviz::hudmeta;

namespace {
fe::Designs Sample ()
{
    fe::Design d;
    d.added = { { { 36, 0 }, { 40, 0 }, { 40, 16 }, { 36, 16 } } };
    d.cut = { { { 0, 0 }, { 6, 0 }, { 6, 6 }, { 0, 6 } } };
    d.shallow = fs::Access::OneSide;
    d.locked = { { 12.5, 3.25 } };
    d.pins.cores = { { { 18, 8.1 }, 4.5, 4.2 } };
    d.pins.access = { { { 20, 8 }, fs::Access::Rows } };
    d.pins.walls = { { { 11.636, 3.55 } } };
    d.pins.rooms = { { { 5.8, 3.55 }, 1.5 } };
    d.pins.ends = { { { 4.2, 8 } } };
    d.pins.counts = { { { 8.4, 3.55 }, 2 } };
    fe::Designs designs;
    designs.floors[0] = d;
    designs.floors[3] = fe::Design {};
    designs.unique = { 3 };
    return designs;
}
} // namespace

TEST (FloorSchemeDesigns, DesignsReadBackAsWritten)
{
    const auto designs = Sample ();
    const std::string text = fe::ToJson (designs);
    ASSERT_FALSE (text.empty ());
    fe::Designs back;
    std::string error;
    ASSERT_TRUE (fe::FromJson (text, back, error)) << error;
    EXPECT_TRUE (back == designs) << text;
    EXPECT_EQ (fe::ToJson (back), text);
    EXPECT_TRUE (fe::ToJson ({}).empty ()) << "no designs: nothing written";
    ASSERT_TRUE (fe::FromJson ("", back, error));
    EXPECT_TRUE (back.Empty ());
}

TEST (FloorSchemeDesigns, MalformedDesignsAreRefusedAndLeaveWhatWasThere)
{
    auto kept = Sample ();
    std::string error;
    for (const char* text : { "{", "[]", "{\"version\":9,\"floors\":[]}", "{\"version\":1}",
                              "{\"version\":1,\"floors\":[{\"story\":0,\"design\":{\"walls\":[[1]]}}]}",
                              "{\"version\":1,\"floors\":[{\"story\":0,\"design\":{\"shallow\":42}}]}" }) {
        error.clear ();
        EXPECT_FALSE (fe::FromJson (text, kept, error)) << text;
        EXPECT_FALSE (error.empty ()) << text;
    }
    EXPECT_TRUE (kept == Sample ());
}

TEST (FloorSchemeDesigns, SaveWritesTheDesignsWithTheStairsAndReadsThemBack)
{
    meta::EntityMetadata entity;
    hudmeta::Edit edit;
    edit.id = bp::kLocations;
    edit.type = meta::ValueType::List;
    edit.numbers = { 18, 8.1 };
    edit.text = fe::ToJson (Sample ());
    std::string error;
    const auto schema = meta::DefaultSchema ();
    ASSERT_TRUE (hudmeta::Apply (entity, edit, schema, 0, error)) << error;
    EXPECT_TRUE (meta::Validate (entity, schema).empty ());
    const auto stored = bp::Read (entity);
    EXPECT_EQ (stored.cores.size (), 1u);
    EXPECT_EQ (stored.designs, edit.text);
    EXPECT_NE (bp::Fingerprint (entity).find ("floorDesigns"), std::string::npos) << "a design change is a conflict";
    // No stairs but designs: the designs stay; no designs: they go.
    edit.action = hudmeta::Edit::Action::Clear;
    ASSERT_TRUE (hudmeta::Apply (entity, edit, schema, 0, error)) << error;
    EXPECT_TRUE (bp::Read (entity).cores.empty ());
    EXPECT_EQ (bp::Read (entity).designs, edit.text);
    edit.text.clear ();
    ASSERT_TRUE (hudmeta::Apply (entity, edit, schema, 0, error)) << error;
    EXPECT_TRUE (bp::Read (entity).designs.empty ());
}
