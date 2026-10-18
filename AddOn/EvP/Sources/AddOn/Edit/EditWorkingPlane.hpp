#ifndef EVP_EDIT_EDITWORKINGPLANE_HPP
#define EVP_EDIT_EDITWORKINGPLANE_HPP

#include "Edit/EditSnap.hpp"

namespace geomsrv::edit {

enum class PlaneSource : uint8_t { WorldXY, Explicit, Face, PathPlan, PathSection };
enum class PathPlaneKind : uint8_t { Plan, Section };

// World metres and a right-handed orthonormal basis. This input frame is not the
// gizmo's pivot/frame, nor a new view/camera. Hosts may support fewer sources.
struct WorkingPlane {
    Vec3 origin;
    Vec3 u { 1, 0, 0 }, v { 0, 1, 0 }, normal { 0, 0, 1 };
    PlaneSource source = PlaneSource::WorldXY;
    bool sticky = true, locked = false;
    // Pick-time provenance only, not a live binding: face indices are session-local.
    Ref sourceRef;
    uint64_t sourceRevision = 0;
    double sourceStation = 0.0; // plan metres from the source path's start, captured at pick time
};
WorkingPlane WorldXY (double z = 0.0);
bool PlaneFromFace (const Document& doc, Ref face, WorkingPlane& out, std::string& error);
// Plan is horizontal (U=tangent, V=left), not tilted to grade/crossfall. Section is vertical
// (U=left, V=world Z). Both use the evaluated profile centre; neither is a live path binding.
// Refuse out-of-range/nonfinite stations and the ambiguous tangent at a sharp corner; no output on failure.
bool PlaneFromPath (const Document& doc, Id path, double station, PathPlaneKind kind, WorkingPlane& out,
                    std::string& error);
bool ValidPlane (const WorkingPlane& plane);
bool IsWorldXY (const WorkingPlane& plane);
bool SamePlaneFrame (const WorkingPlane& a, const WorkingPlane& b);
bool SamePlane (const WorkingPlane& a, const WorkingPlane& b);
Plane SnapPlane (const WorkingPlane& plane);
Vec3 ToLocal (const WorkingPlane& plane, Vec3 world);
Vec3 ToWorld (const WorkingPlane& plane, Vec3 local);
// Refuse an invalid frame/ray, an edge-on plane or a hit behind the ray. No output on failure.
bool HitPlane (const WorkingPlane& plane, Vec3 origin, Vec3 direction, Vec3& out);
// Explicit changes can override a lock; automatic adoption cannot. Refusal leaves `plane` intact.
bool ChangePlane (WorkingPlane& plane, const WorkingPlane& next, bool automatic, std::string& error);

} // namespace geomsrv::edit
#endif
