#ifndef EVP_EDIT_EDITLANDSCAPEENTRANCE_HPP
#define EVP_EDIT_EDITLANDSCAPEENTRANCE_HPP

// A building outline's entrances: the places on it a path must reach, each at its threshold
// height. An entrance is held as a distance along the outline (so it follows the outline's
// vertices as they move) and, when it came from one, the Archicad door it stands for.
#include "Edit/EditLandscape.hpp"

namespace tapioca::geometry {

// The binding a path's end break carries once it reaches an entrance: "entrance:<id>".
std::string EntranceBinding (Id entrance);

// Adds an entrance where `near` projects onto the Building source `source`'s outline, at
// threshold height `z`. One document edit. Refuses a source that is not a live closed
// building outline, or a non-finite point or height.
bool AddLandscapeEntrance (geomsrv::edit::Document& doc, Id source, Vec3 near, double z, const std::string& doorGuid,
                           Id& entrance, std::string& error);
bool RemoveLandscapeEntrance (geomsrv::edit::Document& doc, Id entrance, std::string& error);
// The entrance's plan point on its outline as the outline is now, at its threshold height.
bool LandscapeEntrancePoint (const geomsrv::edit::Document& doc, const LandscapeEntrance& entrance, Vec3& at);
// The entrance nearest `point` in plan within `reach` metres; null when none is.
const LandscapeEntrance* EntranceNear (const geomsrv::edit::Document& doc, Vec3 point, double reach);
// The building source whose outline passes nearest `point` in plan, within `reach` metres;
// kNoId when none does (a door read from Archicad finds its building this way).
Id BuildingNear (const geomsrv::edit::Document& doc, Vec3 point, double reach);
// The path's end nearer the entrance moves onto it, and that end's grade break takes the
// threshold height, fixed and bound to the entrance (EntranceBinding). One document edit.
bool ReachLandscapeEntrance (geomsrv::edit::Document& doc, Id path, Id entrance, std::string& error);

} // namespace tapioca::geometry
#endif
