// Metadata/TapiocaSchema -- the project's schema (TapiocaMetadata.hpp `ProjectSchema`): its
// lookups, the schema a project starts from, extending one, and validating an entity against it.

#include "Metadata/TapiocaMetadata.hpp"
#include "Metadata/TapiocaMetadataDetail.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace geomsrv {
namespace metadata {

using detail::FromHex;
using detail::Hex;
using detail::Measured;
using detail::Number;
using detail::Textual;

// ---- the schema -------------------------------------------------------------------------------

const EnumOption* Enumeration::Find (const std::string& value) const
{
    for (const EnumOption& option : options)
        if (option.value == value)
            return &option;
    return nullptr;
}

const PropertyDefinition* ProjectSchema::Find (const std::string& key) const
{
    for (const PropertyDefinition& definition : properties)
        if (definition.key == key)
            return &definition;
    return nullptr;
}

const Enumeration* ProjectSchema::FindEnumeration (const std::string& id) const
{
    for (const Enumeration& enumeration : enumerations)
        if (enumeration.id == id)
            return &enumeration;
    return nullptr;
}

const ClassificationSystem* ProjectSchema::FindSystem (const std::string& id) const
{
    for (const ClassificationSystem& system : classifications)
        if (system.id == id)
            return &system;
    return nullptr;
}

std::vector<const PropertyDefinition*> ProjectSchema::ApplicableTo (const std::vector<std::string>& classes) const
{
    std::vector<const PropertyDefinition*> out;
    for (const PropertyDefinition& definition : properties) {
        bool applies = definition.applicableTo.empty ();
        for (const std::string& name : definition.applicableTo)
            applies = applies || std::find (classes.begin (), classes.end (), name) != classes.end ();
        if (applies)
            out.push_back (&definition);
    }
    return out;
}

namespace {

PropertyDefinition Definition (const char* key, const char* label, ValueType type, const char* group,
                               const char* unit = "")
{
    PropertyDefinition definition;
    definition.key = key;
    definition.label = label;
    definition.type = type;
    definition.group = group;
    definition.unit = unit;
    return definition;
}

} // namespace

ProjectSchema DefaultSchema ()
{
    ProjectSchema schema;
    // ⚠️ THE PROGRAMME'S USES, IN THE COLOURS A ZONING PLAN USES FOR THEM: residential yellow,
    // commerce red, office blue, parking grey -- read at a glance on the building section.
    Enumeration usage;
    usage.id = "building-usage";
    usage.label = "Usage";
    usage.options = {
        { "residential", "Residential", 0xF2C14EFFu }, { "commercial", "Commerce", 0xE4572EFFu },
        { "office", "Office", 0x4A90D9FFu },           { "hotel", "Hotel", 0xB36AE2FFu },
        { "education", "Education", 0xF29E4CFFu },     { "healthcare", "Healthcare", 0x5FBF77FFu },
        { "industrial", "Industrial", 0x8E6C8AFFu },   { "parking", "Parking", 0x9AA0A6FFu },
        { "technical", "Technical", 0x6C757DFFu },     { "mixed", "Mixed use", 0xD98BA6FFu },
        { "vacant", "Vacant", 0xDADCE0FFu },
    };
    schema.enumerations.push_back (usage);
    Enumeration structure;
    structure.id = "structure-system";
    structure.label = "Structural system";
    structure.options = { { "concrete", "Concrete", 0xA7A9ACFFu },
                          { "steel", "Steel", 0x5B7DB1FFu },
                          { "timber", "Timber", 0xC08A4BFFu },
                          { "masonry", "Masonry", 0xB5654AFFu },
                          { "hybrid", "Hybrid", 0x7F8C8DFFu } };
    schema.enumerations.push_back (structure);
    Enumeration phase;
    phase.id = "planning-phase";
    phase.label = "Phase";
    phase.options = { { "existing", "Existing", 0x9AA0A6FFu },
                      { "new", "New", 0x4A90D9FFu },
                      { "demolish", "Demolish", 0xE5484DFFu },
                      { "temporary", "Temporary", 0xF29E4CFFu } };
    schema.enumerations.push_back (phase);

    PropertyDefinition usageKey = Definition ("program.usage", "Usage", ValueType::Enum, "Program");
    usageKey.enumId = "building-usage";
    usageKey.domains = { kFloorDomain };
    usageKey.description = "What a building block -- or a floor of it -- is used for.";
    schema.properties.push_back (usageKey);
    PropertyDefinition occupancy = Definition ("program.occupancy", "Occupancy", ValueType::Int, "Program", "people");
    occupancy.min = 0.0;
    occupancy.max = 2000.0;
    occupancy.step = 1.0;
    occupancy.domains = { kFloorDomain };
    schema.properties.push_back (occupancy);
    PropertyDefinition units = Definition ("program.units", "Units", ValueType::Int, "Program");
    units.min = 0.0;
    units.max = 200.0;
    units.step = 1.0;
    units.domains = { kFloorDomain };
    schema.properties.push_back (units);
    PropertyDefinition gfa = Definition ("program.gfaTarget", "GFA target", ValueType::Area, "Program", "m2");
    gfa.min = 0.0;
    gfa.max = 100000.0;
    gfa.step = 10.0;
    schema.properties.push_back (gfa);
    PropertyDefinition system = Definition ("structure.system", "Structure", ValueType::Enum, "Structure");
    system.enumId = "structure-system";
    schema.properties.push_back (system);
    PropertyDefinition phaseKey = Definition ("planning.phase", "Phase", ValueType::Enum, "Planning");
    phaseKey.enumId = "planning-phase";
    schema.properties.push_back (phaseKey);
    PropertyDefinition rate = Definition ("cost.rate", "Cost rate", ValueType::Double, "Cost", "per m2");
    rate.min = 0.0;
    rate.max = 20000.0;
    rate.step = 10.0;
    schema.properties.push_back (rate);
    PropertyDefinition sun =
        Definition ("analysis.sunHours.mean", "Sun hours (mean)", ValueType::Double, "Analysis", "h");
    sun.description = "Computed by a sun study; invalidated when the geometry changes.";
    schema.properties.push_back (sun);

    ClassificationSystem program;
    program.id = "Tapioca.Program";
    program.label = "Programme element";
    program.values = { { "BuildingMass", "Building mass" },
                       { "Zone", "Zone" },
                       { "Floor", "Floor" },
                       { "Unit", "Unit" },
                       { "Core", "Core" },
                       { "Circulation", "Circulation" },
                       { "Plot", "Plot" } };
    schema.classifications.push_back (program);
    ClassificationSystem option;
    option.id = "Tapioca.DesignOption";
    option.label = "Design option";
    option.values = { { "OptionA", "Option A" }, { "OptionB", "Option B" }, { "OptionC", "Option C" } };
    schema.classifications.push_back (option);

    schema.relationships = { { "contains", "Contains", "containedBy" },
                             { "containedBy", "Contained by", "contains" },
                             { "hosts", "Hosts", "hostedBy" },
                             { "hostedBy", "Hosted by", "hosts" },
                             { "belongsTo", "Belongs to", "has" },
                             { "has", "Has", "belongsTo" },
                             { "generatedBy", "Generated by", "generates" },
                             { "generates", "Generates", "generatedBy" },
                             { "supportedBy", "Supported by", "supports" },
                             { "supports", "Supports", "supportedBy" },
                             { "adjacentTo", "Adjacent to", "adjacentTo" },
                             { "references", "References", "" } };
    schema.units = { { "m", "m", "length", 1.0 },         { "mm", "mm", "length", 0.001 },
                     { "m2", "m\xC2\xB2", "area", 1.0 },  { "m3", "m\xC2\xB3", "volume", 1.0 },
                     { "deg", "\xC2\xB0", "angle", 1.0 }, { "%", "%", "percentage", 0.01 },
                     { "h", "h", "time", 1.0 },           { "people", "people", "count", 1.0 } };
    schema.tags = { "existing", "demolish", "temporary", "locked", "review", "exclude-analysis", "reference-only" };
    schema.ui = {
        { "program", "Program", { "program.usage", "program.occupancy", "program.units", "program.gfaTarget" } },
        { "structure", "Structure", { "structure.system" } },
        { "planning", "Planning", { "planning.phase", "cost.rate" } },
        { "analysis", "Analysis", { "analysis.sunHours.mean" } }
    };
    return schema;
}

size_t Extend (ProjectSchema& schema, const ProjectSchema& extra)
{
    size_t added = 0;
    for (const PropertyDefinition& definition : extra.properties)
        if (schema.Find (definition.key) == nullptr) {
            schema.properties.push_back (definition);
            ++added;
        }
    for (const Enumeration& enumeration : extra.enumerations)
        if (schema.FindEnumeration (enumeration.id) == nullptr) {
            schema.enumerations.push_back (enumeration);
            ++added;
        }
    for (const ClassificationSystem& system : extra.classifications)
        if (schema.FindSystem (system.id) == nullptr) {
            schema.classifications.push_back (system);
            ++added;
        }
    const auto addById = [&] (auto& into, const auto& from) {
        for (const auto& item : from) {
            const bool known =
                std::any_of (into.begin (), into.end (), [&] (const auto& existing) { return existing.id == item.id; });
            if (!known) {
                into.push_back (item);
                ++added;
            }
        }
    };
    addById (schema.relationships, extra.relationships);
    addById (schema.units, extra.units);
    addById (schema.sets, extra.sets);
    addById (schema.ui, extra.ui);
    for (const std::string& tag : extra.tags)
        if (std::find (schema.tags.begin (), schema.tags.end (), tag) == schema.tags.end ()) {
            schema.tags.push_back (tag);
            ++added;
        }
    return added;
}

// ---- validation ---------------------------------------------------------------------------------

namespace {

void CheckValue (const Property& property, const ProjectSchema& schema, const std::string& where,
                 std::vector<std::string>& problems)
{
    const PropertyDefinition* definition = schema.Find (property.key);
    if (definition == nullptr)
        return;
    const std::string at = where + property.key;
    if (property.value.type != definition->type) {
        problems.push_back (at + " holds a " + TypeName (property.value.type) + "; its definition says " +
                            TypeName (definition->type));
        return;
    }
    if (definition->type == ValueType::Enum) {
        const Enumeration* enumeration = schema.FindEnumeration (definition->enumId);
        if (enumeration != nullptr && enumeration->Find (property.value.s) == nullptr)
            problems.push_back (at + " is \"" + property.value.s + "\", not an option of " + enumeration->id);
    }
    if (IsNumber (definition->type) && definition->max > definition->min) {
        const double x = property.value.AsNumber ();
        if (x < definition->min || x > definition->max)
            problems.push_back (at + " is " + ToText (property.value) + ", outside " + Number (definition->min, 2) +
                                " to " + Number (definition->max, 2));
    }
}

} // namespace

std::vector<std::string> Validate (const EntityMetadata& meta, const ProjectSchema& schema)
{
    std::vector<std::string> problems;
    std::set<std::string> keys;
    for (const Property& property : meta.properties) {
        if (!keys.insert (property.key).second)
            problems.push_back (property.key + " is set twice");
        CheckValue (property, schema, std::string (), problems);
    }
    for (const RangeAssignment& range : meta.ranges) {
        const std::string where = range.domain + " " + Number (range.from, Counted (range.domain) ? 0 : 2) + "-" +
                                  Number (range.to, Counted (range.domain) ? 0 : 2) + ": ";
        if (range.from > range.to)
            problems.push_back (where + "its end is before its start");
        for (const Property& property : range.properties) {
            CheckValue (property, schema, where, problems);
            // A definition names the domains it may be assigned over; none, it is the element's
            // as a whole and is not assigned over a range at all.
            const PropertyDefinition* definition = schema.Find (property.key);
            if (definition != nullptr && std::find (definition->domains.begin (), definition->domains.end (),
                                                    range.domain) == definition->domains.end ())
                problems.push_back (where + property.key + " is not assigned over " + range.domain + " ranges");
        }
    }
    for (const Classification& classification : meta.classifications) {
        const ClassificationSystem* system = schema.FindSystem (classification.system);
        if (system == nullptr || system->values.empty ())
            continue;
        const bool known = std::any_of (system->values.begin (), system->values.end (), [&] (const EnumOption& option) {
            return option.value == classification.value;
        });
        if (!known)
            problems.push_back ("classification " + classification.system + " is \"" + classification.value +
                                "\", not one of its values");
    }
    return problems;
}

} // namespace metadata
} // namespace geomsrv
