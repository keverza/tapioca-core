#include "hud_fixture.hpp"
#include "ArchViz/HudMassing.hpp"
#include "ArchViz/HudMetadata.hpp"
#include "Metadata/TapiocaMetadata.hpp"

#include <gtest/gtest.h>

namespace hm = geomsrv::archviz::hudmassing;
namespace meta = geomsrv::metadata;
using namespace hudtest;

TEST (HudMassing, UpdateReplacesWhileAddDeduplicatesAndRemoveKeepsOthers)
{
    auto writes = hm::Plan ({ hm::Group::MassingSlabs, hm::Action::Update }, { "A", "B" }, { "B", "C", "C" });
    ASSERT_EQ (writes.size (), 2u);
    EXPECT_EQ (writes[0].guid, "A");
    EXPECT_FALSE (writes[0].assign);
    EXPECT_EQ (writes[1].guid, "C");
    EXPECT_TRUE (writes[1].assign);
    writes = hm::Plan ({ hm::Group::ExistingTerrain, hm::Action::Add }, { "A" }, { "A", "B", "B" });
    ASSERT_EQ (writes.size (), 1u);
    EXPECT_EQ (writes[0].guid, "B");
    EXPECT_TRUE (writes[0].assign);
    writes = hm::Plan ({ hm::Group::ExistingTerrain, hm::Action::Remove }, { "A", "B" }, { "B", "C" });
    ASSERT_EQ (writes.size (), 1u);
    EXPECT_EQ (writes[0].guid, "B");
    EXPECT_FALSE (writes[0].assign);
}

TEST (HudMassing, ClearOnlyRemovesMembersAndReselectNeverWrites)
{
    auto writes = hm::Plan ({ hm::Group::PropertyLine, hm::Action::Clear }, { "A", "B" }, { "C" });
    ASSERT_EQ (writes.size (), 2u);
    for (const auto& write : writes)
        EXPECT_FALSE (write.assign);
    EXPECT_TRUE (hm::Plan ({ hm::Group::PropertyLine, hm::Action::Reselect }, { "A" }, { "B" }).empty ());
    EXPECT_TRUE (hm::Plan ({ hm::Group::PropertyLine, hm::Action::Update }, { "A" }, { "A", "A" }).empty ());
}

TEST (HudMassing, RolesAndBuildingSlabFieldsUseTheElementMetadataSchema)
{
    EXPECT_STREQ (hm::Role (hm::Group::PropertyLine), "PropertyLine");
    EXPECT_STREQ (hm::Role (hm::Group::ExistingTerrain), "ExistingTerrain");
    EXPECT_STREQ (hm::Role (hm::Group::NewTerrain), "NewTerrain");
    EXPECT_STREQ (hm::Role (hm::Group::MassingSlabs), "MassingSlab");
    const auto schema = meta::DefaultSchema ();
    ASSERT_NE (schema.Find ("massing.height"), nullptr);
    EXPECT_EQ (schema.Find ("massing.height")->type, meta::ValueType::Length);
    EXPECT_EQ (schema.Find ("massing.story")->type, meta::ValueType::Int);
    meta::EntityMetadata entity;
    for (const char* key : { "tapioca.role", "massing.buildingId", "massing.function" }) {
        meta::Property p;
        p.key = key;
        p.value = meta::Value::Text ("example");
        meta::SetProperty (entity, p);
    }
    EXPECT_TRUE (meta::Validate (entity, schema).empty ());
    const auto page = geomsrv::archviz::hudmeta::Fields (schema, { entity }, 1);
    EXPECT_TRUE (page.known);
}

TEST (HudMassing, NativeTabDrawsWithoutAnyPythonPanelOrLayer)
{
    Watched hud;
    hud::OwnPages pages;
    pages.standalone = true;
    pages.massing.known = true;
    pages.massing.guids[0] = { "parcel" };
    hud.engine.SetOwnPages (pages);
    hud::SelectKey (*hud.state, hm::kTabKey);
    const auto out = hud.Lay ({}, At (600, 600));
    EXPECT_EQ (out.hostKey, hm::kTabKey);
    EXPECT_GT (out.host.height, 180);
    EXPECT_TRUE (out.panels.empty ());
    EXPECT_TRUE (hud::TakeMassingRequests (*hud.state).empty ());
    // Locate the first five-button row below the Define header and property label.
    // Horizontal control islands are discovered from real ImGui hover, not guessed.
    float row = 0;
    for (float y = 80; y < 150; y += 1)
        if (hud.Lay ({}, At (40, y)).hand) {
            row = y + 2;
            break;
        }
    ASSERT_GT (row, 0);
    hud.Click ({}, 40, row);
    const auto requests = hud::TakeMassingRequests (*hud.state);
    ASSERT_EQ (requests.size (), 1u);
    EXPECT_EQ (requests[0].group, hm::Group::PropertyLine);
    EXPECT_EQ (requests[0].action, hm::Action::Update);
    EXPECT_TRUE (hud::TakeMassingRequests (*hud.state).empty ());
    hud::ClearState (*hud.state);
    EXPECT_TRUE (hud::TakeMassingRequests (*hud.state).empty ());
}

TEST (HudMassing, EnvelopeCheckboxBelowCollapseControlsLayerVisibilityEvenBeforeTheLayerExists)
{
    Watched hud;
    hud::OwnPages pages;
    pages.standalone = true;
    hud.engine.SetOwnPages (pages);
    hud::SelectKey (*hud.state, hm::kTabKey);
    hud.Lay ({}, At (600, 600));
    const auto layout = hud.Lay ({}, At (600, 600));
    std::vector<float> controls;
    bool inControl = false;
    for (float y = 16 + layout.host.height - 1; y > 60; --y)
        if (hud.Lay ({}, At (40, y)).hand) {
            if (!inControl)
                controls.push_back (y - 4);
            inControl = true;
        }
        else {
            inControl = false;
            if (controls.size () == 3)
                break;
        }
    ASSERT_EQ (controls.size (), 3u) << "Large floors follows unique buildings and envelope; envelope follows collapse";
    const float lastControl = controls[2];
    ASSERT_GT (lastControl, 60);
    hud::TakeMassingCalculations (*hud.state); // Discard the existing initial Massing-page preview request.
    constexpr const char* envelope = geomsrv::archviz::massingcalculation::kEnvelopeLayer;
    EXPECT_TRUE (hud::LayerShown (*hud.state, envelope));
    hud.Click ({}, 40, lastControl);
    EXPECT_FALSE (hud::LayerShown (*hud.state, envelope));
    EXPECT_FALSE (hud::MassingCollapseZone (*hud.state));
    ASSERT_FALSE (hud.heard.empty ());
    EXPECT_EQ (hud.heard.back ().kind, "layer");
    EXPECT_EQ (hud.heard.back ().id, envelope);
    hud.engine.SetLayers ({ envelope });
    hud.Lay ({}, At (600, 600));
    EXPECT_FALSE (hud::LayerShown (*hud.state, envelope)) << "New envelope publication keeps the visibility choice";
    hud.Click ({}, 40, lastControl);
    EXPECT_TRUE (hud::LayerShown (*hud.state, envelope));
    EXPECT_TRUE (hud::TakeMassingCalculations (*hud.state).empty ());
    EXPECT_FALSE (hud::UniqueBuildings (*hud.state));
    hud.Click ({}, 40, controls[1]);
    EXPECT_TRUE (hud::UniqueBuildings (*hud.state));
    EXPECT_EQ (hud.heard.back ().kind, "uniqueBuildings");
    EXPECT_FALSE (hud::MarkLargeFloors (*hud.state));
    hud.Click ({}, 40, controls[0]);
    EXPECT_TRUE (hud::MarkLargeFloors (*hud.state));
    EXPECT_EQ (hud.heard.back ().kind, "markLargeFloors");
    EXPECT_TRUE (hud::TakeMassingCalculations (*hud.state).empty ());
    EXPECT_TRUE (hud::TakeMetadataEdits (*hud.state).empty ());
    hud.Click ({}, 40, controls[0]);
    EXPECT_FALSE (hud::MarkLargeFloors (*hud.state));
    hud.Click ({}, 40, controls[0]);
    EXPECT_TRUE (hud::MarkLargeFloors (*hud.state));
    hud::ClearState (*hud.state);
    EXPECT_FALSE (hud::MarkLargeFloors (*hud.state));
}
