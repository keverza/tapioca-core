#ifndef TAPIOCA_EDIT_LANDSCAPE_TERRAIN_INTERNAL_HPP
#define TAPIOCA_EDIT_LANDSCAPE_TERRAIN_INTERNAL_HPP

#include "Edit/EditLandscapeInternal.hpp"
#include "Edit/EditLandscapeTerrain.hpp"

namespace tapioca::geometry::landscape_terrain_detail {

// Plan masks of one operation's reached constraint scope, clipped to the property
// line. Grading-limit envelopes and positional protection stay separate sets.
struct ScopeMasks {
    landscape_detail::cp::PathsD property, allowed, protectedPaths;
    bool envelope = false; // A reached grading limit narrows `allowed` below `property`.
};

// Solve-application entry over the field-edit path's own mask derivation:
// evaluates the planar DAG, checks the operation's unmixed scope and the retained
// input catalog, then derives the masks. No terrain math; Clipper2 only
// intersects 2D guides.
bool DeriveScopeMasks (const geomsrv::edit::Document& doc, const ProvenanceRequest& provenance, OperationId operation,
                       LandscapeResult& landscape, ScopeMasks& masks, std::string& error);

} // namespace tapioca::geometry::landscape_terrain_detail
#endif
