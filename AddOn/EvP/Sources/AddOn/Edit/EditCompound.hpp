#ifndef EVP_EDIT_EDITCOMPOUND_HPP
#define EVP_EDIT_EDITCOMPOUND_HPP

// Meshes of compound entities (EditDocument.hpp): a profile with holes as a cap and as a solid.
// The caps come from the hole-preserving planar triangulator, never from a single-ring ear clip,
// so a hole is never filled; a cover that would drop or approximate a hole refuses instead.
#include "Edit/EditDocument.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::edit {

// The drawn contours of `entity` (EditShape.hpp ShapeRings, outer first) concatenated into
// `points`, and triangles over them that cover the even-odd fill, counter-clockwise seen from
// above. A simple entity is one ring. False, outputs cleared, when no exact cover keeps every hole.
bool CapTriangles (const Entity& entity, std::vector<Vec3>& points, std::vector<uint32_t>& triangles,
                   std::string& error);

// An extrusion as a closed triangle mesh: the cap's points at the base then the same at the top,
// the bottom facing down, the top up, and a wall up every contour facing out of the solid (into
// a hole for a hole's wall). False when the cap cannot be covered (CapTriangles) or the entity is
// not an extrusion.
bool SolidTriangles (const Entity& entity, std::vector<Vec3>& points, std::vector<uint32_t>& triangles,
                     std::string& error);

} // namespace geomsrv::edit
#endif
