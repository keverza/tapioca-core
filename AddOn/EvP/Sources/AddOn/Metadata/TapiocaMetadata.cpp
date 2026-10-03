// Metadata/TapiocaMetadata -- see the header: the values, their names and the editing.
// The schema is TapiocaSchema.cpp's, the JSON TapiocaMetadataJson.cpp's.

#include "Metadata/TapiocaMetadata.hpp"
#include "Metadata/TapiocaMetadataDetail.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <set>
#include <utility>

namespace geomsrv {
namespace metadata {

using detail::FromHex;
using detail::Hex;
using detail::Measured;
using detail::Number;
using detail::Textual;

namespace {

struct TypeEntry {
    ValueType type;
    const char* name;
};
constexpr TypeEntry kTypes[] = {
    { ValueType::Bool, "bool" },
    { ValueType::Int, "int" },
    { ValueType::Double, "double" },
    { ValueType::String, "string" },
    { ValueType::Enum, "enum" },
    { ValueType::DateTime, "datetime" },
    { ValueType::Length, "length" },
    { ValueType::Area, "area" },
    { ValueType::Volume, "volume" },
    { ValueType::Angle, "angle" },
    { ValueType::Percentage, "percentage" },
    { ValueType::Vector2, "vector2" },
    { ValueType::Vector3, "vector3" },
    { ValueType::Color, "color" },
    { ValueType::Reference, "reference" },
    { ValueType::List, "list" },
    { ValueType::Object, "object" },
};
constexpr const char* kSources[] = { "user", "graph", "archicad", "analysis", "import" };
constexpr const char* kStates[] = { "authored", "computed", "imported", "derived", "cached" };

// ---- ranges ------------------------------------------------------------------------------

// Whether position `at` lies in `range` (a counted domain's ends both belong to it).
bool Contains (const RangeAssignment& range, double at, bool counted)
{
    return counted ? at >= range.from && at <= range.to : at >= range.from && at < range.to;
}

bool Overlaps (const RangeAssignment& range, double from, double to, bool counted)
{
    return counted ? range.from <= to && range.to >= from : range.from < to && range.to > from;
}

bool HasExtent (double from, double to, bool counted)
{
    return counted ? from <= to : from < to;
}

bool SameProperties (const std::vector<Property>& a, const std::vector<Property>& b)
{
    if (a.size () != b.size ())
        return false;
    for (const Property& p : a) {
        const auto found = std::find_if (b.begin (), b.end (), [&] (const Property& q) { return q.key == p.key; });
        if (found == b.end () || found->value != p.value || found->state != p.state || found->unit != p.unit)
            return false;
    }
    return true;
}

// `key` taken away from [from, to] of `domain`, splitting what it covers.
bool ClearKey (EntityMetadata& meta, const std::string& domain, double from, double to, const std::string& key)
{
    const bool counted = Counted (domain);
    bool changed = false;
    std::vector<RangeAssignment> out;
    for (const RangeAssignment& range : meta.ranges) {
        const auto holds = std::find_if (range.properties.begin (), range.properties.end (),
                                         [&] (const Property& p) { return p.key == key; });
        if (range.domain != domain || holds == range.properties.end () || !Overlaps (range, from, to, counted)) {
            out.push_back (range);
            continue;
        }
        changed = true;
        // Before the cleared part, as it was.
        const double step = counted ? 1.0 : 0.0;
        if (HasExtent (range.from, from - step, counted)) {
            RangeAssignment before = range;
            before.to = from - step;
            out.push_back (before);
        }
        // The cleared part, without the key.
        RangeAssignment middle = range;
        middle.from = (std::max) (range.from, from);
        middle.to = (std::min) (range.to, to);
        middle.properties.erase (std::remove_if (middle.properties.begin (), middle.properties.end (),
                                                 [&] (const Property& p) { return p.key == key; }),
                                 middle.properties.end ());
        if (!middle.properties.empty () && HasExtent (middle.from, middle.to, counted))
            out.push_back (middle);
        // After it, as it was.
        if (HasExtent (to + step, range.to, counted)) {
            RangeAssignment after = range;
            after.from = to + step;
            out.push_back (after);
        }
    }
    meta.ranges = std::move (out);
    return changed;
}

// The domain's ranges in order, empty ones gone, neighbours that say the same merged.
void Normalize (EntityMetadata& meta, const std::string& domain)
{
    const bool counted = Counted (domain);
    std::vector<RangeAssignment> mine, others;
    for (RangeAssignment& range : meta.ranges) {
        if (range.domain != domain)
            others.push_back (std::move (range));
        else if (!range.properties.empty () && HasExtent (range.from, range.to, counted))
            mine.push_back (std::move (range));
    }
    std::stable_sort (mine.begin (), mine.end (),
                      [] (const RangeAssignment& a, const RangeAssignment& b) { return a.from < b.from; });
    std::vector<RangeAssignment> merged;
    for (RangeAssignment& range : mine) {
        if (!merged.empty ()) {
            RangeAssignment& last = merged.back ();
            const bool adjacent = counted ? last.to + 1.0 == range.from : last.to == range.from;
            if (adjacent && SameProperties (last.properties, range.properties)) {
                last.to = range.to;
                last.provenance = range.provenance;
                continue;
            }
        }
        merged.push_back (std::move (range));
    }
    meta.ranges = std::move (others);
    meta.ranges.insert (meta.ranges.end (), merged.begin (), merged.end ());
}
} // namespace

// ---- names ----------------------------------------------------------------------------------

const char* TypeName (ValueType type)
{
    for (const TypeEntry& entry : kTypes)
        if (entry.type == type)
            return entry.name;
    return "string";
}

bool TypeFromName (const std::string& name, ValueType& type)
{
    for (const TypeEntry& entry : kTypes)
        if (name == entry.name) {
            type = entry.type;
            return true;
        }
    return false;
}

bool IsNumber (ValueType type)
{
    return type == ValueType::Int || Measured (type);
}

const char* SourceName (Source source)
{
    return kSources[size_t (source) < 5 ? size_t (source) : 0];
}

bool SourceFromName (const std::string& name, Source& source)
{
    for (size_t i = 0; i < 5; ++i)
        if (name == kSources[i]) {
            source = Source (i);
            return true;
        }
    return false;
}

const char* StateName (State state)
{
    return kStates[size_t (state) < 5 ? size_t (state) : 0];
}

bool StateFromName (const std::string& name, State& state)
{
    for (size_t i = 0; i < 5; ++i)
        if (name == kStates[i]) {
            state = State (i);
            return true;
        }
    return false;
}

// ---- values ---------------------------------------------------------------------------------

Value Value::Boolean (bool value)
{
    Value out;
    out.type = ValueType::Bool;
    out.b = value;
    return out;
}

Value Value::Integer (int64_t value)
{
    Value out;
    out.type = ValueType::Int;
    out.i = value;
    return out;
}

Value Value::Number (double value, ValueType type)
{
    Value out;
    out.type = Measured (type) ? type : ValueType::Double;
    out.d = value;
    return out;
}

Value Value::Text (std::string value, ValueType type)
{
    Value out;
    out.type = Textual (type) ? type : ValueType::String;
    out.s = std::move (value);
    return out;
}

Value Value::Option (std::string value)
{
    return Text (std::move (value), ValueType::Enum);
}

double Value::AsNumber () const
{
    if (type == ValueType::Int)
        return double (i);
    if (Measured (type))
        return d;
    return 0.0;
}

bool operator== (const Value& a, const Value& b)
{
    if (a.type != b.type)
        return false;
    switch (a.type) {
        case ValueType::Bool:
            return a.b == b.b;
        case ValueType::Int:
            return a.i == b.i;
        case ValueType::Vector2:
        case ValueType::Vector3:
            return a.v == b.v;
        case ValueType::Color:
            return a.rgba == b.rgba;
        case ValueType::List:
            return a.elementType == b.elementType && a.list == b.list;
        case ValueType::Object:
            return a.fields == b.fields;
        default:
            return Textual (a.type) ? a.s == b.s : a.d == b.d;
    }
}

std::string ToText (const Value& value, int decimals)
{
    switch (value.type) {
        case ValueType::Bool:
            return value.b ? "yes" : "no";
        case ValueType::Int:
            return std::to_string (value.i);
        case ValueType::Percentage:
            return Number (value.d * 100.0, decimals) + " %";
        case ValueType::Vector2:
        case ValueType::Vector3: {
            std::string out;
            for (size_t k = 0; k < value.v.size (); ++k)
                out += (k > 0 ? ", " : "") + Number (value.v[k], decimals);
            return out;
        }
        case ValueType::Color:
            return Hex (value.rgba);
        case ValueType::List: {
            std::string out;
            for (size_t k = 0; k < value.list.size (); ++k)
                out += (k > 0 ? ", " : "") + ToText (value.list[k], decimals);
            return out;
        }
        case ValueType::Object:
            return std::to_string (value.fields.size ()) + " fields";
        default:
            return Textual (value.type) ? value.s : Number (value.d, decimals);
    }
}

std::string NamespaceOf (const std::string& key)
{
    const size_t dot = key.find ('.');
    return dot == std::string::npos ? std::string () : key.substr (0, dot);
}

bool Counted (const std::string& domain)
{
    return domain == kFloorDomain || domain == "storey" || domain == "phase" || domain == "level";
}

bool EntityMetadata::Empty () const
{
    return classifications.empty () && tags.empty () && properties.empty () && ranges.empty () &&
           relationships.empty () && sets.empty ();
}

// ---- editing ----------------------------------------------------------------------------------

const Property* FindProperty (const EntityMetadata& meta, const std::string& key)
{
    for (const Property& property : meta.properties)
        if (property.key == key)
            return &property;
    return nullptr;
}

void SetProperty (EntityMetadata& meta, Property property)
{
    for (Property& existing : meta.properties)
        if (existing.key == property.key) {
            existing = std::move (property);
            return;
        }
    meta.properties.push_back (std::move (property));
}

bool RemoveProperty (EntityMetadata& meta, const std::string& key)
{
    const size_t before = meta.properties.size ();
    meta.properties.erase (std::remove_if (meta.properties.begin (), meta.properties.end (),
                                           [&] (const Property& p) { return p.key == key; }),
                           meta.properties.end ());
    return meta.properties.size () != before;
}

void AssignRange (EntityMetadata& meta, const RangeAssignment& assignment)
{
    RangeAssignment range = assignment;
    if (range.from > range.to)
        std::swap (range.from, range.to);
    if (range.properties.empty () || !HasExtent (range.from, range.to, Counted (range.domain)))
        return;
    for (const Property& property : range.properties)
        ClearKey (meta, range.domain, range.from, range.to, property.key);
    meta.ranges.push_back (range);
    Normalize (meta, range.domain);
}

bool ClearRange (EntityMetadata& meta, const std::string& domain, double from, double to, const std::string& key)
{
    if (from > to)
        std::swap (from, to);
    const bool changed = ClearKey (meta, domain, from, to, key);
    if (changed)
        Normalize (meta, domain);
    return changed;
}

const Property* RangeValue (const EntityMetadata& meta, const std::string& domain, double position,
                            const std::string& key, const RangeAssignment** range)
{
    const bool counted = Counted (domain);
    for (const RangeAssignment& candidate : meta.ranges) {
        if (candidate.domain != domain || !Contains (candidate, position, counted))
            continue;
        for (const Property& property : candidate.properties)
            if (property.key == key) {
                if (range != nullptr)
                    *range = &candidate;
                return &property;
            }
    }
    return nullptr;
}

bool AddTag (EntityMetadata& meta, const std::string& tag)
{
    if (tag.empty () || HasTag (meta, tag))
        return false;
    meta.tags.push_back (tag);
    return true;
}

bool RemoveTag (EntityMetadata& meta, const std::string& tag)
{
    const size_t before = meta.tags.size ();
    meta.tags.erase (std::remove (meta.tags.begin (), meta.tags.end (), tag), meta.tags.end ());
    return meta.tags.size () != before;
}

bool HasTag (const EntityMetadata& meta, const std::string& tag)
{
    return std::find (meta.tags.begin (), meta.tags.end (), tag) != meta.tags.end ();
}

void Classify (EntityMetadata& meta, const std::string& system, const std::string& value, const Provenance& provenance)
{
    for (Classification& existing : meta.classifications)
        if (existing.system == system) {
            existing.value = value;
            existing.provenance = provenance;
            return;
        }
    meta.classifications.push_back ({ system, value, provenance });
}

bool Unclassify (EntityMetadata& meta, const std::string& system)
{
    const size_t before = meta.classifications.size ();
    meta.classifications.erase (std::remove_if (meta.classifications.begin (), meta.classifications.end (),
                                                [&] (const Classification& c) { return c.system == system; }),
                                meta.classifications.end ());
    return meta.classifications.size () != before;
}

std::string ClassificationIn (const EntityMetadata& meta, const std::string& system)
{
    for (const Classification& classification : meta.classifications)
        if (classification.system == system)
            return classification.value;
    return std::string ();
}

void Relate (EntityMetadata& meta, Relationship relationship)
{
    for (Relationship& existing : meta.relationships)
        if (existing.type == relationship.type && existing.target == relationship.target) {
            existing = std::move (relationship);
            return;
        }
    meta.relationships.push_back (std::move (relationship));
}

bool Unrelate (EntityMetadata& meta, const std::string& type, const std::string& target)
{
    const size_t before = meta.relationships.size ();
    meta.relationships.erase (
        std::remove_if (meta.relationships.begin (), meta.relationships.end (),
                        [&] (const Relationship& r) { return r.type == type && r.target == target; }),
        meta.relationships.end ());
    return meta.relationships.size () != before;
}

bool JoinSet (EntityMetadata& meta, const std::string& set)
{
    if (set.empty () || std::find (meta.sets.begin (), meta.sets.end (), set) != meta.sets.end ())
        return false;
    meta.sets.push_back (set);
    return true;
}

bool LeaveSet (EntityMetadata& meta, const std::string& set)
{
    const size_t before = meta.sets.size ();
    meta.sets.erase (std::remove (meta.sets.begin (), meta.sets.end (), set), meta.sets.end ());
    return meta.sets.size () != before;
}

void Merge (EntityMetadata& into, const EntityMetadata& from)
{
    for (const Property& property : from.properties)
        SetProperty (into, property);
    for (const Classification& classification : from.classifications)
        Classify (into, classification.system, classification.value, classification.provenance);
    for (const RangeAssignment& range : from.ranges)
        AssignRange (into, range);
    for (const Relationship& relationship : from.relationships)
        Relate (into, relationship);
    for (const std::string& tag : from.tags)
        AddTag (into, tag);
    for (const std::string& set : from.sets)
        JoinSet (into, set);
}

size_t InvalidateComputed (EntityMetadata& meta, const std::string& generator)
{
    const auto stale = [&] (const Property& p) {
        return (p.state == State::Computed || p.state == State::Cached) &&
               (generator.empty () || p.generator == generator);
    };
    size_t gone = 0;
    const auto purge = [&] (std::vector<Property>& properties) {
        const size_t before = properties.size ();
        properties.erase (std::remove_if (properties.begin (), properties.end (), stale), properties.end ());
        gone += before - properties.size ();
    };
    purge (meta.properties);
    std::set<std::string> domains;
    for (RangeAssignment& range : meta.ranges) {
        purge (range.properties);
        domains.insert (range.domain);
    }
    for (const std::string& domain : domains)
        Normalize (meta, domain);
    return gone;
}

bool AdoptElement (EntityMetadata& meta, const std::string& elementGuid)
{
    if (meta.archicadGuid.empty ()) {
        meta.archicadGuid = elementGuid;
        if (meta.entityId.empty ())
            meta.entityId = NewEntityId ();
        return false;
    }
    if (meta.archicadGuid == elementGuid)
        return false;
    meta.archicadGuid = elementGuid;
    meta.entityId = NewEntityId ();
    return true;
}

std::string NewEntityId ()
{
    static std::mt19937_64 random { std::random_device {}() ^
                                    uint64_t (std::chrono::steady_clock::now ().time_since_epoch ().count ()) };
    const uint64_t hi = random (), lo = random ();
    uint8_t bytes[16];
    for (int k = 0; k < 8; ++k) {
        bytes[k] = uint8_t (hi >> (56 - 8 * k));
        bytes[8 + k] = uint8_t (lo >> (56 - 8 * k));
    }
    bytes[6] = uint8_t ((bytes[6] & 0x0Fu) | 0x40u); // version 4
    bytes[8] = uint8_t ((bytes[8] & 0x3Fu) | 0x80u); // RFC 4122 variant
    char text[40] = {};
    std::snprintf (text, sizeof (text), "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                   bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9],
                   bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return text;
}

} // namespace metadata
} // namespace geomsrv
