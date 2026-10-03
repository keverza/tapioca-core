// Metadata/TapiocaMetadata: the typed property graph Tapioca keeps on an element -- Property,
// Classification, Range, Relationship, Set and Tag, each value with its provenance and whether it
// was authored or computed -- and the project's schema its editors are generated from. A range
// that loses a floor, a computed value that survives a geometry change or a copy that keeps its
// original's identity is a wrong programme in someone's feasibility study, so each is pinned.

#include "Metadata/TapiocaMetadata.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace meta = geomsrv::metadata;

namespace {

meta::Property Usage (const char* value)
{
    meta::Property property;
    property.key = "program.usage";
    property.value = meta::Value::Option (value);
    return property;
}

meta::RangeAssignment Floors (double from, double to, std::vector<meta::Property> properties)
{
    meta::RangeAssignment range;
    range.domain = meta::kFloorDomain;
    range.from = from;
    range.to = to;
    range.properties = std::move (properties);
    return range;
}

std::string UsageAt (const meta::EntityMetadata& entity, double floor)
{
    const meta::Property* property = meta::RangeValue (entity, meta::kFloorDomain, floor, "program.usage");
    return property != nullptr ? property->value.s : std::string ("-");
}

} // namespace

// ⚠️ THE USER'S EXAMPLE: floor 0 commerce, floors 1-7 residential, floors 8-9 office. A floor
// assigned again takes its key from the range it was in, splitting it; assigned back, the
// neighbours that say the same merge.
TEST (TapiocaMetadata, AFloorRangeSplitsWhatItCoversAndMergesBack)
{
    meta::EntityMetadata entity;
    meta::AssignRange (entity, Floors (0, 0, { Usage ("commercial") }));
    meta::AssignRange (entity, Floors (1, 7, { Usage ("residential") }));
    meta::AssignRange (entity, Floors (8, 9, { Usage ("office") }));
    EXPECT_EQ (entity.ranges.size (), 3u);
    EXPECT_EQ (UsageAt (entity, 0), "commercial");
    EXPECT_EQ (UsageAt (entity, 1), "residential");
    EXPECT_EQ (UsageAt (entity, 7), "residential");
    EXPECT_EQ (UsageAt (entity, 8), "office");
    EXPECT_EQ (UsageAt (entity, 10), "-");

    meta::AssignRange (entity, Floors (3, 3, { Usage ("office") }));
    EXPECT_EQ (UsageAt (entity, 2), "residential");
    EXPECT_EQ (UsageAt (entity, 3), "office");
    EXPECT_EQ (UsageAt (entity, 4), "residential");
    EXPECT_EQ (entity.ranges.size (), 5u) << "1-2 and 4-7 residential, 3 office, beside 0 and 8-9";

    meta::AssignRange (entity, Floors (3, 3, { Usage ("residential") }));
    EXPECT_EQ (entity.ranges.size (), 3u) << "1-7 residential again, one range";
    const meta::RangeAssignment* range = nullptr;
    meta::RangeValue (entity, meta::kFloorDomain, 5, "program.usage", &range);
    ASSERT_NE (range, nullptr);
    EXPECT_EQ (range->from, 1.0);
    EXPECT_EQ (range->to, 7.0);
}

// Each key has one value at each position: a range of two keys keeps the one not assigned again.
TEST (TapiocaMetadata, ARangeOfTwoKeysKeepsTheOneNotAssignedAgain)
{
    meta::EntityMetadata entity;
    meta::Property occupancy;
    occupancy.key = "program.occupancy";
    occupancy.value = meta::Value::Integer (40);
    meta::AssignRange (entity, Floors (1, 3, { Usage ("residential"), occupancy }));
    meta::AssignRange (entity, Floors (2, 2, { Usage ("office") }));
    EXPECT_EQ (UsageAt (entity, 2), "office");
    const meta::Property* kept = meta::RangeValue (entity, meta::kFloorDomain, 2, "program.occupancy");
    ASSERT_NE (kept, nullptr);
    EXPECT_EQ (kept->value.i, 40);
    EXPECT_TRUE (meta::ClearRange (entity, meta::kFloorDomain, 2, 2, "program.usage"));
    EXPECT_EQ (UsageAt (entity, 2), "-");
    EXPECT_NE (meta::RangeValue (entity, meta::kFloorDomain, 2, "program.occupancy"), nullptr);
    EXPECT_FALSE (meta::ClearRange (entity, meta::kFloorDomain, 9, 9, "program.usage")) << "nothing there";
}

// A measured domain is half-open: the position where one range ends is the next one's.
TEST (TapiocaMetadata, AHeightRangeIsHalfOpen)
{
    meta::EntityMetadata entity;
    meta::RangeAssignment low;
    low.domain = "height";
    low.from = 0.0;
    low.to = 3.2;
    low.properties = { Usage ("commercial") };
    meta::RangeAssignment high = low;
    high.from = 3.2;
    high.to = 30.0;
    high.properties = { Usage ("residential") };
    meta::AssignRange (entity, low);
    meta::AssignRange (entity, high);
    EXPECT_EQ (meta::RangeValue (entity, "height", 3.2, "program.usage")->value.s, "residential");
    EXPECT_EQ (meta::RangeValue (entity, "height", 3.19, "program.usage")->value.s, "commercial");
    EXPECT_EQ (meta::RangeValue (entity, "height", 30.0, "program.usage"), nullptr);
}

// Every primitive and every kind of value comes back from JSON as it went in.
TEST (TapiocaMetadata, EverythingRoundTripsThroughJson)
{
    meta::EntityMetadata entity;
    entity.entityId = meta::NewEntityId ();
    entity.archicadGuid = "0D3B5C8E-1A2B-4C3D-9E8F-0123456789AB";
    meta::Classify (entity, "Tapioca.Program", "BuildingMass");
    meta::AddTag (entity, "review");
    meta::JoinSet (entity, "building-a");
    meta::Property gfa;
    gfa.key = "program.gfaTarget";
    gfa.value = meta::Value::Number (2633.25, meta::ValueType::Area);
    gfa.provenance.source = meta::Source::Graph;
    gfa.provenance.sourceId = "CostStudy_v4";
    gfa.provenance.timestampMs = 1790000000000;
    meta::SetProperty (entity, gfa);
    meta::Property sun;
    sun.key = "analysis.sunHours.mean";
    sun.value = meta::Value::Number (4.72);
    sun.state = meta::State::Computed;
    sun.generator = "SunStudy";
    meta::SetProperty (entity, sun);
    meta::Property colour;
    colour.key = "display.colour";
    colour.value.type = meta::ValueType::Color;
    colour.value.rgba = 0xF2C14EFFu;
    meta::SetProperty (entity, colour);
    meta::Property axis;
    axis.key = "geometry.axis";
    axis.value.type = meta::ValueType::Vector3;
    axis.value.v = { 0.0, 0.0, 1.0 };
    meta::SetProperty (entity, axis);
    meta::Property list;
    list.key = "notes.keywords";
    list.value.type = meta::ValueType::List;
    list.value.elementType = meta::ValueType::String;
    list.value.list = { meta::Value::Text ("podium"), meta::Value::Text ("tower") };
    meta::SetProperty (entity, list);
    meta::Property object;
    object.key = "cost.breakdown";
    object.value.type = meta::ValueType::Object;
    object.value.fields["shell"] = meta::Value::Number (0.25, meta::ValueType::Percentage);
    object.value.fields["floors"] = meta::Value::Integer (9);
    meta::SetProperty (entity, object);
    meta::AssignRange (entity, Floors (1, 7, { Usage ("residential") }));
    meta::Relationship parcel;
    parcel.type = "belongsTo";
    parcel.target = "PARCEL-7";
    meta::Relate (entity, parcel);

    const std::string text = meta::ToJson (entity);
    meta::EntityMetadata back;
    std::string error;
    ASSERT_TRUE (meta::FromJson (text, back, error)) << error;
    EXPECT_EQ (back.entityId, entity.entityId);
    EXPECT_EQ (back.archicadGuid, entity.archicadGuid);
    EXPECT_EQ (meta::ClassificationIn (back, "Tapioca.Program"), "BuildingMass");
    EXPECT_TRUE (meta::HasTag (back, "review"));
    ASSERT_EQ (back.sets.size (), 1u);
    ASSERT_EQ (back.properties.size (), entity.properties.size ());
    for (const meta::Property& property : entity.properties) {
        const meta::Property* found = meta::FindProperty (back, property.key);
        ASSERT_NE (found, nullptr) << property.key;
        EXPECT_EQ (found->value, property.value) << property.key;
        EXPECT_EQ (found->state, property.state) << property.key;
        EXPECT_EQ (found->generator, property.generator) << property.key;
        EXPECT_EQ (found->provenance.sourceId, property.provenance.sourceId) << property.key;
        EXPECT_EQ (found->provenance.timestampMs, property.provenance.timestampMs) << property.key;
    }
    EXPECT_EQ (UsageAt (back, 4), "residential");
    ASSERT_EQ (back.relationships.size (), 1u);
    EXPECT_EQ (back.relationships[0].target, "PARCEL-7");
    EXPECT_EQ (meta::ToJson (back), text) << "written again, the same";
}

// A document from a newer add-on, or not JSON, is refused with a reason, never half-read.
TEST (TapiocaMetadata, AnUnreadableDocumentIsRefusedWithAReason)
{
    meta::EntityMetadata entity;
    std::string error;
    EXPECT_FALSE (meta::FromJson ("{\"format\":99}", entity, error));
    EXPECT_NE (error.find ("newer"), std::string::npos) << error;
    EXPECT_FALSE (meta::FromJson ("not json", entity, error));
    EXPECT_FALSE (meta::FromJson ("{\"properties\":[{\"key\":\"a\",\"value\":{\"type\":\"nonsense\",\"value\":1}}]}",
                                  entity, error));
    EXPECT_NE (error.find ("nonsense"), std::string::npos) << error;
}

// ⚠️ OPEN, NOT CLOSED: an undefined key is kept; a defined one holds its type, its options, its
// bounds and its domains -- each refusal a sentence.
TEST (TapiocaMetadata, ValidationNamesWhatAValueGetsWrong)
{
    const meta::ProjectSchema schema = meta::DefaultSchema ();
    meta::EntityMetadata entity;
    meta::Property free;
    free.key = "studio.anything";
    free.value = meta::Value::Text ("kept");
    meta::SetProperty (entity, free);
    EXPECT_TRUE (meta::Validate (entity, schema).empty ());

    meta::Property wrongType;
    wrongType.key = "program.usage";
    wrongType.value = meta::Value::Text ("residential");
    meta::SetProperty (entity, wrongType);
    meta::Property occupancy;
    occupancy.key = "program.occupancy";
    occupancy.value = meta::Value::Integer (50000);
    meta::SetProperty (entity, occupancy);
    meta::AssignRange (entity, Floors (0, 1, { Usage ("spaceport") }));
    meta::Property gfa;
    gfa.key = "program.gfaTarget";
    gfa.value = meta::Value::Number (100.0, meta::ValueType::Area);
    meta::AssignRange (entity, Floors (2, 2, { gfa }));
    meta::Classify (entity, "Tapioca.Program", "Castle");
    const std::vector<std::string> problems = meta::Validate (entity, schema);
    const auto said = [&] (const char* words) {
        return std::any_of (problems.begin (), problems.end (),
                            [&] (const std::string& line) { return line.find (words) != std::string::npos; });
    };
    EXPECT_TRUE (said ("program.usage holds a string")) << problems.size ();
    EXPECT_TRUE (said ("outside 0.00 to 2000.00"));
    EXPECT_TRUE (said ("\"spaceport\", not an option of building-usage"));
    EXPECT_TRUE (said ("program.gfaTarget is not assigned over floor ranges"));
    EXPECT_TRUE (said ("\"Castle\", not one of its values"));
}

// ⚠️ THE GEOMETRY CHANGED: computed values go -- of one generator when it is named -- and what a
// person decided stays.
TEST (TapiocaMetadata, AGeometryChangeInvalidatesWhatWasComputedOnly)
{
    meta::EntityMetadata entity;
    meta::Property sun;
    sun.key = "analysis.sunHours.mean";
    sun.value = meta::Value::Number (4.72);
    sun.state = meta::State::Computed;
    sun.generator = "SunStudy";
    meta::SetProperty (entity, sun);
    meta::Property cost = sun;
    cost.key = "cost.estimate";
    cost.generator = "CostStudy";
    meta::SetProperty (entity, cost);
    meta::SetProperty (entity, Usage ("residential"));
    meta::Property floorSun = sun;
    meta::AssignRange (entity, Floors (0, 2, { floorSun }));
    EXPECT_EQ (meta::InvalidateComputed (entity, "SunStudy"), 2u);
    EXPECT_EQ (meta::FindProperty (entity, "analysis.sunHours.mean"), nullptr);
    EXPECT_NE (meta::FindProperty (entity, "cost.estimate"), nullptr) << "another generator's";
    EXPECT_TRUE (entity.ranges.empty ()) << "a range left with nothing goes";
    EXPECT_EQ (meta::InvalidateComputed (entity), 1u);
    EXPECT_NE (meta::FindProperty (entity, "program.usage"), nullptr) << "authored: never";
}

// ⚠️ A COPIED ELEMENT CARRIES ITS ORIGINAL'S METADATA: read on another GUID it becomes the
// copy's, with an identity of its own; read on its own GUID nothing changes.
TEST (TapiocaMetadata, ACopyBecomesAnEntityOfItsOwn)
{
    meta::EntityMetadata entity;
    EXPECT_FALSE (meta::AdoptElement (entity, "A"));
    const std::string original = entity.entityId;
    EXPECT_EQ (original.size (), 36u);
    EXPECT_EQ (original[14], '4') << "a version-4 UUID";
    EXPECT_FALSE (meta::AdoptElement (entity, "A"));
    EXPECT_EQ (entity.entityId, original);
    EXPECT_TRUE (meta::AdoptElement (entity, "B"));
    EXPECT_EQ (entity.archicadGuid, "B");
    EXPECT_NE (entity.entityId, original);
}

// The schema comes back from JSON whole, and extending it adds only what it lacks.
TEST (TapiocaMetadata, TheSchemaRoundTripsAndExtendsWithoutReplacing)
{
    const meta::ProjectSchema schema = meta::DefaultSchema ();
    ASSERT_NE (schema.Find ("program.usage"), nullptr);
    ASSERT_NE (schema.FindEnumeration ("building-usage"), nullptr);
    EXPECT_NE (schema.FindEnumeration ("building-usage")->Find ("residential"), nullptr);
    meta::ProjectSchema back;
    std::string error;
    ASSERT_TRUE (meta::FromJson (meta::ToJson (schema), back, error)) << error;
    EXPECT_EQ (back.properties.size (), schema.properties.size ());
    EXPECT_EQ (back.enumerations.size (), schema.enumerations.size ());
    EXPECT_EQ (back.classifications.size (), schema.classifications.size ());
    EXPECT_EQ (back.relationships.size (), schema.relationships.size ());
    EXPECT_EQ (back.tags, schema.tags);
    const meta::PropertyDefinition* usage = back.Find ("program.usage");
    ASSERT_NE (usage, nullptr);
    EXPECT_EQ (usage->type, meta::ValueType::Enum);
    EXPECT_EQ (usage->domains, std::vector<std::string> ({ "floor" }));
    EXPECT_EQ (back.FindEnumeration ("building-usage")->Find ("residential")->rgba,
               schema.FindEnumeration ("building-usage")->Find ("residential")->rgba);
    EXPECT_EQ (meta::ToJson (back), meta::ToJson (schema));

    meta::ProjectSchema extra;
    extra.properties.push_back (*schema.Find ("program.usage")); // known: not added
    meta::PropertyDefinition fire;
    fire.key = "safety.fireCompartment";
    fire.label = "Fire compartment";
    fire.type = meta::ValueType::String;
    extra.properties.push_back (fire);
    extra.tags = { "review", "survey" };
    meta::ProjectSchema extended = schema;
    EXPECT_EQ (meta::Extend (extended, extra), 2u);
    EXPECT_NE (extended.Find ("safety.fireCompartment"), nullptr);
    EXPECT_EQ (extended.properties.size (), schema.properties.size () + 1);
}

// The definitions an element is offered follow what it is classified as.
TEST (TapiocaMetadata, TheDefinitionsOfferedFollowTheClassification)
{
    meta::ProjectSchema schema = meta::DefaultSchema ();
    meta::PropertyDefinition parking;
    parking.key = "program.parkingSpaces";
    parking.type = meta::ValueType::Int;
    parking.applicableTo = { "BuildingMass" };
    schema.properties.push_back (parking);
    const auto offered = [&] (const std::vector<std::string>& classes) {
        const std::vector<const meta::PropertyDefinition*> list = schema.ApplicableTo (classes);
        return std::any_of (list.begin (), list.end (),
                            [] (const meta::PropertyDefinition* d) { return d->key == "program.parkingSpaces"; });
    };
    EXPECT_TRUE (offered ({ "BuildingMass" }));
    EXPECT_FALSE (offered ({ "Zone" }));
    EXPECT_FALSE (offered ({}));
}

// Tags, classifications, relationships and sets are each held once.
TEST (TapiocaMetadata, EachPrimitiveIsHeldOnce)
{
    meta::EntityMetadata entity;
    EXPECT_TRUE (meta::AddTag (entity, "locked"));
    EXPECT_FALSE (meta::AddTag (entity, "locked"));
    meta::Classify (entity, "Tapioca.DesignOption", "OptionA");
    meta::Classify (entity, "Tapioca.DesignOption", "OptionB");
    EXPECT_EQ (entity.classifications.size (), 1u);
    EXPECT_EQ (meta::ClassificationIn (entity, "Tapioca.DesignOption"), "OptionB");
    meta::Relationship relationship;
    relationship.type = "contains";
    relationship.target = "Z1";
    meta::Relate (entity, relationship);
    meta::Relate (entity, relationship);
    EXPECT_EQ (entity.relationships.size (), 1u);
    EXPECT_TRUE (meta::JoinSet (entity, "option-2"));
    EXPECT_FALSE (meta::JoinSet (entity, "option-2"));
    EXPECT_TRUE (meta::LeaveSet (entity, "option-2"));
    EXPECT_TRUE (meta::Unrelate (entity, "contains", "Z1"));
    EXPECT_TRUE (meta::Unclassify (entity, "Tapioca.DesignOption"));
    EXPECT_TRUE (meta::RemoveTag (entity, "locked"));
    EXPECT_TRUE (entity.Empty ());
}
