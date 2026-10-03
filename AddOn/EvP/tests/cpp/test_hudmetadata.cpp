// ArchViz/HudMetadata: the Selection page's Tapioca metadata (the user, 2026-10-03). Its fields
// are the project's schema -- a dropdown for an enumeration or a classification, a slider for
// a bounded number, a toggle for a flag or an offered tag -- with the value the selected
// elements share or "mixed"; an edit is the user's authored value, written by the owner.

#include "hud_fixture.hpp"

#include "ArchViz/HudMetadata.hpp"
#include "Metadata/TapiocaMetadata.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hudtest;

namespace meta = geomsrv::metadata;
namespace hm = geomsrv::archviz::hudmeta;
namespace shell = geomsrv::archviz::hudshell;

namespace {

const hm::Field* FieldOf (const hm::Page& page, const std::string& id)
{
    for (const hm::Field& field : page.fields)
        if (field.id == id)
            return &field;
    return nullptr;
}

size_t IndexOf (const hm::Page& page, const std::string& id)
{
    for (size_t i = 0; i < page.fields.size (); ++i)
        if (page.fields[i].id == id)
            return i;
    return page.fields.size ();
}

meta::EntityMetadata WithUsage (const char* usage)
{
    meta::EntityMetadata entity;
    meta::Property property;
    property.key = "program.usage";
    property.value = meta::Value::Option (usage);
    meta::SetProperty (entity, property);
    return entity;
}

} // namespace

// ⚠️ THE USER: the editors are generated from the definitions, not written per workflow.
TEST (HudMetadata, TheSchemasDefinitionsAreTheFieldsInItsGroups)
{
    const meta::ProjectSchema schema = meta::DefaultSchema ();
    const hm::Page page = hm::Fields (schema, { WithUsage ("residential") }, 1);
    ASSERT_TRUE (page.known);
    EXPECT_EQ (page.elements, 1u);

    const hm::Field* usage = FieldOf (page, "program.usage");
    ASSERT_NE (usage, nullptr);
    EXPECT_EQ (usage->kind, hm::FieldKind::Choice);
    EXPECT_EQ (usage->group, "Program");
    EXPECT_TRUE (usage->set);
    EXPECT_FALSE (usage->mixed);
    EXPECT_EQ (usage->text, "residential");
    ASSERT_FALSE (usage->options.empty ());
    EXPECT_EQ (usage->options.front ().value, "residential");
    EXPECT_EQ (usage->options.front ().rgba, 0xF2C14EFFu) << "the zoning plan's colour";

    const hm::Field* occupancy = FieldOf (page, "program.occupancy");
    ASSERT_NE (occupancy, nullptr);
    EXPECT_EQ (occupancy->kind, hm::FieldKind::Number);
    EXPECT_TRUE (occupancy->integer);
    EXPECT_FALSE (occupancy->set);
    EXPECT_DOUBLE_EQ (occupancy->max, 2000.0) << "a bounded number is a slider";
    const hm::Field* gfa = FieldOf (page, "program.gfaTarget");
    ASSERT_NE (gfa, nullptr);
    EXPECT_EQ (gfa->unit, "m\xC2\xB2") << "the unit's symbol, not its id";

    EXPECT_LT (IndexOf (page, "program.usage"), IndexOf (page, "structure.system")) << "the schema's UI order";
    const hm::Field* program = FieldOf (page, "class:Tapioca.Program");
    ASSERT_NE (program, nullptr);
    EXPECT_EQ (program->kind, hm::FieldKind::Choice);
    const hm::Field* review = FieldOf (page, "tag:review");
    ASSERT_NE (review, nullptr);
    EXPECT_EQ (review->kind, hm::FieldKind::Toggle);
    EXPECT_FALSE (review->on);

    EXPECT_TRUE (hm::Fields (schema, {}, 0).fields.empty ()) << "nothing selected, nothing to edit";
}

// One value for every selected element: the one they share, or "mixed".
TEST (HudMetadata, AFieldTheSelectionDisagreesOnIsMixed)
{
    const meta::ProjectSchema schema = meta::DefaultSchema ();
    meta::EntityMetadata a = WithUsage ("residential"), b = WithUsage ("office"), none;
    meta::AddTag (a, "review");
    meta::Classify (a, "Tapioca.Program", "BuildingMass");
    meta::Classify (b, "Tapioca.Program", "BuildingMass");
    const hm::Page page = hm::Fields (schema, { a, b }, 5);
    EXPECT_EQ (page.selected, 5u);
    EXPECT_TRUE (FieldOf (page, "program.usage")->mixed);
    const hm::Field* program = FieldOf (page, "class:Tapioca.Program");
    EXPECT_FALSE (program->mixed);
    EXPECT_EQ (program->text, "BuildingMass");
    const hm::Field* review = FieldOf (page, "tag:review");
    EXPECT_TRUE (review->mixed);
    EXPECT_FALSE (review->on);
    EXPECT_TRUE (FieldOf (hm::Fields (schema, { a, none }, 2), "program.usage")->mixed)
        << "held by one and not the other";
}

// Shown in its unit, stored canonical: a percentage is a fraction.
TEST (HudMetadata, ANumberIsShownInItsUnit)
{
    meta::ProjectSchema schema = meta::DefaultSchema ();
    meta::PropertyDefinition share;
    share.key = "program.share";
    share.label = "Share";
    share.type = meta::ValueType::Percentage;
    share.unit = "%";
    share.max = 1.0;
    share.step = 0.01;
    schema.properties.push_back (share);
    meta::EntityMetadata entity;
    meta::Property property;
    property.key = "program.share";
    property.value = meta::Value::Number (0.25, meta::ValueType::Percentage);
    meta::SetProperty (entity, property);
    const hm::Page page = hm::Fields (schema, { entity }, 1);
    const hm::Field* field = FieldOf (page, "program.share");
    ASSERT_NE (field, nullptr);
    EXPECT_DOUBLE_EQ (field->scale, 100.0);
    EXPECT_DOUBLE_EQ (field->number, 25.0);
    EXPECT_DOUBLE_EQ (field->max, 100.0);
    EXPECT_EQ (field->unit, "%");
}

// ⚠️ AN EDIT IS THE USER'S AUTHORED VALUE: its provenance says so, and the schema takes it.
TEST (HudMetadata, AnEditIsTheUsersAuthoredValue)
{
    const meta::ProjectSchema schema = meta::DefaultSchema ();
    meta::EntityMetadata entity;
    std::string error;
    hm::Edit usage;
    usage.id = "program.usage";
    usage.kind = hm::FieldKind::Choice;
    usage.type = meta::ValueType::Enum;
    usage.text = "office";
    ASSERT_TRUE (hm::Apply (entity, usage, schema, 1234, error)) << error;
    const meta::Property* written = meta::FindProperty (entity, "program.usage");
    ASSERT_NE (written, nullptr);
    EXPECT_EQ (written->value.type, meta::ValueType::Enum);
    EXPECT_EQ (written->value.s, "office");
    EXPECT_EQ (written->state, meta::State::Authored);
    EXPECT_EQ (written->provenance.source, meta::Source::User);
    EXPECT_EQ (written->provenance.timestampMs, 1234);
    EXPECT_TRUE (meta::Validate (entity, schema).empty ());

    hm::Edit units;
    units.id = "program.units";
    units.kind = hm::FieldKind::Number;
    units.number = 11.6;
    ASSERT_TRUE (hm::Apply (entity, units, schema, 0, error)) << error;
    EXPECT_EQ (meta::FindProperty (entity, "program.units")->value.i, 12) << "an integer, rounded";

    hm::Edit program;
    program.id = "class:Tapioca.Program";
    program.text = "BuildingMass";
    ASSERT_TRUE (hm::Apply (entity, program, schema, 0, error)) << error;
    EXPECT_EQ (meta::ClassificationIn (entity, "Tapioca.Program"), "BuildingMass");

    hm::Edit review;
    review.id = "tag:review";
    review.kind = hm::FieldKind::Toggle;
    review.on = true;
    ASSERT_TRUE (hm::Apply (entity, review, schema, 0, error));
    EXPECT_TRUE (meta::HasTag (entity, "review"));
    review.on = false;
    ASSERT_TRUE (hm::Apply (entity, review, schema, 0, error));
    EXPECT_FALSE (meta::HasTag (entity, "review"));

    hm::Edit clear;
    clear.id = "program.usage";
    clear.action = hm::Edit::Action::Clear;
    ASSERT_TRUE (hm::Apply (entity, clear, schema, 0, error));
    EXPECT_EQ (meta::FindProperty (entity, "program.usage"), nullptr);

    hm::Edit ask;
    ask.id = "program.note";
    ask.action = hm::Edit::Action::AskText;
    EXPECT_FALSE (hm::Apply (entity, ask, schema, 0, error)) << "the dialog answers a text first";
}

// Through the HUD: a toggle pressed on the Selection page is an edit for the owner -- said,
// not written -- and an event for Python.
TEST (HudMetadata, ATogglePressedOnTheSelectionPageIsAnEdit)
{
    Watched hud;
    hud::OwnPages pages;
    pages.standalone = true;
    pages.selection.known = true;
    pages.selection.count = 1; // listed by none: the page is the toggle alone
    hm::Field review;
    review.id = "tag:review";
    review.label = "review";
    review.group = "Tags";
    review.kind = hm::FieldKind::Toggle;
    pages.metadata.known = true;
    pages.metadata.elements = 1;
    pages.metadata.selected = 1;
    pages.metadata.fields.push_back (review);
    hud.engine.SetOwnPages (pages);
    hud::SelectKey (*hud.state, shell::kSelectionKey);
    const hud::Layout out = hud.Lay ({}, At (600.0f, 600.0f));
    ASSERT_GT (out.host.height, 0.0f);

    // Below the tab row, the first thing the pointer shows the hand over: the toggle.
    float x = 0.0f, y = 0.0f;
    for (float down = 52.0f; down < 16.0f + out.host.height && x == 0.0f; down += 2.0f)
        for (float across = 18.0f; across < 16.0f + out.host.width; across += 2.0f)
            if (hud.Lay ({}, At (across, down)).hand) {
                x = across;
                y = down;
                break;
            }
    ASSERT_GT (x, 0.0f) << "the toggle is on the page";
    hud.Click ({}, x, y);
    const std::vector<hm::Edit> edits = hud::TakeMetadataEdits (*hud.state);
    ASSERT_EQ (edits.size (), 1u);
    EXPECT_EQ (edits[0].id, "tag:review");
    EXPECT_TRUE (edits[0].on);
    EXPECT_TRUE (hud::TakeMetadataEdits (*hud.state).empty ()) << "taken once";
    ASSERT_FALSE (hud.heard.empty ());
    EXPECT_EQ (hud.heard.back ().kind, "metadata");
    EXPECT_EQ (hud.heard.back ().id, "tag:review");
}
