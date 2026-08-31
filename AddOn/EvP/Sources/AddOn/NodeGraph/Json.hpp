#ifndef EVP_NODEGRAPH_JSON_HPP
#define EVP_NODEGRAPH_JSON_HPP

// A small JSON value, reader and writer, owned by the graph runtime.
//
// WHY NOT GS::ObjectState, WHICH THE REPOSITORY ALREADY HAS. Two reasons, and
// the first is decisive:
//
//  1. GS::ObjectState is DevKit. The whole NodeGraph tree is deliberately
//     DevKit-free and therefore covered by the offline suite; a persisted graph
//     format written in ObjectState would be the one part of the runtime that
//     can only be tested inside Archicad. A workflow library that outlives the
//     project it was authored in is exactly the thing that must be testable
//     without a host.
//  2. ObjectState reserves the key "type" and cannot represent JSON null, both
//     of which the architecture document records as quirks to hide behind
//     helpers. A stored document should not inherit a bridge's quirks.
//
// WHY NOT A LIBRARY. ADR-007 adds no entry to AddOn/reference/CATALOG.yaml, and
// §40.3 says a JSON dependency is warranted when a future layer needs generated
// schema, reflection or throughput. None of that is true of a file holding a few
// hundred nodes. This is a few hundred lines with a fixed surface, and it stays
// behind GraphSerializer so replacing it later is one file.
//
// The bridge keeps ObjectState. This is for FILES, not for the wire.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace evp::nodegraph::json {

class JsonValue;

using JsonObject = std::map<std::string, JsonValue>;
using JsonArray = std::vector<JsonValue>;

enum class JsonKind {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object,
};

// Numbers carry whether they were written without a fraction, because a graph
// parameter distinguishes Integer from Double and a format that rounds one into
// the other silently changes what a node computes.
class JsonValue {
  public:
    JsonValue () = default;
    static JsonValue Bool (bool value);
    static JsonValue Integer (int64_t value);
    static JsonValue Double (double value);
    static JsonValue String (std::string value);
    static JsonValue Array (JsonArray value);
    static JsonValue Object (JsonObject value);

    JsonKind Kind () const
    {
        return kind_;
    }
    bool IsNull () const
    {
        return kind_ == JsonKind::Null;
    }

    // Typed readers. Each returns false and leaves `out` alone when this value
    // is not of that kind, so a malformed document is a rejection rather than a
    // default that quietly means something else.
    bool AsBool (bool& out) const;
    bool AsInteger (int64_t& out) const;
    bool AsDouble (double& out) const;
    bool AsString (std::string& out) const;
    const JsonArray* AsArray () const;
    const JsonObject* AsObject () const;

    // Object member lookup. Returns nullptr when this is not an object or the
    // member is absent - absence being how an optional value is expressed.
    const JsonValue* Find (const std::string& key) const;

    bool IsIntegral () const
    {
        return kind_ == JsonKind::Number && integral_;
    }

  private:
    JsonKind kind_ = JsonKind::Null;
    bool bool_ = false;
    bool integral_ = false;
    double number_ = 0.0;
    int64_t integer_ = 0;
    std::string string_;
    JsonArray array_;
    JsonObject object_;
};

// Writes `value` as UTF-8 JSON. `indent` of 0 writes one line; anything larger
// pretty-prints, which is what a file a human may open in a diff wants.
std::string Write (const JsonValue& value, size_t indent = 2);

struct ParseResult {
    bool ok = false;

    // Names what was wrong and where, because "invalid JSON" is not actionable
    // on a file somebody hand-edited.
    std::string error;
    size_t offset = 0;

    JsonValue value;
};

// Parses UTF-8 JSON. Bounded: nesting deeper than `maxDepth` is refused rather
// than recursed into, so a hostile or corrupt file cannot overflow the stack
// inside Archicad's process - the same rule the value walker follows.
ParseResult Parse (const std::string& text, size_t maxDepth = 64);

} // namespace evp::nodegraph::json

#endif
