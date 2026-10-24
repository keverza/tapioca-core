#ifndef EVP_EDIT_EDITIMPORT_HPP
#define EVP_EDIT_EDITIMPORT_HPP

// Host 2D drafting brought into the editor: lines, polylines, arcs and circles, and the contours
// of fills, as the native reader hands them over in plan. Each becomes one ordinary polyline --
// arcs as cubic pieces of at most a quarter turn -- edited as any line drawn here; nothing stays
// bound to the element it came from.
#include "Edit/EditDocument.hpp"
#include <string>
#include <vector>

namespace geomsrv::edit {

// A run of points and, per segment (point i to i + 1, wrapping when closed), its signed arc angle
// in radians: positive turns counter-clockwise, 0 is straight. `arcs` empty: every segment straight.
struct ImportedCurve {
    std::vector<Vec3> points;
    std::vector<double> arcs;
    bool closed = false;
};

// An ellipse or an arc of one: its centre, the radius along its main axis and the other axis as a
// fraction of it, the main axis's direction, and -- unless whole -- the parameter angles it runs
// between, counter-clockwise (the angles of the circle the ellipse is that circle scaled from).
struct ImportedEllipse {
    Vec3 centre;
    double radius = 0.0, ratio = 1.0, axisAngle = 0.0, from = 0.0, to = 0.0;
    bool whole = true;
};

// Kept within this of the true arc by the quarter-turn split (a fraction of its radius).
constexpr double kImportArcError = 3.0e-4;

// Adds the curve or the ellipse at its points' heights; kNoId and a reason when it is degenerate
// (fewer than two distinct points, an arc with no chord, a non-finite value), the document unchanged.
Id AddImportedCurve (Document& doc, const ImportedCurve& curve, const std::string& role, std::string& error);
Id AddImportedEllipse (Document& doc, const ImportedEllipse& ellipse, const std::string& role, std::string& error);

} // namespace geomsrv::edit
#endif
