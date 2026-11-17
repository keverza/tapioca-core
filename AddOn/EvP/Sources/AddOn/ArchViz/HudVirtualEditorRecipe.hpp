#ifndef TAPIOCA_HUD_VIRTUAL_EDITOR_RECIPE_HPP
#define TAPIOCA_HUD_VIRTUAL_EDITOR_RECIPE_HPP

// The Outliner's Recipe view: the terrain recipe (#105 slice 1). See Edit/EditLandscapeRecipe.hpp.
#include "Edit/EditLandscapeRecipe.hpp"

namespace geomsrv::archviz::edithud {
struct Session;
// The recipe for the current document revision and design request, recompiled only when
// either changes.
const tapioca::geometry::LandscapeRecipe& CurrentRecipe (Session& session);
// Layout only; a row click selects the row's line in the view and nothing else.
void RecipePanel (Session& session);
// #191 U5: why moving region `object` by `step` (-1 up, +1 down) would be refused, or empty when it
// would move. Worked out on a copy of the document once per revision; the arrow is disabled and
// says why in its tooltip.
std::string RecipeMoveRefusal (Session& session, tapioca::geometry::Id object, int step);
// #220: why turning footprint step `operation` off would be refused, or empty when it may be. Only
// a region's root whose bypass leaves no enabled step to build from refuses; turning a step back
// on never does. Both "Footprint on" switches are disabled and say why in their tooltip.
std::string RecipeBypassRefusal (const Session& session, tapioca::geometry::Id operation);
} // namespace geomsrv::archviz::edithud
#endif
