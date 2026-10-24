#ifndef EVP_EDIT_EDITPRIMITIVES_HPP
#define EVP_EDIT_EDITPRIMITIVES_HPP

// Closed shapes drawn from two points on a working plane: a rectangle corner to corner, a regular
// polygon from its centre to a vertex, an ellipse from its centre to a corner of its bounding box.
// Each becomes one ordinary closed polyline -- its vertices, segments and Bezier handles edited as
// any other -- never a parametric object the editor has to keep alive.
#include "Edit/EditDocument.hpp"
#include <string>
#include <vector>

namespace geomsrv::edit {

enum class Primitive : uint8_t { Rectangle, Polygon, Ellipse };

struct PrimitiveInput {
    Primitive kind = Primitive::Rectangle;
    Vec3 first, second;                // the two clicks, on the plane
    Vec3 u { 1, 0, 0 }, v { 0, 1, 0 }; // the plane's axes: a rectangle's sides, an ellipse's axes
    int sides = 6;                     // a polygon's, 3..kMaxPolygonSides
    // A square, a circle; ignored by a polygon, which is always regular.
    bool uniform = false;
};
constexpr int kMaxPolygonSides = 64;

struct PrimitiveOutline {
    std::vector<Vec3> points; // counter-clockwise about the plane's normal, closed implicitly
    // Bezier handles as plan offsets from their point (Vertex::in/out); empty for straight sides.
    std::vector<Vec3> in, out;
};

// False with a reason for a degenerate shape (no size, or a side count out of range).
bool OutlinePrimitive (const PrimitiveInput& input, PrimitiveOutline& out, std::string& error);
// Adds the shape as a closed polyline (smooth vertices for an ellipse); kNoId and a reason on failure,
// with the document unchanged.
Id AddPrimitive (Document& doc, const PrimitiveInput& input, const std::string& role, std::string& error);

} // namespace geomsrv::edit
#endif
