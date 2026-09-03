#ifndef EVP_NODEGRAPH_SCRIPTVALUEJSON_HPP
#define EVP_NODEGRAPH_SCRIPTVALUEJSON_HPP

// What a graph Value LOOKS LIKE to a script, written down once.
//
// ⚠️ THIS IS A DIFFERENT ENCODING FROM THE GRAPH FILE'S, ON PURPOSE. The file
// format (GraphSerializer) writes a typed envelope - `{"valueType":"double",
// "value":2}` - because a file has to survive a round trip with no schema in
// hand. A script has the schema: its header declared the port's type, so the
// runtime knows what it is handing over and what it expects back. Making a
// script author write `radius.value` to get at a number would be a tax paid on
// every line of every script, for information the runtime already has.
//
// So a script sees plain data:
//
//     bool        true
//     integer     7
//     number      2.5
//     text        "wall"
//     point       {"x":0,"y":0,"z":0}
//     polyline    [ <point>, ... ]      and a polygon the same
//     element     {"elementGuid":"..."}
//     mesh        {"isMesh":true,"vertexCount":N,"triangleCount":M}
//     list        [ <value>, ... ]
//     any         whatever it already was
//
// ⚠️ AND THE JAVASCRIPT ENGINE IMPLEMENTS THIS SAME SHAPE DIRECTLY AGAINST ITS
// OWN API rather than going through here, because building a JSON string only to
// have QuickJS parse it again would be pure cost. That is a real drift risk, and
// it is why test_nodegraph_script.cpp runs one value of every type through BOTH
// paths and compares. If the two ever disagree, a script that works in one
// language stops working when translated to the other - which is the one thing
// this node family promises will not happen.
//
// ⚠️ A MESH IS READABLE, NEVER CONSTRUCTIBLE. Reconstructing geometry from
// whatever a script left in a variable is how a graph acquires meshes with three
// vertices and no normals, failing far downstream in the renderer. An element is
// refused for a different reason: it is a REFERENCE to something in the model,
// and a script must not be able to invent one.

#include "NodeGraph/Json.hpp"
#include "NodeGraph/Value.hpp"

#include <string>

namespace evp::nodegraph {

json::JsonValue ScriptValueToJson (const Value& value);
json::JsonValue ScriptValueToJson (const Argument& value);

// `expected` comes from the port's declared type; the script's own value is
// coerced to it or refused with a reason naming what was wanted.
bool ScriptValueFromJson (const json::JsonValue& source, ValueType expected, Value& out, std::string& error);
bool ScriptValueFromJson (const json::JsonValue& source, ValueType expected, Argument& out, std::string& error);

} // namespace evp::nodegraph

#endif
