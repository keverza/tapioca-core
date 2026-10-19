#ifndef EVP_EDIT_EDITPATHSURFACE_HPP
#define EVP_EDIT_EDITPATHSURFACE_HPP
#include "Edit/EditPathNetwork.hpp"
#include <functional>

namespace geomsrv::edit {
// Shared-edge refinement of the united footprint; authored samples retain profile
// features that an outline-only triangulation cannot describe.
void RefinePavedSurface (PavedArea& area, const std::vector<Vec3>& samples, double step,
                         const std::function<double (Vec3)>& height);
} // namespace geomsrv::edit
#endif
