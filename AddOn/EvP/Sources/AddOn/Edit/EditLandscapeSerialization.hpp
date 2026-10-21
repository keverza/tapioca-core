#ifndef TAPIOCA_EDIT_LANDSCAPE_SERIALIZATION_HPP
#define TAPIOCA_EDIT_LANDSCAPE_SERIALIZATION_HPP

#include "Edit/EditDocument.hpp"
#include <functional>
#include <map>

namespace tapioca::geometry {
enum class LandscapeStorage : uint8_t { Virtual, Native };
// Native reload requires a read-only capture keyed by authored point IDs. A
// matching vertex count or ordering is not a correspondence proof. The codec
// neither calls the SDK nor writes project metadata.
using LandscapeCapture = std::function<bool (const LandscapeSource&, std::map<Id, Vec3>&, std::string&)>;
bool WriteLandscape (const geomsrv::edit::Document& doc, LandscapeStorage mode, std::string& text, std::string& error);
bool ReadLandscape (const std::string& text, geomsrv::edit::Document& doc, std::string& error,
                    const LandscapeCapture& capture = {});
} // namespace tapioca::geometry
#endif
