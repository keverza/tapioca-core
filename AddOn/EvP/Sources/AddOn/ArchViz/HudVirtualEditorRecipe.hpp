#ifndef TAPIOCA_HUD_VIRTUAL_EDITOR_RECIPE_HPP
#define TAPIOCA_HUD_VIRTUAL_EDITOR_RECIPE_HPP

// The Operations tab's read-only terrain recipe (#105 slice 1). See Edit/EditLandscapeRecipe.hpp.
#include "Edit/EditLandscapeRecipe.hpp"

namespace geomsrv::archviz::edithud {
struct Session;
// The recipe for the current document revision and design request, recompiled only when
// either changes.
const tapioca::geometry::LandscapeRecipe& CurrentRecipe (Session& session);
// Layout only; a row click selects the row's line in the view and nothing else.
void RecipePanel (Session& session);
} // namespace geomsrv::archviz::edithud
#endif
