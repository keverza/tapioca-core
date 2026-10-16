#ifndef EVP_EDIT_EDITPATHEDIT_HPP
#define EVP_EDIT_EDITPATHEDIT_HPP

// Edits on a path inside the document. Every one goes through the same step: apply
// the change to a copy, rebuild the plan alignment, move the profile and the ranges
// with any change of length (anchored grade breaks follow their control points),
// validate, and only then write back. A refused edit leaves the path untouched.
#include "Edit/EditDocument.hpp"
#include <functional>
#include <string>
#include <vector>

namespace geomsrv::edit {

// A path through plan points, its profile from the first and last points' Z, and one
// cross-section range `width` wide centred on it.
Id AddPath (Document& doc, const std::vector<Vec3>& points, double width, const std::string& role = {});

bool EditPath (Document& doc, Id path, const std::function<bool (Path&, std::string&)>& change, std::string& error);

bool MovePathPoint (Document& doc, Id path, Id point, Vec3 to, std::string& error);
// A control point at the plan position of `station`; the plan does not change shape.
bool InsertPathPoint (Document& doc, Id path, double station, std::string& error, Id* created = nullptr);
bool DeletePathPoint (Document& doc, Id path, Id point, std::string& error);
bool SetPathRadius (Document& doc, Id path, Id point, double radius, std::string& error);
// Sets the bend radius of each of `points` at once (a whole path's bends, dragged together).
bool SetPathRadii (Document& doc, Id path, const std::vector<Id>& points, double radius, std::string& error);
// A path's centreline as an open polyline: its control points as vertices (same ids, at
// their profile heights) and their radii as live corners, so the live-corner geometry
// (EditShape.hpp: grips, the largest radius that fits) applies to its bends unchanged.
Entity Centreline (const Path& path);
// A path along an open polyline of lines and rounded corners, `width` wide: its corners'
// radii become the path's bends, its heights the profile (a break where a vertex is off the
// grade). The polyline goes. kNoId and why when it is closed or has curves.
Id PathFromPolyline (Document& doc, Id entity, double width, std::string& error);

bool SetPathElevation (Document& doc, Id path, Id gradeBreak, double z, std::string& error);
// The grade of the stretch that contains `station`, in percent.
bool SetPathGrade (Document& doc, Id path, double station, double percent, std::string& error);
bool InsertPathBreak (Document& doc, Id path, double station, std::string& error, Id* created = nullptr);
bool DeletePathBreak (Document& doc, Id path, Id gradeBreak, std::string& error);
bool MovePathBreak (Document& doc, Id path, Id gradeBreak, double station, std::string& error);
bool SetPathCurve (Document& doc, Id path, Id gradeBreak, double length, std::string& error);
// Locks an elevation at a station: an existing break there is set, otherwise one is
// inserted. `binding` records the host item it came from (a door, a kerb).
bool FixElevationAt (Document& doc, Id path, double station, double z, const std::string& binding, std::string& error,
                     Id* created = nullptr);
bool UnfixBreak (Document& doc, Id path, Id gradeBreak, std::string& error);
// The break follows the control point's station when the plan changes.
bool AnchorBreak (Document& doc, Id path, Id gradeBreak, Id point, std::string& error);
// Replaces the free breaks with ones following the surface along the path; fixed
// breaks are kept and the surface is followed between them.
bool ProfileFromSurface (Document& doc, Id path, const std::function<bool (double, double, double&)>& height,
                         double step, double tolerance, std::string& error);

bool SplitPathSection (Document& doc, Id path, double station, std::string& error, Id* created = nullptr);
bool DeletePathSection (Document& doc, Id path, Id range, std::string& error);
bool SetPathWidth (Document& doc, Id path, Id range, double left, double right, std::string& error);
// The path's kerb radius (Path::edgeRadius): all its own corners, those set apart included.
bool SetPathEdgeRadius (Document& doc, Id path, double radius, std::string& error);
// A dragged edge: the plan point's offset from the centreline becomes that side's width
// in the range in force where the point lies.
bool SetPathWidthFromPoint (Document& doc, Id path, Vec3 point, std::string& error);
bool SetPathCrossfall (Document& doc, Id path, Id range, double percent, Fall fall, std::string& error);

// The gizmo on path parts: control points move in plan, grade breaks move in Z, a
// whole path moves its plan and lifts its free breaks.
bool TransformPathParts (Document& doc, const std::vector<Ref>& refs, Vec3 translate, std::string& error);
// Turns or scales whole paths and control points in plan: each control point goes where
// `map` sends it, corner radii are multiplied by `radiusFactor` (a uniform scale keeps the
// shape), and no height changes -- the profile follows by station. Grade breaks and ranges
// refuse: they are placed along the path, not in plan.
bool MapPathParts (Document& doc, const std::vector<Ref>& refs, const std::function<Vec3 (Vec3)>& map,
                   double radiusFactor, std::string& error);

} // namespace geomsrv::edit
#endif
