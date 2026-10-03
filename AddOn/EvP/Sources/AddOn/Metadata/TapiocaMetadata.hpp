#ifndef EVP_METADATA_TAPIOCAMETADATA_HPP
#define EVP_METADATA_TAPIOCAMETADATA_HPP

// Metadata/TapiocaMetadata -- what Tapioca knows about an element that Archicad does not: a
// typed property graph, not a schema per workflow (the user, 2026-10-03).
//
// ⚠️ SIX PRIMITIVES, AND A WORKFLOW IS A SCHEMA OVER THEM. Property, Classification, Range,
// Relationship, Set and Tag carry everything from a building's programme and feasibility to
// structure, analysis results, design options and graph references. A workflow never gets a
// struct of its own: it defines keys, enumerations and classification systems in the
// project's schema (`ProjectSchema`), and the HUD's editors are generated from those
// definitions rather than hard-coded.
//
//   Property        a typed value under a namespaced key: program.usage = residential
//   Classification  what an element IS, per system: Tapioca.Program -> BuildingMass
//   Range           properties over a part of a domain: floors 1-7 -> program.usage residential
//   Relationship    a directed, typed edge to another entity: mass belongsTo parcel
//   Set             membership of a named group: "Building A", "Option 2"
//   Tag             a bare flag: existing, demolish, review
//
// ⚠️ EVERY VALUE KNOWS WHERE IT CAME FROM AND WHETHER IT WAS AUTHORED. `Provenance` says who
// wrote it (the user, a graph, Archicad, an analysis, an import); `State` separates what a
// person decided from what was computed, so a computed value can be invalidated when the
// geometry it was computed from changes, and an authored one never is.
//
// ⚠️ OPEN, NOT CLOSED. A key the schema does not define is kept and round-tripped -- the graph
// is extensible by anyone -- but a key it does define must hold its type and, for an
// enumeration, one of its options (`Validate`).
//
// ⚠️ UNITS ARE CANONICAL INSIDE: lengths in metres, areas in square metres, volumes in cubic
// metres, angles in degrees, percentages as a fraction (0.25 is 25 %). A definition's `unit`
// is how a value is shown, never how it is stored.
//
// Pure: no ACAPI, no ImGui. tests/cpp builds it. Storage on the element and in the project is
// Metadata/MetadataStorage.hpp's.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace geomsrv {
namespace metadata {

// The format's version, written with every entity and schema.
constexpr int kFormatVersion = 1;

// ---- values ----------------------------------------------------------------------------

enum class ValueType : uint8_t {
    Bool,
    Int,
    Double,
    String,
    Enum,       // an option of an enumeration, by its value
    DateTime,   // ISO 8601, as text
    Length,     // metres
    Area,       // square metres
    Volume,     // cubic metres
    Angle,      // degrees
    Percentage, // a fraction: 0.25 is 25 %
    Vector2,
    Vector3,
    Color,     // 0xRRGGBBAA
    Reference, // another entity's id or an element's GUID
    List,      // values of one `elementType`
    Object,    // named values
};
const char* TypeName (ValueType type);
bool TypeFromName (const std::string& name, ValueType& type);
// A number of some kind: Int, Double and the measured quantities.
bool IsNumber (ValueType type);

struct Value {
    ValueType type = ValueType::String;
    bool b = false;                            // Bool
    int64_t i = 0;                             // Int
    double d = 0.0;                            // Double, Length, Area, Volume, Angle, Percentage
    std::string s;                             // String, Enum, DateTime, Reference
    std::vector<double> v;                     // Vector2, Vector3
    uint32_t rgba = 0;                         // Color
    ValueType elementType = ValueType::String; // List
    std::vector<Value> list;                   // List
    std::map<std::string, Value> fields;       // Object

    static Value Boolean (bool value);
    static Value Integer (int64_t value);
    static Value Number (double value, ValueType type = ValueType::Double);
    static Value Text (std::string value, ValueType type = ValueType::String);
    static Value Option (std::string value);
    // The number a numeric value holds; 0 for anything else.
    double AsNumber () const;
};
bool operator== (const Value& a, const Value& b);
inline bool operator!= (const Value& a, const Value& b)
{
    return !(a == b);
}
// How a value reads: its number with `decimals`, its option's value, "yes"/"no"...
std::string ToText (const Value& value, int decimals = 2);

// ---- where a value came from, and what kind of value it is ------------------------------

enum class Source : uint8_t { User, Graph, Archicad, Analysis, Import };
const char* SourceName (Source source);
bool SourceFromName (const std::string& name, Source& source);

enum class State : uint8_t { Authored, Computed, Imported, Derived, Cached };
const char* StateName (State state);
bool StateFromName (const std::string& name, State& state);

struct Provenance {
    Source source = Source::User;
    std::string sourceId;    // the graph, the command, the analysis that wrote it
    int64_t timestampMs = 0; // milliseconds since 1970 UTC
    std::string author;
};

// ---- the six primitives -------------------------------------------------------------------

struct Property {
    std::string key; // namespaced: "program.usage", "analysis.sunHours.mean"
    Value value;
    std::string unit; // how it is shown, when not its definition's; storage is canonical
    State state = State::Authored;
    std::string generator; // what computed it: "SunStudy"
    Provenance provenance;
};
// "program" of "program.usage"; empty for a key without a dot.
std::string NamespaceOf (const std::string& key);

struct Classification {
    std::string system; // "Tapioca.Program", "IFC"
    std::string value;  // "BuildingMass", "IfcSlab"
    Provenance provenance;
};

// ⚠️ A RANGE, NOT A FLOOR USAGE: properties over a part of a domain -- floors, heights,
// stations along a curve, periods, phases, facade sectors, percentages. In a COUNTED domain
// (`Counted`: floors, phases) `from` and `to` are whole positions and both belong to it; in a
// continuous one the range is [from, to).
struct RangeAssignment {
    std::string domain;
    double from = 0.0;
    double to = 0.0;
    std::vector<Property> properties;
    Provenance provenance;
};
// The domains whose positions are counted, not measured.
bool Counted (const std::string& domain);
constexpr char kFloorDomain[] = "floor";

struct Relationship {
    std::string type;   // "contains", "belongsTo", "generatedBy"
    std::string target; // the other entity's id or element's GUID
    std::vector<Property> properties;
    Provenance provenance;
};

// Everything Tapioca knows about one element.
struct EntityMetadata {
    int formatVersion = kFormatVersion;
    std::string entityId;     // Tapioca's own UUID for the entity
    std::string archicadGuid; // the element it was written on: a copy of the element says another
    std::vector<Classification> classifications;
    std::vector<std::string> tags;
    std::vector<Property> properties;
    std::vector<RangeAssignment> ranges;
    std::vector<Relationship> relationships;
    std::vector<std::string> sets; // the sets it belongs to, by id
    Provenance provenance;

    bool Empty () const;
};

// ---- editing ------------------------------------------------------------------------------

// The property under `key`, or null.
const Property* FindProperty (const EntityMetadata& meta, const std::string& key);
// Set by key: an existing one is replaced. Removing returns whether there was one.
void SetProperty (EntityMetadata& meta, Property property);
bool RemoveProperty (EntityMetadata& meta, const std::string& key);

// ⚠️ EACH KEY HAS ONE VALUE AT EACH POSITION. Assigning a range takes its keys away from
// whatever part of other ranges in the same domain it covers -- splitting them -- and then
// adds it; ranges left with no property go, and neighbours that say the same merge. So floor 0
// commercial, then floors 1-7 residential, then floor 3 office reads floor 0 commercial,
// floors 1-2 and 4-7 residential, floor 3 office.
void AssignRange (EntityMetadata& meta, const RangeAssignment& assignment);
// `key` taken away from [from, to] of `domain` (a counted domain's both ends belong to it).
// Returns whether anything was.
bool ClearRange (EntityMetadata& meta, const std::string& domain, double from, double to, const std::string& key);
// The value `key` has at `position` of `domain`, and the range it comes from; null for none.
const Property* RangeValue (const EntityMetadata& meta, const std::string& domain, double position,
                            const std::string& key, const RangeAssignment** range = nullptr);

bool AddTag (EntityMetadata& meta, const std::string& tag);
bool RemoveTag (EntityMetadata& meta, const std::string& tag);
bool HasTag (const EntityMetadata& meta, const std::string& tag);
// One value per system: classifying again in a system replaces its value.
void Classify (EntityMetadata& meta, const std::string& system, const std::string& value,
               const Provenance& provenance = Provenance ());
bool Unclassify (EntityMetadata& meta, const std::string& system);
std::string ClassificationIn (const EntityMetadata& meta, const std::string& system);
// A relationship once per type and target.
void Relate (EntityMetadata& meta, Relationship relationship);
bool Unrelate (EntityMetadata& meta, const std::string& type, const std::string& target);
bool JoinSet (EntityMetadata& meta, const std::string& set);
bool LeaveSet (EntityMetadata& meta, const std::string& set);

// ⚠️ COMPUTED VALUES ARE INVALIDATED, AUTHORED ONES NEVER ARE: the geometry they were computed
// from changed. Every Computed and Cached property -- of `generator` alone when it is given --
// goes, and those of the ranges too. Returns how many went.
size_t InvalidateComputed (EntityMetadata& meta, const std::string& generator = std::string ());

// ⚠️ A COPIED ELEMENT CARRIES ITS ORIGINAL'S METADATA (the user data travels with a copy). Read
// on an element whose GUID differs from the one it was written on, the metadata is the copy's
// from now on: its GUID, and a new entity id. Returns whether it was a copy.
bool AdoptElement (EntityMetadata& meta, const std::string& elementGuid);
// A new entity id: a random version-4 UUID, upper case and dashed as Archicad writes GUIDs.
std::string NewEntityId ();

// ---- the project's schema ----------------------------------------------------------------

struct EnumOption {
    std::string value; // stored: "residential"
    std::string label; // shown: "Residential"
    uint32_t rgba = 0; // its colour in the HUD, 0xRRGGBBAA; alpha 0: none
};

struct Enumeration {
    std::string id;
    std::string label;
    std::vector<EnumOption> options;
    const EnumOption* Find (const std::string& value) const;
};

struct PropertyDefinition {
    std::string key;
    std::string label;
    ValueType type = ValueType::String;
    std::string enumId;                    // an Enum's options
    std::string unit;                      // how it is shown: "m", "m2", "%", "h"
    std::vector<std::string> applicableTo; // classification values it applies to; empty: any
    bool hasDefault = false;
    Value defaultValue;
    double min = 0.0; // a number's bounds: a slider when max > min, on `step`s (0: any)
    double max = 0.0;
    double step = 0.0;
    std::vector<std::string> domains; // the range domains it may be assigned over ("floor")
    std::string group;                // the section it is listed under in the HUD
    std::string description;
};

struct ClassificationSystem {
    std::string id;
    std::string label;
    std::vector<EnumOption> values;
};

struct RelationshipType {
    std::string id;
    std::string label;
    std::string inverse; // the same edge read from the target: "contains" <-> "containedBy"
};

struct UnitDefinition {
    std::string id;
    std::string symbol;
    std::string quantity; // "length", "area"...
    double toCanonical = 1.0;
};

struct SetDefinition {
    std::string id;
    std::string label;
    uint32_t rgba = 0;
    std::string description;
};

// A panel of the HUD: which keys it lists, in order.
struct UiSchema {
    std::string id;
    std::string title;
    std::vector<std::string> keys;
};

struct Migration {
    int from = 0;
    int to = 0;
    std::string note;
};

// ⚠️ ONE PER PROJECT, AND WORKFLOWS EXTEND IT -- they never replace it. Absent from a project,
// `DefaultSchema` is what it reads as.
struct ProjectSchema {
    int formatVersion = kFormatVersion;
    int revision = 0; // moves with every change the user makes to it
    std::vector<PropertyDefinition> properties;
    std::vector<Enumeration> enumerations;
    std::vector<ClassificationSystem> classifications;
    std::vector<RelationshipType> relationships;
    std::vector<UnitDefinition> units;
    std::vector<SetDefinition> sets;
    std::vector<UiSchema> ui;
    std::vector<std::string> tags; // the tags offered
    std::vector<Migration> migrations;

    const PropertyDefinition* Find (const std::string& key) const;
    const Enumeration* FindEnumeration (const std::string& id) const;
    const ClassificationSystem* FindSystem (const std::string& id) const;
    // The definitions that apply to an element classified as `classes` (every system's value):
    // those whose `applicableTo` is empty or names one of them, in schema order.
    std::vector<const PropertyDefinition*> ApplicableTo (const std::vector<std::string>& classes) const;
};

// The schema a project starts from: building programme (usage, occupancy, GFA target),
// structure, planning phase, analysis results, the Tapioca.Program and Tapioca.DesignOption
// classification systems, the relationship types, the units and the tags offered.
ProjectSchema DefaultSchema ();
// Definitions, enumerations, systems, relationship types, units, sets and tags of `extra`
// added to `schema` where `schema` has none of that id. Returns how many were added.
size_t Extend (ProjectSchema& schema, const ProjectSchema& extra);

// ⚠️ WHAT A VALUE GETS WRONG, IN A SENTENCE EACH: a defined key holding another type, an
// option its enumeration does not have, a number outside its bounds, a range assigned over a
// domain its definition does not allow, a range whose end is before its start. Undefined
// keys are fine. Empty when valid.
std::vector<std::string> Validate (const EntityMetadata& meta, const ProjectSchema& schema);

// ---- JSON --------------------------------------------------------------------------------

std::string ToJson (const EntityMetadata& meta);
bool FromJson (const std::string& text, EntityMetadata& meta, std::string& error);
std::string ToJson (const ProjectSchema& schema);
bool FromJson (const std::string& text, ProjectSchema& schema, std::string& error);

} // namespace metadata
} // namespace geomsrv

#endif
