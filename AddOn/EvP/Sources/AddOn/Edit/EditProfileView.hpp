#ifndef EVP_EDIT_EDITPROFILEVIEW_HPP
#define EVP_EDIT_EDITPROFILEVIEW_HPP

// A path's longitudinal profile as a flat view: stations run right, elevations run up,
// stretched vertically so a gentle grade can be seen and dragged. The panel host draws
// it and routes its pointer here; the plan and 3D hosts never see this view.
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditSession.hpp"
#include <functional>
#include <string>
#include <vector>

namespace geomsrv::edit {

struct ProfileView {
    double left = 0.0, top = 0.0, width = 400.0, height = 160.0; // the canvas in pixels
    double stationMin = 0.0, stationMax = 1.0, zMin = 0.0, zMax = 1.0;
    double exaggeration = 1.0; // vertical pixels per metre over horizontal pixels per metre
};
// Fits the whole profile (and the terrain heights, when given) inside the canvas with
// a margin, scaling heights independently (including compression for steep profiles)
// up to `maxExaggeration` so the elevations fit. The vertical axis remains linear.
ProfileView FitProfile (const Path& path, double left, double top, double width, double height, double margin,
                        const std::vector<std::pair<double, double>>& terrain = {}, double maxExaggeration = 20.0);
void ProfileToScreen (const ProfileView& view, double station, double z, double& x, double& y);
void ProfileFromScreen (const ProfileView& view, double x, double y, double& station, double& z);

struct ProfileHit {
    // A curve hit is one of a break's two smoothing nubs (CurveNubs).
    enum class Kind : uint8_t { Break, Tangent, Curve } kind = Kind::Break;
    size_t index = 0; // the break, or the tangent from break `index` to `index + 1`
    double distance = 0.0;
};
// Breaks, then curve nubs, then tangents, then nearest; within `radius` pixels. `nubPixels` is the
// least distance a sharp break's nubs stand out from it (CurveNubs).
std::vector<ProfileHit> PickProfile (const Path& path, const ProfileView& view, double x, double y, double radius,
                                     double nubPixels = 16.0);

// ⚠️ A GRADE BREAK'S SMOOTHING IS A FILLET'S GRIP (the user, 2026-10-24). Its vertical curve's two
// ends lie on its two tangents, `curve / 2` metres of station either side; a nub stands at each,
// and dragging one along its tangent sets the length the curve rounds the grade change over.
// A sharp break's nubs stand `nubPixels` out on screen, so there is something to grab. Interior
// breaks only: the end breaks carry no curve. (station, elevation) of the backward and forward nub.
bool CurveNubs (const Path& path, const ProfileView& view, size_t index, double nubPixels,
                std::pair<double, double> nubs[2]);
// The longest curve break `index` can take: the room its tangents leave after the neighbouring
// breaks' own half curves, so curves never overlap. 0 for an end break.
double MaxCurve (const Path& path, size_t index);
// The curve length a nub dragged to `station` asks for: twice its distance from the break, 0 within
// half a sharp nub's offset, clamped to MaxCurve.
double CurveFromNub (const Path& path, const ProfileView& view, size_t index, double station, double nubPixels = 16.0);
// Sets the break's curve length; clamped to [0, MaxCurve] when the drag ran past it.
Operation DragCurve (Id path, Id gradeBreak, double length);

struct ProfileSnap {
    double stationStep = 0.0;    // metres; 0 leaves stations free
    double zStep = 0.01;         // metres
    double gradeStep = 0.5;      // percent: adjacent grades snap to multiples of this
    double gradeTolerance = 0.1; // percent
};
// Where a dragged break lands: grades on either side snap to round percentages when
// close, otherwise the elevation snaps to `zStep`. `hint` says which won.
void SnapBreak (const Path& path, size_t index, double& station, double& z, const ProfileSnap& snap, std::string& hint);

// Gesture operations, built from the position the drag reached. A break moves in
// station and elevation (a fixed break only along the station, an anchored one only
// in height); a tangent moves up or down with both of its breaks.
Operation DragBreak (Id path, Id gradeBreak, double station, double z);
Operation DragTangent (Id path, size_t index, double dz);

// What the panel draws, in (station, elevation): the profile, the terrain under it,
// the breaks (fixed ones marked), vertical-curve extents and a grade label per tangent.
struct ProfileDrawing {
    std::vector<std::pair<double, double>> line, terrain;
    struct Marker {
        double station = 0.0, z = 0.0;
        bool fixed = false;
        Id id = kNoId;
    };
    std::vector<Marker> breaks;
    struct Curve {
        double from = 0.0, to = 0.0;
    };
    std::vector<Curve> curves;
    struct Label {
        double station = 0.0, z = 0.0, percent = 0.0;
    };
    std::vector<Label> grades;
};
ProfileDrawing DrawProfile (const Path& path, double step, const std::vector<std::pair<double, double>>& terrain = {});

} // namespace geomsrv::edit
#endif
