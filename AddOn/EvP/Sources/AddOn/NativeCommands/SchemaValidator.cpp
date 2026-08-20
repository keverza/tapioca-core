#include "NativeCommands/SchemaValidator.hpp"

#include "NativeCommands/CommandSchemas.hpp"
#include "ObjectStateJSONConversion.hpp"

#include <string>
#include <vector>

namespace geomsrv {
namespace {

struct JsonValue {
    enum class Kind { Object, List, Bool, Int, UInt, Real, String };

    Kind kind = Kind::Object;
    std::vector<GS::String> names;
    std::vector<JsonValue> values;
    bool boolValue = false;
    Int64 intValue = 0;
    UInt64 uintValue = 0;
    double realValue = 0.0;
    GS::UniString stringValue;

    const JsonValue* Find (const char* name) const
    {
        for (std::size_t i = 0; i < names.size (); ++i) {
            if (names[i] == name)
                return &values[i];
        }
        return nullptr;
    }
};

class TreeBuilder final : public GS::ObjectState::Processor {
public:
    JsonValue root;

    TreeBuilder () { stack.push_back (&root); }

    void BoolFound (const GS::String& name, bool value) override
    {
        JsonValue node; node.kind = JsonValue::Kind::Bool; node.boolValue = value;
        Add (name, std::move (node));
    }
    void IntFound (const GS::String& name, Int64 value) override
    {
        JsonValue node; node.kind = JsonValue::Kind::Int; node.intValue = value;
        Add (name, std::move (node));
    }
    void UIntFound (const GS::String& name, UInt64 value) override
    {
        JsonValue node; node.kind = JsonValue::Kind::UInt; node.uintValue = value;
        Add (name, std::move (node));
    }
    void RealFound (const GS::String& name, double value) override
    {
        JsonValue node; node.kind = JsonValue::Kind::Real; node.realValue = value;
        Add (name, std::move (node));
    }
    void StringFound (const GS::String& name, const GS::UniString& value) override
    {
        JsonValue node; node.kind = JsonValue::Kind::String; node.stringValue = value;
        Add (name, std::move (node));
    }
    bool ObjectFound (const GS::String&, const GS::ObjectState&) override { return true; }
    void ObjectEntered (const GS::String& name) override { Enter (name, JsonValue::Kind::Object); }
    void ObjectExited (const GS::String&) override { stack.pop_back (); }
    bool ListFound (const GS::String&) override { return true; }
    void ListEntered (const GS::String& name) override { Enter (name, JsonValue::Kind::List); }
    void ListExited (const GS::String&) override { stack.pop_back (); }

private:
    std::vector<JsonValue*> stack;

    JsonValue* Add (const GS::String& name, JsonValue&& node)
    {
        JsonValue* parent = stack.back ();
        if (parent->kind == JsonValue::Kind::Object)
            parent->names.push_back (name);
        parent->values.push_back (std::move (node));
        return &parent->values.back ();
    }

    void Enter (const GS::String& name, JsonValue::Kind kind)
    {
        JsonValue node; node.kind = kind;
        stack.push_back (Add (name, std::move (node)));
    }
};

JsonValue BuildTree (const GS::ObjectState& os)
{
    TreeBuilder builder;
    os.Enumerate (builder);
    return std::move (builder.root);
}

GS::UniString ChildPath (const GS::UniString& path, const GS::String& name)
{
    return path + "." + GS::UniString (name.ToCStr ());
}

const char* KindName (JsonValue::Kind kind)
{
    switch (kind) {
        case JsonValue::Kind::Object: return "object";
        case JsonValue::Kind::List:   return "array";
        case JsonValue::Kind::Bool:   return "boolean";
        case JsonValue::Kind::Int:
        case JsonValue::Kind::UInt:   return "integer";
        case JsonValue::Kind::Real:   return "number";
        case JsonValue::Kind::String: return "string";
    }
    return "unknown";
}

const JsonValue* ResolveRef (const GS::UniString& ref, const JsonValue& root,
                             const JsonValue& definitions)
{
    const char* text = ref.ToCStr ().Get ();
    if (text[0] != '#')
        return nullptr;
    if (text[1] != '/')
        return definitions.Find (text + 1);

    const JsonValue* current = &root;
    const char* segment = text + 2;
    while (*segment != '\0') {
        const char* slash = segment;
        while (*slash != '\0' && *slash != '/')
            ++slash;
        const std::string key (segment, slash);
        current = current->Find (key.c_str ());
        if (current == nullptr)
            return nullptr;
        segment = (*slash == '/') ? slash + 1 : slash;
    }
    return current;
}

bool IsNumeric (const JsonValue& value)
{
    return value.kind == JsonValue::Kind::Int ||
           value.kind == JsonValue::Kind::UInt ||
           value.kind == JsonValue::Kind::Real;
}

long double NumericValue (const JsonValue& value)
{
    switch (value.kind) {
        case JsonValue::Kind::Int:  return static_cast<long double> (value.intValue);
        case JsonValue::Kind::UInt: return static_cast<long double> (value.uintValue);
        case JsonValue::Kind::Real: return static_cast<long double> (value.realValue);
        default:                    return 0.0L;
    }
}

bool JsonValuesEqual (const JsonValue& lhs, const JsonValue& rhs)
{
    if (IsNumeric (lhs) && IsNumeric (rhs))
        return NumericValue (lhs) == NumericValue (rhs);
    if (lhs.kind != rhs.kind)
        return false;

    switch (lhs.kind) {
        case JsonValue::Kind::Bool:
            return lhs.boolValue == rhs.boolValue;
        case JsonValue::Kind::String:
            return lhs.stringValue == rhs.stringValue;
        case JsonValue::Kind::Object:
            if (lhs.names.size () != rhs.names.size ())
                return false;
            for (std::size_t i = 0; i < lhs.names.size (); ++i) {
                const JsonValue* rhsValue = rhs.Find (lhs.names[i].ToCStr ());
                if (rhsValue == nullptr || !JsonValuesEqual (lhs.values[i], *rhsValue))
                    return false;
            }
            return true;
        case JsonValue::Kind::List:
            if (lhs.values.size () != rhs.values.size ())
                return false;
            for (std::size_t i = 0; i < lhs.values.size (); ++i) {
                if (!JsonValuesEqual (lhs.values[i], rhs.values[i]))
                    return false;
            }
            return true;
        case JsonValue::Kind::Int:
        case JsonValue::Kind::UInt:
        case JsonValue::Kind::Real:
            return false; // Numeric values are handled before the kind switch.
    }
    return false;
}

bool ReadSizeConstraint (const JsonValue& schema, const char* keyword,
                         const GS::UniString& path, UInt64& result,
                         bool& present, GS::UniString& error)
{
    present = false;
    const JsonValue* constraint = schema.Find (keyword);
    if (constraint == nullptr)
        return true;
    present = true;
    if (constraint->kind == JsonValue::Kind::UInt) {
        result = constraint->uintValue;
        return true;
    }
    if (constraint->kind == JsonValue::Kind::Int && constraint->intValue >= 0) {
        result = static_cast<UInt64> (constraint->intValue);
        return true;
    }
    error = path + ": schema keyword '" + GS::UniString (keyword) + "' must be a non-negative integer";
    return false;
}

bool ValidateNumericBounds (const JsonValue& value, const JsonValue& schema,
                            const GS::UniString& path, GS::UniString& error)
{
    struct Bound { const char* keyword; bool lower; bool exclusive; };
    constexpr Bound bounds[] = {
        { "minimum", true, false },
        { "maximum", false, false },
        { "exclusiveMinimum", true, true },
        { "exclusiveMaximum", false, true }
    };
    const long double actual = NumericValue (value);
    for (const Bound& bound : bounds) {
        const JsonValue* constraint = schema.Find (bound.keyword);
        if (constraint == nullptr)
            continue;
        if (!IsNumeric (*constraint)) {
            error = path + ": schema keyword '" + GS::UniString (bound.keyword) + "' must be numeric";
            return false;
        }
        const long double limit = NumericValue (*constraint);
        const bool accepted = bound.lower
            ? (bound.exclusive ? actual > limit : actual >= limit)
            : (bound.exclusive ? actual < limit : actual <= limit);
        if (!accepted) {
            error = path + ": value violates " + GS::UniString (bound.keyword);
            return false;
        }
    }
    return true;
}

bool Validate (const JsonValue& value, const JsonValue& schema, const JsonValue& root,
               const JsonValue& definitions, const GS::UniString& path, GS::UniString& error);

bool ValidateAlternatives (const JsonValue& value, const JsonValue& alternatives,
                           const JsonValue& root, const JsonValue& definitions,
                           const GS::UniString& path, bool exactlyOne, GS::UniString& error)
{
    if (alternatives.kind != JsonValue::Kind::List) {
        error = path + ": schema alternatives must be an array";
        return false;
    }
    UInt32 matches = 0;
    GS::UniString firstFailure;
    for (const JsonValue& alternative : alternatives.values) {
        GS::UniString failure;
        if (Validate (value, alternative, root, definitions, path, failure))
            ++matches;
        else if (firstFailure.IsEmpty ())
            firstFailure = failure;
    }
    if ((exactlyOne && matches == 1) || (!exactlyOne && matches > 0))
        return true;
    error = path + (exactlyOne ? ": expected exactly one oneOf match" : ": expected an anyOf match");
    if (matches == 0 && !firstFailure.IsEmpty ())
        error += " (first alternative: " + firstFailure + ")";
    else if (matches > 1)
        error += GS::UniString::Printf ("; matched %u alternatives", (unsigned) matches);
    return false;
}

bool ValidateObject (const JsonValue& value, const JsonValue& schema, const JsonValue& root,
                     const JsonValue& definitions, const GS::UniString& path, GS::UniString& error)
{
    const JsonValue* required = schema.Find ("required");
    if (required != nullptr && required->kind == JsonValue::Kind::List) {
        for (const JsonValue& item : required->values) {
            if (item.kind == JsonValue::Kind::String && value.Find (item.stringValue.ToCStr ().Get ()) == nullptr) {
                error = path + ": missing required property '" + item.stringValue + "'";
                return false;
            }
        }
    }

    const JsonValue* properties = schema.Find ("properties");
    const JsonValue* additional = schema.Find ("additionalProperties");
    if (additional != nullptr && additional->kind == JsonValue::Kind::Bool && !additional->boolValue) {
        for (const GS::String& name : value.names) {
            if (properties == nullptr || properties->Find (name.ToCStr ()) == nullptr) {
                error = ChildPath (path, name) + ": additional property is not allowed";
                return false;
            }
        }
    }

    if (properties != nullptr && properties->kind == JsonValue::Kind::Object) {
        for (std::size_t i = 0; i < properties->names.size (); ++i) {
            const JsonValue* child = value.Find (properties->names[i].ToCStr ());
            if (child != nullptr && !Validate (*child, properties->values[i], root, definitions,
                                               ChildPath (path, properties->names[i]), error))
                return false;
        }
    }
    return true;
}

bool Validate (const JsonValue& value, const JsonValue& schema, const JsonValue& root,
               const JsonValue& definitions, const GS::UniString& path, GS::UniString& error)
{
    if (schema.kind != JsonValue::Kind::Object) {
        error = path + ": schema node is not an object";
        return false;
    }
    if (const JsonValue* ref = schema.Find ("$ref")) {
        const JsonValue* target = ref->kind == JsonValue::Kind::String
            ? ResolveRef (ref->stringValue, root, definitions) : nullptr;
        if (target == nullptr) {
            error = path + ": unresolved local schema ref '" + ref->stringValue + "'";
            return false;
        }
        return Validate (value, *target, root, definitions, path, error);
    }
    if (const JsonValue* oneOf = schema.Find ("oneOf")) {
        if (!ValidateAlternatives (value, *oneOf, root, definitions, path, true, error))
            return false;
    }
    if (const JsonValue* anyOf = schema.Find ("anyOf")) {
        if (!ValidateAlternatives (value, *anyOf, root, definitions, path, false, error))
            return false;
    }
    if (const JsonValue* constant = schema.Find ("const")) {
        if (!JsonValuesEqual (value, *constant)) {
            error = path + ": value does not match const";
            return false;
        }
    }
    if (const JsonValue* enumeration = schema.Find ("enum")) {
        if (enumeration->kind != JsonValue::Kind::List) {
            error = path + ": schema keyword 'enum' must be an array";
            return false;
        }
        bool matched = false;
        for (const JsonValue& candidate : enumeration->values) {
            if (JsonValuesEqual (value, candidate)) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            error = path + ": value is not in enum";
            return false;
        }
    }

    const JsonValue* type = schema.Find ("type");
    const bool inferredObject = type == nullptr &&
        (schema.Find ("properties") != nullptr || schema.Find ("required") != nullptr ||
         schema.Find ("additionalProperties") != nullptr);
    if (type == nullptr && !inferredObject)
        return true;

    const GS::UniString expected = inferredObject ? GS::UniString ("object") : type->stringValue;
    const bool typeOk =
        (expected == "object"  && value.kind == JsonValue::Kind::Object) ||
        (expected == "array"   && value.kind == JsonValue::Kind::List) ||
        (expected == "boolean" && value.kind == JsonValue::Kind::Bool) ||
        (expected == "string"  && value.kind == JsonValue::Kind::String) ||
        (expected == "integer" && (value.kind == JsonValue::Kind::Int || value.kind == JsonValue::Kind::UInt)) ||
        (expected == "number"  && (value.kind == JsonValue::Kind::Int || value.kind == JsonValue::Kind::UInt ||
                                    value.kind == JsonValue::Kind::Real));
    if (!typeOk) {
        error = path + ": expected " + expected + ", got " + GS::UniString (KindName (value.kind));
        return false;
    }
    if (expected == "object")
        return ValidateObject (value, schema, root, definitions, path, error);
    if (expected == "integer" || expected == "number")
        return ValidateNumericBounds (value, schema, path, error);
    if (expected == "string") {
        UInt64 minLength = 0;
        UInt64 maxLength = 0;
        bool hasMinLength = false;
        bool hasMaxLength = false;
        if (!ReadSizeConstraint (schema, "minLength", path, minLength, hasMinLength, error) ||
            !ReadSizeConstraint (schema, "maxLength", path, maxLength, hasMaxLength, error))
            return false;
        const UInt64 length = static_cast<UInt64> (value.stringValue.GetLength ());
        if (hasMinLength && length < minLength) {
            error = path + ": string is shorter than minLength";
            return false;
        }
        if (hasMaxLength && length > maxLength) {
            error = path + ": string is longer than maxLength";
            return false;
        }
    }
    if (expected == "array") {
        UInt64 minItems = 0;
        UInt64 maxItems = 0;
        bool hasMinItems = false;
        bool hasMaxItems = false;
        if (!ReadSizeConstraint (schema, "minItems", path, minItems, hasMinItems, error) ||
            !ReadSizeConstraint (schema, "maxItems", path, maxItems, hasMaxItems, error))
            return false;
        const UInt64 itemCount = static_cast<UInt64> (value.values.size ());
        if (hasMinItems && itemCount < minItems) {
            error = path + ": array has fewer items than minItems";
            return false;
        }
        if (hasMaxItems && itemCount > maxItems) {
            error = path + ": array has more items than maxItems";
            return false;
        }
        if (const JsonValue* uniqueItems = schema.Find ("uniqueItems")) {
            if (uniqueItems->kind != JsonValue::Kind::Bool) {
                error = path + ": schema keyword 'uniqueItems' must be boolean";
                return false;
            }
            if (uniqueItems->boolValue) {
                for (std::size_t i = 0; i < value.values.size (); ++i) {
                    for (std::size_t j = i + 1; j < value.values.size (); ++j) {
                        if (JsonValuesEqual (value.values[i], value.values[j])) {
                            error = path + GS::UniString::Printf ("[%u]: duplicate array item", (unsigned) j);
                            return false;
                        }
                    }
                }
            }
        }
        if (const JsonValue* items = schema.Find ("items")) {
            for (std::size_t i = 0; i < value.values.size (); ++i) {
                const GS::UniString itemPath = path + GS::UniString::Printf ("[%u]", (unsigned) i);
                if (!Validate (value.values[i], *items, root, definitions, itemPath, error))
                    return false;
            }
        }
    }
    return true;
}

bool Parse (const GS::UniString& json, JsonValue& value)
{
    GS::ObjectState os;
    if (JSON::ConvertToObjectState (json, os) != NoError)
        return false;
    value = BuildTree (os);
    return true;
}

} // namespace

bool ValidateObjectStateSchema (const GS::ObjectState& value,
                                const GS::Optional<GS::UniString>& schemaJson,
                                GS::UniString& error)
{
    if (!schemaJson.HasValue ()) {
        error = "$: command has no schema";
        return false;
    }

    JsonValue schema;
    if (!Parse (schemaJson.Get (), schema)) {
        error = "$: schema is not valid JSON";
        return false;
    }

    JsonValue definitions;
    const GS::Optional<GS::UniString> definitionsJson = GetNativeSchemaDefinitions ();
    if (!definitionsJson.HasValue () || !Parse (definitionsJson.Get (), definitions)) {
        error = "$: shared schema definitions are not valid JSON";
        return false;
    }

    return Validate (BuildTree (value), schema, schema, definitions, "$", error);
}

bool RunSchemaValidatorSelfCheck (GS::UniString& error)
{
    const GS::UniString schema = R"json({
        "type":"object",
        "properties":{
            "choice":{"type":"string","enum":["a","b"]},
            "number":{"type":"number","minimum":1,"maximum":2,"exclusiveMinimum":0,"exclusiveMaximum":3},
            "text":{"type":"string","minLength":1,"maxLength":2},
            "items":{"type":"array","minItems":1,"maxItems":2,"uniqueItems":true,"items":{"type":"number"}}
        },
        "additionalProperties":false,
        "required":["choice","number","text","items"]
    })json";

    struct Case { const char* name; const char* json; bool expected; };
    constexpr Case cases[] = {
        { "accepted boundaries", R"json({"choice":"a","number":1,"text":"ab","items":[1,2]})json", true },
        { "enum", R"json({"choice":"c","number":1,"text":"a","items":[1]})json", false },
        { "minimum", R"json({"choice":"a","number":0.5,"text":"a","items":[1]})json", false },
        { "maximum", R"json({"choice":"a","number":2.5,"text":"a","items":[1]})json", false },
        { "exclusiveMinimum", R"json({"choice":"a","number":0,"text":"a","items":[1]})json", false },
        { "exclusiveMaximum", R"json({"choice":"a","number":3,"text":"a","items":[1]})json", false },
        { "minLength", R"json({"choice":"a","number":1,"text":"","items":[1]})json", false },
        { "maxLength", R"json({"choice":"a","number":1,"text":"abc","items":[1]})json", false },
        { "minItems", R"json({"choice":"a","number":1,"text":"a","items":[]})json", false },
        { "maxItems", R"json({"choice":"a","number":1,"text":"a","items":[1,2,3]})json", false },
        { "uniqueItems", R"json({"choice":"a","number":1,"text":"a","items":[1,1.0]})json", false }
    };

    for (const Case& testCase : cases) {
        GS::ObjectState value;
        if (JSON::ConvertToObjectState (GS::UniString (testCase.json), value) != NoError) {
            error = "schema validator self-check could not parse case: " + GS::UniString (testCase.name);
            return false;
        }
        GS::UniString validationError;
        const bool accepted = ValidateObjectStateSchema (value, schema, validationError);
        if (accepted != testCase.expected) {
            error = "schema validator self-check failed for " + GS::UniString (testCase.name);
            if (!validationError.IsEmpty ())
                error += ": " + validationError;
            return false;
        }
    }
    return true;
}

} // namespace geomsrv
