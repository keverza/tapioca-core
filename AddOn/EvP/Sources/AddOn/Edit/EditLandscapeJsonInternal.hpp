#ifndef TAPIOCA_EDIT_LANDSCAPE_JSON_INTERNAL_HPP
#define TAPIOCA_EDIT_LANDSCAPE_JSON_INTERNAL_HPP

#include "Edit/EditLandscapeSerialization.hpp"
#include "NodeGraph/Json.hpp"
#include <initializer_list>
#include <stdexcept>

namespace tapioca::geometry::landscape_json {
// Reuse the repository's SDK-free JSON value/parser; the landscape boundary
// adds strict lexical validation, field checking and unsigned decimal IDs.
namespace json = evp::nodegraph::json;
using Value = json::JsonValue;
using Array = json::JsonArray;
using Object = json::JsonObject;
inline Value IdValue (Id id)
{
    return Value::String (std::to_string (id));
}
Value IdsValue (const std::vector<Id>& ids);
Value PointValue (Vec3 point);
Value MaterialValue (const MaterialSpec& material);
Value DescriptorValue (const RegionDescriptor& descriptor);
Value DocumentValue (const geomsrv::edit::Document& doc, LandscapeStorage mode);

[[noreturn]] inline void Fail (const std::string& path, const std::string& reason)
{
    throw std::runtime_error (path + ": " + reason);
}
class Reader {
  public:
    Reader (const Value& value, std::string path) : value (value), path (std::move (path))
    {
    }
    Reader Field (const std::string& name) const;
    bool Has (const std::string& name) const
    {
        return value.Find (name) != nullptr;
    }
    void Fields (std::initializer_list<const char*> names, std::initializer_list<const char*> optional = {}) const;
    const Array& Items () const;
    std::string Text () const;
    Id Identifier () const;
    double Number () const;
    int Integer () const;
    bool Flag () const;
    Vec3 Point () const;
    std::vector<Id> Ids () const;
    MaterialSpec Material () const;
    RegionDescriptor Descriptor () const;
    const Value& value;
    std::string path;
};
bool StrictJson (const std::string& text, std::string& normalized, std::string& error);
void ReadDocument (const Reader& root, geomsrv::edit::Document& doc, LandscapeStorage mode);
} // namespace tapioca::geometry::landscape_json
#endif
