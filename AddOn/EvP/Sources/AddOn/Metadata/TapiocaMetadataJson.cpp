// Metadata/TapiocaMetadataJson -- an entity's metadata and the project's schema as JSON
// (TapiocaMetadata.hpp), through the graph runtime's own JSON (NodeGraph/Json.hpp).

#include "Metadata/TapiocaMetadata.hpp"
#include "Metadata/TapiocaMetadataDetail.hpp"
#include "NodeGraph/Json.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

namespace geomsrv {
namespace metadata {

namespace json = evp::nodegraph::json;

using detail::FromHex;
using detail::Hex;
using detail::Measured;
using detail::Number;
using detail::Textual;

namespace {

// ---- JSON writing ---------------------------------------------------------------------------

json::JsonValue Str (const std::string& text)
{
    return json::JsonValue::String (text);
}

json::JsonValue Strings (const std::vector<std::string>& texts)
{
    json::JsonArray out;
    for (const std::string& text : texts)
        out.push_back (Str (text));
    return json::JsonValue::Array (std::move (out));
}

json::JsonValue Write (const Value& value)
{
    json::JsonObject out;
    out["type"] = Str (TypeName (value.type));
    switch (value.type) {
        case ValueType::Bool:
            out["value"] = json::JsonValue::Bool (value.b);
            break;
        case ValueType::Int:
            out["value"] = json::JsonValue::Integer (value.i);
            break;
        case ValueType::Vector2:
        case ValueType::Vector3: {
            json::JsonArray v;
            for (const double x : value.v)
                v.push_back (json::JsonValue::Double (x));
            out["value"] = json::JsonValue::Array (std::move (v));
            break;
        }
        case ValueType::Color:
            out["value"] = Str (Hex (value.rgba));
            break;
        case ValueType::List: {
            out["of"] = Str (TypeName (value.elementType));
            json::JsonArray items;
            for (const Value& item : value.list)
                items.push_back (Write (item));
            out["value"] = json::JsonValue::Array (std::move (items));
            break;
        }
        case ValueType::Object: {
            json::JsonObject fields;
            for (const auto& field : value.fields)
                fields[field.first] = Write (field.second);
            out["value"] = json::JsonValue::Object (std::move (fields));
            break;
        }
        default:
            if (Textual (value.type))
                out["value"] = Str (value.s);
            else
                out["value"] = json::JsonValue::Double (value.d);
            break;
    }
    return json::JsonValue::Object (std::move (out));
}

// What a provenance says beyond the default (the user, nothing else): absent otherwise.
bool Plain (const Provenance& provenance)
{
    return provenance.source == Source::User && provenance.sourceId.empty () && provenance.timestampMs == 0 &&
           provenance.author.empty ();
}

json::JsonValue Write (const Provenance& provenance)
{
    json::JsonObject out;
    out["source"] = Str (SourceName (provenance.source));
    if (!provenance.sourceId.empty ())
        out["sourceId"] = Str (provenance.sourceId);
    if (provenance.timestampMs != 0)
        out["timestamp"] = json::JsonValue::Integer (provenance.timestampMs);
    if (!provenance.author.empty ())
        out["author"] = Str (provenance.author);
    return json::JsonValue::Object (std::move (out));
}

json::JsonValue Write (const std::vector<Property>& properties)
{
    json::JsonArray out;
    for (const Property& property : properties) {
        json::JsonObject p;
        p["key"] = Str (property.key);
        p["value"] = Write (property.value);
        if (!property.unit.empty ())
            p["unit"] = Str (property.unit);
        if (property.state != State::Authored)
            p["state"] = Str (StateName (property.state));
        if (!property.generator.empty ())
            p["generator"] = Str (property.generator);
        if (!Plain (property.provenance))
            p["provenance"] = Write (property.provenance);
        out.push_back (json::JsonValue::Object (std::move (p)));
    }
    return json::JsonValue::Array (std::move (out));
}

json::JsonValue Write (const std::vector<EnumOption>& options)
{
    json::JsonArray out;
    for (const EnumOption& option : options) {
        json::JsonObject o;
        o["value"] = Str (option.value);
        if (!option.label.empty ())
            o["label"] = Str (option.label);
        if ((option.rgba & 0xFFu) != 0)
            o["color"] = Str (Hex (option.rgba));
        out.push_back (json::JsonValue::Object (std::move (o)));
    }
    return json::JsonValue::Array (std::move (out));
}

// ---- JSON reading ---------------------------------------------------------------------------

std::string Text (const json::JsonValue* value)
{
    std::string out;
    if (value != nullptr)
        value->AsString (out);
    return out;
}

double Real (const json::JsonValue* value, double otherwise = 0.0)
{
    double out = otherwise;
    if (value != nullptr)
        value->AsDouble (out);
    return out;
}

std::vector<std::string> TextList (const json::JsonValue* value)
{
    std::vector<std::string> out;
    if (value != nullptr && value->AsArray () != nullptr)
        for (const json::JsonValue& item : *value->AsArray ()) {
            std::string text;
            if (item.AsString (text))
                out.push_back (text);
        }
    return out;
}

bool Read (const json::JsonValue& node, Value& value, std::string& error)
{
    if (node.AsObject () == nullptr) {
        error = "a value is an object with a type and a value";
        return false;
    }
    const std::string type = Text (node.Find ("type"));
    if (!TypeFromName (type, value.type)) {
        error = "unknown value type \"" + type + "\"";
        return false;
    }
    const json::JsonValue* raw = node.Find ("value");
    if (raw == nullptr) {
        error = "a " + type + " value without its value";
        return false;
    }
    switch (value.type) {
        case ValueType::Bool:
            if (!raw->AsBool (value.b)) {
                error = "a bool value is true or false";
                return false;
            }
            return true;
        case ValueType::Int:
            if (!raw->AsInteger (value.i)) {
                error = "an int value is a number";
                return false;
            }
            return true;
        case ValueType::Vector2:
        case ValueType::Vector3: {
            const size_t count = value.type == ValueType::Vector2 ? 2 : 3;
            const json::JsonArray* v = raw->AsArray ();
            if (v == nullptr || v->size () != count) {
                error = type + " is " + std::to_string (count) + " numbers";
                return false;
            }
            value.v.clear ();
            for (const json::JsonValue& x : *v) {
                double d = 0.0;
                if (!x.AsDouble (d)) {
                    error = type + " is " + std::to_string (count) + " numbers";
                    return false;
                }
                value.v.push_back (d);
            }
            return true;
        }
        case ValueType::Color:
            if (!FromHex (Text (raw), value.rgba)) {
                error = "a color is #RRGGBBAA";
                return false;
            }
            return true;
        case ValueType::List: {
            if (!TypeFromName (Text (node.Find ("of")), value.elementType)) {
                error = "a list names the type of its items (\"of\")";
                return false;
            }
            const json::JsonArray* items = raw->AsArray ();
            if (items == nullptr) {
                error = "a list's value is an array";
                return false;
            }
            for (const json::JsonValue& item : *items) {
                Value element;
                if (!Read (item, element, error))
                    return false;
                if (element.type != value.elementType) {
                    error = "a list of " + std::string (TypeName (value.elementType)) + " holds a " +
                            TypeName (element.type);
                    return false;
                }
                value.list.push_back (std::move (element));
            }
            return true;
        }
        case ValueType::Object: {
            const json::JsonObject* fields = raw->AsObject ();
            if (fields == nullptr) {
                error = "an object's value is an object";
                return false;
            }
            for (const auto& field : *fields) {
                Value inner;
                if (!Read (field.second, inner, error))
                    return false;
                value.fields[field.first] = std::move (inner);
            }
            return true;
        }
        default:
            if (Textual (value.type)) {
                if (!raw->AsString (value.s)) {
                    error = "a " + type + " value is text";
                    return false;
                }
                return true;
            }
            if (!raw->AsDouble (value.d)) {
                error = "a " + type + " value is a number";
                return false;
            }
            return true;
    }
}

void Read (const json::JsonValue* node, Provenance& provenance)
{
    if (node == nullptr || node->AsObject () == nullptr)
        return;
    SourceFromName (Text (node->Find ("source")), provenance.source);
    provenance.sourceId = Text (node->Find ("sourceId"));
    int64_t at = 0;
    if (const json::JsonValue* t = node->Find ("timestamp"); t != nullptr && t->AsInteger (at))
        provenance.timestampMs = at;
    provenance.author = Text (node->Find ("author"));
}

bool Read (const json::JsonValue* node, std::vector<Property>& properties, std::string& error)
{
    if (node == nullptr)
        return true;
    const json::JsonArray* items = node->AsArray ();
    if (items == nullptr) {
        error = "properties are an array";
        return false;
    }
    for (const json::JsonValue& item : *items) {
        Property property;
        property.key = Text (item.Find ("key"));
        if (property.key.empty ()) {
            error = "a property without its key";
            return false;
        }
        const json::JsonValue* value = item.Find ("value");
        if (value == nullptr || !Read (*value, property.value, error)) {
            error = property.key + ": " + (error.empty () ? std::string ("no value") : error);
            return false;
        }
        property.unit = Text (item.Find ("unit"));
        if (item.Find ("state") != nullptr && !StateFromName (Text (item.Find ("state")), property.state)) {
            error = property.key + ": unknown state \"" + Text (item.Find ("state")) + "\"";
            return false;
        }
        property.generator = Text (item.Find ("generator"));
        Read (item.Find ("provenance"), property.provenance);
        properties.push_back (std::move (property));
    }
    return true;
}

std::vector<EnumOption> ReadOptions (const json::JsonValue* node)
{
    std::vector<EnumOption> out;
    if (node == nullptr || node->AsArray () == nullptr)
        return out;
    for (const json::JsonValue& item : *node->AsArray ()) {
        EnumOption option;
        option.value = Text (item.Find ("value"));
        option.label = Text (item.Find ("label"));
        FromHex (Text (item.Find ("color")), option.rgba);
        if (!option.value.empty ())
            out.push_back (std::move (option));
    }
    return out;
}

bool Parse (const std::string& text, json::JsonValue& root, std::string& error)
{
    json::ParseResult parsed = json::Parse (text);
    if (!parsed.ok) {
        error = "not JSON at byte " + std::to_string (parsed.offset) + ": " + parsed.error;
        return false;
    }
    if (parsed.value.AsObject () == nullptr) {
        error = "the document is a JSON object";
        return false;
    }
    root = std::move (parsed.value);
    return true;
}
} // namespace

// ---- JSON -------------------------------------------------------------------------------------

std::string ToJson (const EntityMetadata& meta)
{
    json::JsonObject out;
    out["format"] = json::JsonValue::Integer (meta.formatVersion);
    out["entityId"] = Str (meta.entityId);
    if (!meta.archicadGuid.empty ())
        out["archicadGuid"] = Str (meta.archicadGuid);
    if (!meta.classifications.empty ()) {
        json::JsonArray items;
        for (const Classification& classification : meta.classifications) {
            json::JsonObject c;
            c["system"] = Str (classification.system);
            c["value"] = Str (classification.value);
            if (!Plain (classification.provenance))
                c["provenance"] = Write (classification.provenance);
            items.push_back (json::JsonValue::Object (std::move (c)));
        }
        out["classifications"] = json::JsonValue::Array (std::move (items));
    }
    if (!meta.tags.empty ())
        out["tags"] = Strings (meta.tags);
    if (!meta.properties.empty ())
        out["properties"] = Write (meta.properties);
    if (!meta.ranges.empty ()) {
        json::JsonArray items;
        for (const RangeAssignment& range : meta.ranges) {
            json::JsonObject r;
            r["domain"] = Str (range.domain);
            r["from"] = Counted (range.domain) ? json::JsonValue::Integer (int64_t (std::llround (range.from)))
                                               : json::JsonValue::Double (range.from);
            r["to"] = Counted (range.domain) ? json::JsonValue::Integer (int64_t (std::llround (range.to)))
                                             : json::JsonValue::Double (range.to);
            r["properties"] = Write (range.properties);
            if (!Plain (range.provenance))
                r["provenance"] = Write (range.provenance);
            items.push_back (json::JsonValue::Object (std::move (r)));
        }
        out["ranges"] = json::JsonValue::Array (std::move (items));
    }
    if (!meta.relationships.empty ()) {
        json::JsonArray items;
        for (const Relationship& relationship : meta.relationships) {
            json::JsonObject r;
            r["type"] = Str (relationship.type);
            r["target"] = Str (relationship.target);
            if (!relationship.properties.empty ())
                r["properties"] = Write (relationship.properties);
            if (!Plain (relationship.provenance))
                r["provenance"] = Write (relationship.provenance);
            items.push_back (json::JsonValue::Object (std::move (r)));
        }
        out["relationships"] = json::JsonValue::Array (std::move (items));
    }
    if (!meta.sets.empty ())
        out["sets"] = Strings (meta.sets);
    if (!Plain (meta.provenance))
        out["provenance"] = Write (meta.provenance);
    return json::Write (json::JsonValue::Object (std::move (out)), 0);
}

bool FromJson (const std::string& text, EntityMetadata& meta, std::string& error)
{
    json::JsonValue root;
    if (!Parse (text, root, error))
        return false;
    EntityMetadata out;
    int64_t format = kFormatVersion;
    if (const json::JsonValue* f = root.Find ("format"); f != nullptr)
        f->AsInteger (format);
    if (format > kFormatVersion) {
        error = "metadata format " + std::to_string (format) + " is newer than this add-on's " +
                std::to_string (kFormatVersion);
        return false;
    }
    out.formatVersion = int (format);
    out.entityId = Text (root.Find ("entityId"));
    out.archicadGuid = Text (root.Find ("archicadGuid"));
    if (const json::JsonValue* items = root.Find ("classifications"); items != nullptr && items->AsArray () != nullptr)
        for (const json::JsonValue& item : *items->AsArray ()) {
            Classification classification;
            classification.system = Text (item.Find ("system"));
            classification.value = Text (item.Find ("value"));
            Read (item.Find ("provenance"), classification.provenance);
            if (!classification.system.empty ())
                out.classifications.push_back (std::move (classification));
        }
    out.tags = TextList (root.Find ("tags"));
    if (!Read (root.Find ("properties"), out.properties, error))
        return false;
    if (const json::JsonValue* items = root.Find ("ranges"); items != nullptr && items->AsArray () != nullptr)
        for (const json::JsonValue& item : *items->AsArray ()) {
            RangeAssignment range;
            range.domain = Text (item.Find ("domain"));
            range.from = Real (item.Find ("from"));
            range.to = Real (item.Find ("to"));
            if (!Read (item.Find ("properties"), range.properties, error)) {
                error = "a " + range.domain + " range: " + error;
                return false;
            }
            Read (item.Find ("provenance"), range.provenance);
            out.ranges.push_back (std::move (range));
        }
    if (const json::JsonValue* items = root.Find ("relationships"); items != nullptr && items->AsArray () != nullptr)
        for (const json::JsonValue& item : *items->AsArray ()) {
            Relationship relationship;
            relationship.type = Text (item.Find ("type"));
            relationship.target = Text (item.Find ("target"));
            if (!Read (item.Find ("properties"), relationship.properties, error))
                return false;
            Read (item.Find ("provenance"), relationship.provenance);
            out.relationships.push_back (std::move (relationship));
        }
    out.sets = TextList (root.Find ("sets"));
    Read (root.Find ("provenance"), out.provenance);
    meta = std::move (out);
    return true;
}

std::string ToJson (const ProjectSchema& schema)
{
    json::JsonObject out;
    out["format"] = json::JsonValue::Integer (schema.formatVersion);
    out["revision"] = json::JsonValue::Integer (schema.revision);
    json::JsonArray properties;
    for (const PropertyDefinition& definition : schema.properties) {
        json::JsonObject d;
        d["key"] = Str (definition.key);
        d["label"] = Str (definition.label);
        d["type"] = Str (TypeName (definition.type));
        if (!definition.enumId.empty ())
            d["enum"] = Str (definition.enumId);
        if (!definition.unit.empty ())
            d["unit"] = Str (definition.unit);
        if (!definition.applicableTo.empty ())
            d["applicableTo"] = Strings (definition.applicableTo);
        if (definition.hasDefault)
            d["default"] = Write (definition.defaultValue);
        if (definition.max > definition.min) {
            d["min"] = json::JsonValue::Double (definition.min);
            d["max"] = json::JsonValue::Double (definition.max);
            if (definition.step > 0.0)
                d["step"] = json::JsonValue::Double (definition.step);
        }
        if (!definition.domains.empty ())
            d["domains"] = Strings (definition.domains);
        if (!definition.group.empty ())
            d["group"] = Str (definition.group);
        if (!definition.description.empty ())
            d["description"] = Str (definition.description);
        properties.push_back (json::JsonValue::Object (std::move (d)));
    }
    out["properties"] = json::JsonValue::Array (std::move (properties));
    json::JsonArray enumerations;
    for (const Enumeration& enumeration : schema.enumerations) {
        json::JsonObject e;
        e["id"] = Str (enumeration.id);
        e["label"] = Str (enumeration.label);
        e["options"] = Write (enumeration.options);
        enumerations.push_back (json::JsonValue::Object (std::move (e)));
    }
    out["enumerations"] = json::JsonValue::Array (std::move (enumerations));
    json::JsonArray systems;
    for (const ClassificationSystem& system : schema.classifications) {
        json::JsonObject s;
        s["id"] = Str (system.id);
        s["label"] = Str (system.label);
        s["values"] = Write (system.values);
        systems.push_back (json::JsonValue::Object (std::move (s)));
    }
    out["classifications"] = json::JsonValue::Array (std::move (systems));
    json::JsonArray relationships;
    for (const RelationshipType& type : schema.relationships) {
        json::JsonObject r;
        r["id"] = Str (type.id);
        r["label"] = Str (type.label);
        if (!type.inverse.empty ())
            r["inverse"] = Str (type.inverse);
        relationships.push_back (json::JsonValue::Object (std::move (r)));
    }
    out["relationships"] = json::JsonValue::Array (std::move (relationships));
    json::JsonArray units;
    for (const UnitDefinition& unit : schema.units) {
        json::JsonObject u;
        u["id"] = Str (unit.id);
        u["symbol"] = Str (unit.symbol);
        u["quantity"] = Str (unit.quantity);
        u["toCanonical"] = json::JsonValue::Double (unit.toCanonical);
        units.push_back (json::JsonValue::Object (std::move (u)));
    }
    out["units"] = json::JsonValue::Array (std::move (units));
    json::JsonArray sets;
    for (const SetDefinition& set : schema.sets) {
        json::JsonObject s;
        s["id"] = Str (set.id);
        s["label"] = Str (set.label);
        if ((set.rgba & 0xFFu) != 0)
            s["color"] = Str (Hex (set.rgba));
        if (!set.description.empty ())
            s["description"] = Str (set.description);
        sets.push_back (json::JsonValue::Object (std::move (s)));
    }
    out["sets"] = json::JsonValue::Array (std::move (sets));
    json::JsonArray ui;
    for (const UiSchema& panel : schema.ui) {
        json::JsonObject u;
        u["id"] = Str (panel.id);
        u["title"] = Str (panel.title);
        u["keys"] = Strings (panel.keys);
        ui.push_back (json::JsonValue::Object (std::move (u)));
    }
    out["ui"] = json::JsonValue::Array (std::move (ui));
    out["tags"] = Strings (schema.tags);
    json::JsonArray migrations;
    for (const Migration& migration : schema.migrations) {
        json::JsonObject m;
        m["from"] = json::JsonValue::Integer (migration.from);
        m["to"] = json::JsonValue::Integer (migration.to);
        m["note"] = Str (migration.note);
        migrations.push_back (json::JsonValue::Object (std::move (m)));
    }
    out["migrations"] = json::JsonValue::Array (std::move (migrations));
    return json::Write (json::JsonValue::Object (std::move (out)), 0);
}

bool FromJson (const std::string& text, ProjectSchema& schema, std::string& error)
{
    json::JsonValue root;
    if (!Parse (text, root, error))
        return false;
    ProjectSchema out;
    int64_t number = kFormatVersion;
    if (const json::JsonValue* f = root.Find ("format"); f != nullptr)
        f->AsInteger (number);
    if (number > kFormatVersion) {
        error = "schema format " + std::to_string (number) + " is newer than this add-on's " +
                std::to_string (kFormatVersion);
        return false;
    }
    out.formatVersion = int (number);
    number = 0;
    if (const json::JsonValue* r = root.Find ("revision"); r != nullptr)
        r->AsInteger (number);
    out.revision = int (number);
    if (const json::JsonValue* items = root.Find ("properties"); items != nullptr && items->AsArray () != nullptr)
        for (const json::JsonValue& item : *items->AsArray ()) {
            PropertyDefinition definition;
            definition.key = Text (item.Find ("key"));
            definition.label = Text (item.Find ("label"));
            if (definition.key.empty () || !TypeFromName (Text (item.Find ("type")), definition.type)) {
                error = "a property definition needs its key and a known type (" + definition.key + ")";
                return false;
            }
            definition.enumId = Text (item.Find ("enum"));
            definition.unit = Text (item.Find ("unit"));
            definition.applicableTo = TextList (item.Find ("applicableTo"));
            if (const json::JsonValue* d = item.Find ("default"); d != nullptr) {
                if (!Read (*d, definition.defaultValue, error)) {
                    error = definition.key + "'s default: " + error;
                    return false;
                }
                definition.hasDefault = true;
            }
            definition.min = Real (item.Find ("min"));
            definition.max = Real (item.Find ("max"));
            definition.step = Real (item.Find ("step"));
            definition.domains = TextList (item.Find ("domains"));
            definition.group = Text (item.Find ("group"));
            definition.description = Text (item.Find ("description"));
            out.properties.push_back (std::move (definition));
        }
    if (const json::JsonValue* items = root.Find ("enumerations"); items != nullptr && items->AsArray () != nullptr)
        for (const json::JsonValue& item : *items->AsArray ())
            out.enumerations.push_back (
                { Text (item.Find ("id")), Text (item.Find ("label")), ReadOptions (item.Find ("options")) });
    if (const json::JsonValue* items = root.Find ("classifications"); items != nullptr && items->AsArray () != nullptr)
        for (const json::JsonValue& item : *items->AsArray ())
            out.classifications.push_back (
                { Text (item.Find ("id")), Text (item.Find ("label")), ReadOptions (item.Find ("values")) });
    if (const json::JsonValue* items = root.Find ("relationships"); items != nullptr && items->AsArray () != nullptr)
        for (const json::JsonValue& item : *items->AsArray ())
            out.relationships.push_back (
                { Text (item.Find ("id")), Text (item.Find ("label")), Text (item.Find ("inverse")) });
    if (const json::JsonValue* items = root.Find ("units"); items != nullptr && items->AsArray () != nullptr)
        for (const json::JsonValue& item : *items->AsArray ())
            out.units.push_back ({ Text (item.Find ("id")), Text (item.Find ("symbol")), Text (item.Find ("quantity")),
                                   Real (item.Find ("toCanonical"), 1.0) });
    if (const json::JsonValue* items = root.Find ("sets"); items != nullptr && items->AsArray () != nullptr)
        for (const json::JsonValue& item : *items->AsArray ()) {
            SetDefinition set;
            set.id = Text (item.Find ("id"));
            set.label = Text (item.Find ("label"));
            FromHex (Text (item.Find ("color")), set.rgba);
            set.description = Text (item.Find ("description"));
            out.sets.push_back (std::move (set));
        }
    if (const json::JsonValue* items = root.Find ("ui"); items != nullptr && items->AsArray () != nullptr)
        for (const json::JsonValue& item : *items->AsArray ())
            out.ui.push_back ({ Text (item.Find ("id")), Text (item.Find ("title")), TextList (item.Find ("keys")) });
    out.tags = TextList (root.Find ("tags"));
    if (const json::JsonValue* items = root.Find ("migrations"); items != nullptr && items->AsArray () != nullptr)
        for (const json::JsonValue& item : *items->AsArray ()) {
            Migration migration;
            int64_t from = 0, to = 0;
            if (const json::JsonValue* f = item.Find ("from"); f != nullptr)
                f->AsInteger (from);
            if (const json::JsonValue* t = item.Find ("to"); t != nullptr)
                t->AsInteger (to);
            migration.from = int (from);
            migration.to = int (to);
            migration.note = Text (item.Find ("note"));
            out.migrations.push_back (std::move (migration));
        }
    schema = std::move (out);
    return true;
}

} // namespace metadata
} // namespace geomsrv
