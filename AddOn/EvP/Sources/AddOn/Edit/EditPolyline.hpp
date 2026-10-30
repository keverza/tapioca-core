#ifndef EVP_EDIT_EDITPOLYLINE_HPP
#define EVP_EDIT_EDITPOLYLINE_HPP

// Shape edits on polylines and extrusion profiles. Each one edits the document in
// place or refuses with a reason, so it can run inside a gesture (from the gesture's
// start snapshot) or as a one-shot action. Plan operations work in XY; heights ride
// along on the segments they came from. Ids survive wherever the item survives:
// inserting a vertex keeps the split segment's id on its first half.
#include "Edit/EditSelection.hpp"
#include <functional>
#include <string>
#include <vector>

namespace geomsrv::edit {

std::vector<Vec3> Rectangle (Vec3 corner, Vec3 opposite);

bool MoveVertex (Document& doc, Id entity, Id vertex, Vec3 to, std::string& error);
bool InsertVertex (Document& doc, Id entity, Id segment, Vec3 at, std::string& error, Id* created = nullptr);
bool DeleteVertex (Document& doc, Id entity, Id vertex, std::string& error);
// Removes vertices closer than `tolerance` to the next one.
bool MergeVertices (Document& doc, Id entity, double tolerance, std::string& error);
// Moves a segment parallel to itself by `distance` (positive to its left in plan);
// its neighbours keep their directions and stretch to meet it.
bool OffsetSegment (Document& doc, Id entity, Id segment, double distance, std::string& error);
// A parallel copy of the whole outline at `distance` (positive to the left in plan).
bool OffsetEntity (Document& doc, Id entity, double distance, std::string& error, Id* created = nullptr);
// Lengthens an open polyline's start or end along its last segment to the first
// edge of `target` it meets.
bool ExtendToEntity (Document& doc, Id entity, bool atStart, Id target, std::string& error);
// Cuts an open polyline back from its start or end to the first edge of `target`.
bool TrimAtEntity (Document& doc, Id entity, bool atStart, Id target, std::string& error);
// An open polyline splits in two at an interior vertex; a closed one opens there.
bool SplitAtVertex (Document& doc, Id entity, Id vertex, std::string& error, Id* created = nullptr);
// Joins two open polylines whose ends meet within `tolerance`; meeting at both ends closes it.
bool Join (Document& doc, Id first, Id second, double tolerance, std::string& error);
// Runs a polyline the other way: vertices, segments and Bezier handles reversed, so the
// drawn shape is unchanged and every id stays on its own item.
bool ReverseDirection (Document& doc, Id entity, std::string& error);
// Closes an open polyline with a new segment from its last vertex to its first; a last
// vertex lying on the first merges into it.
bool Close (Document& doc, Id entity, std::string& error);
// Opens a closed polyline by removing its closing segment (last vertex back to the first).
bool Open (Document& doc, Id entity, std::string& error);
// Rounds a corner with an arc of `radius`, drawn as `arcSegments` straight pieces.
bool Fillet (Document& doc, Id entity, Id vertex, double radius, int arcSegments, std::string& error);
bool Chamfer (Document& doc, Id entity, Id vertex, double distance, std::string& error);
// Sets one coordinate (0 X, 1 Y, 2 Z) of every vertex the refs reach.
bool SetCoordinate (Document& doc, const std::vector<Ref>& refs, int axis, double value, std::string& error);
using HeightField = std::function<bool (double x, double y, double& z)>;
// Drops every vertex of a polyline onto the surface; refuses where there is none.
bool ProjectToSurface (Document& doc, Id entity, const HeightField& height, std::string& error);

struct ActionDefaults {
    double filletRadius = 0.5;
    double chamferDistance = 0.5;
    double mergeTolerance = 0.01;
    double joinTolerance = 0.01;
    double pathWidth = 1.5; // a path made along a polyline
    // An offset copy's distance: positive outside a closed outline, to the left of an open line.
    double offsetDistance = 0.5;
    bool operator== (const ActionDefaults&) const = default;
};
// The shape editor's context actions, ready for `Applicable`.
std::vector<Action> PolylineActions (const ActionDefaults& defaults = {});

} // namespace geomsrv::edit
#endif
