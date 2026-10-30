#ifndef EVP_EDIT_EDITELLIPSE_HPP
#define EVP_EDIT_EDITELLIPSE_HPP

#include "Edit/EditDocument.hpp"
#include <string>

namespace geomsrv::edit {

// Editable parameters in a deterministic frame on the ellipse's plane. Rotation is
// radians from the frame's U axis to the major axis; horizontal ellipses use world X.
struct EllipseParameters {
    EllipseMode mode = EllipseMode::Ellipse;
    Vec3 center;
    Vec3 normal { 0.0, 0.0, 1.0 };
    double majorRadius = 0.0, minorRadius = 0.0;
    double rotation = 0.0;
};

// Regenerates the four existing cubic quarter-arcs from the authoritative definition.
// Vertex and segment ids, locks and entity metadata are retained.
bool RegenerateEllipse (Entity& entity, std::string& error);
// Checks both the definition and its generated four-quarter representation.
bool ValidateEllipse (const Entity& entity, std::string& error);
bool GetEllipseParameters (const Entity& entity, EllipseParameters& parameters, std::string& error);

// Exact typed parameter setters. They refuse locked anchors, invalid radii and attempts
// to make a circle from unequal radii. Apply through Session to get one undo step.
bool SetEllipseParameters (Document& doc, Id entity, const EllipseParameters& parameters, std::string& error);
bool SetEllipseCenter (Document& doc, Id entity, Vec3 center, std::string& error);
bool SetEllipseRadii (Document& doc, Id entity, double majorRadius, double minorRadius, std::string& error);
bool SetEllipseRotation (Document& doc, Id entity, double radians, std::string& error);
bool SetEllipseMode (Document& doc, Id entity, EllipseMode mode, std::string& error);

// Explicit conversion leaves the current four-quarter Bezier shape in place and clears
// its semantic definition, after which ordinary node/handle/topology edits are allowed.
bool ConvertEllipseToFreeform (Document& doc, Id entity, std::string& error);

} // namespace geomsrv::edit
#endif
