#ifndef EVP_EDIT_EDITSHAPE_HPP
#define EVP_EDIT_EDITSHAPE_HPP

// The drawn shape of a polyline or an extrusion's profile. Its vertices are anchors; a
// live corner (Vertex::radius) is rounded by an arc tangent to both legs and stays a
// radius -- editable, never turned into points until the user bakes it. Everything that
// shows, fills or snaps to the outline reads it from here, so they cannot disagree.
#include "Edit/EditDocument.hpp"
#include <vector>

namespace geomsrv::edit {

// Vertex `i` as a corner: unit plan directions to its neighbours, the unit bisector into
// the corner, half the angle between the legs and the legs' plan lengths. False at an
// open end, and where the legs are degenerate or run straight on.
struct CornerGeometry {
    size_t prev = 0, at = 0, next = 0;
    Vec3 corner, toPrev, toNext, bisector;
    double half = 0.0;
    double legPrev = 0.0, legNext = 0.0;
};
bool CornerOf (const Entity& entity, size_t i, CornerGeometry& out);

// How far along each leg a rounding of `radius` reaches from the corner.
double TangentLength (const CornerGeometry& corner, double radius);
// The largest radius the corners `set` can all take at once, every other corner keeping
// its own: each leg holds both its ends' roundings. 0 when one of them is not a corner.
double MaxRadius (const Entity& entity, const std::vector<size_t>& set);
// The radius drawn at vertex `i`: its own, scaled down where moved vertices left a leg
// too short for both its roundings (an edit never stores one that does not fit).
double DrawnRadius (const Entity& entity, size_t i);

// The outline as drawn: anchors, with every live corner replaced by its arc, at most
// `maxAngle` radians a step. Closed when the entity is (the first point is not repeated).
std::vector<Vec3> Shape (const Entity& entity, double maxAngle = 0.15);

// A corner's grip: on its bisector at the arc's middle, or `minInset` metres into a
// sharp corner. RadiusFromGrip inverts it: the radius whose arc's middle is where `p`
// falls on the bisector (0 inside `minInset`).
Vec3 CornerGripAt (const Entity& entity, size_t i, double minInset);
double RadiusFromGrip (const Entity& entity, size_t i, Vec3 p, double minInset);

// Sets the live radius of each of `vertices` (0 makes a corner sharp again). Refused when
// one is not a corner, is locked, or the radius does not fit (the message says how much does).
bool SetCornerRadius (Document& doc, Id entity, const std::vector<Id>& vertices, double radius, std::string& error);

} // namespace geomsrv::edit
#endif
