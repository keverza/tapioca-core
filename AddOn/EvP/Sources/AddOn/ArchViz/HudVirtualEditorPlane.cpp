#include "ArchViz/HudVirtualEditorDetail.hpp"
#include <cstdio>
#include <cmath>

// OVERLAY-INVARIANTS.md: shared cancellation of view-owned input only; no camera or native writes.
namespace geomsrv::archviz::edithud::detail {

// Projection/window changes cancel view gestures but keep unfinished model-space drawing
// points. The profile's drags belong to its independent panel canvas.
void CancelViewEdit (Session& s, const char* reason)
{
    bool cancelled = s.typedHandle >= 0 || s.labelOpen || s.penDown || s.planeTyping;
    if (s.drag != Drag::None && s.drag != Drag::ProfileBreak && s.drag != Drag::ProfileTangent) {
        cancelled = cancelled || s.drag != Drag::Marquee || s.edits.InGesture ();
        s.edits.Cancel ();
        s.drag = Drag::None;
        s.dragStarted = false;
        s.wholeGrab = false;
    }
    if (s.tracker.State () == edit::TrackerState::Editing)
        s.tracker.Key ({ edit::TrackerKey::Kind::Escape }), cancelled = true;
    s.typedHandle = -1;
    s.planeTyping = false;
    s.planeEntryPath = edit::kNoId;
    s.planeEntryRevision = 0;
    s.labelOpen = false;
    s.penDown = false;
    s.snap = {};
    s.selection.hover = {};
    s.hoverHandle = s.hoverCorner = s.hoverKerb = s.hoverBezier = -1;
    s.navigation = {};
    s.dragReleased = false;
    s.dragPointerPause.reset ();
    if (cancelled)
        s.message = reason;
}

} // namespace geomsrv::archviz::edithud::detail

namespace geomsrv::archviz::edithud {

bool SetWorkingPlane (Session& s, const edit::WorkingPlane& plane, bool automatic)
{
    const bool horizontalSource =
        edit::ValidPlane (plane) &&
        (plane.source == edit::PlaneSource::Face || plane.source == edit::PlaneSource::PathPlan) && plane.u.z == 0 &&
        plane.v.z == 0 && std::abs (plane.normal.z) == 1;
    if ((!edit::IsWorldXY (plane) && !horizontalSource) || !plane.sticky) {
        s.message = plane.source == edit::PlaneSource::PathSection
                        ? "Vertical path-section authoring is unavailable; use a horizontal drawing plane along "
                          "the path."
                        : "Use World XY or a horizontal drawing plane on a face or along a path; side/oblique "
                          "plane authoring is unavailable.";
        return false;
    }
    auto next = s.plane;
    std::string error;
    if (!edit::ChangePlane (next, plane, automatic, error)) {
        s.message = error;
        return false;
    }
    if (edit::SamePlane (s.plane, next))
        return true;
    if (edit::SamePlaneFrame (s.plane, next)) {
        // Locking the current frame is not a new geometric datum: keep its input/preview.
        s.plane = next;
        if (s.capturedPlane)
            s.capturedPlane = next;
        return true;
    }
    const bool unfinished = s.drag != Drag::None || s.penDown || s.planeTyping || !s.drawing.empty () ||
                            s.tracker.State () == edit::TrackerState::Editing || s.labelOpen || s.typedHandle >= 0;
    detail::CancelViewEdit (s, "The drawing plane changed; unfinished input was cancelled.");
    // Unlike a view switch, a plane change would mix authoring frames inside one unfinished shape.
    s.edits.Cancel ();
    s.drag = Drag::None;
    s.drawing.clear ();
    s.drawHandles.clear ();
    s.references.clear ();
    s.tracker.Hide ();
    s.capturedPlane.reset ();
    s.plane = next;
    s.message = unfinished ? "The drawing plane changed; unfinished input was cancelled." : "";
    return true;
}

bool UseFacePlane (Session& s, edit::Ref face)
{
    edit::WorkingPlane plane;
    // A plane change will cancel the gesture: never adopt a face from the discarded preview.
    const auto& doc = s.edits.InGesture () ? s.edits.Before () : s.edits.Doc ();
    std::string error;
    if (!edit::PlaneFromFace (doc, face, plane, error)) {
        s.message = error;
        return false;
    }
    plane.locked = s.plane.locked; // an explicit pick can replace a locked datum, not unlock it
    return SetWorkingPlane (s, plane);
}

bool UsePathPlane (Session& s, edit::Id path, double station, edit::PathPlaneKind kind)
{
    edit::WorkingPlane plane;
    const auto& doc = s.edits.InGesture () ? s.edits.Before () : s.edits.Doc ();
    std::string error;
    if (!edit::PlaneFromPath (doc, path, station, kind, plane, error)) {
        s.message = error;
        return false;
    }
    plane.locked = s.plane.locked;
    return SetWorkingPlane (s, plane);
}

bool UsePathPartPlane (Session& s, edit::Ref part)
{
    const auto& doc = s.edits.InGesture () ? s.edits.Before () : s.edits.Doc ();
    const auto* path = edit::FindPath (doc, part.entity);
    std::optional<double> station;
    if (path != nullptr) {
        if (part.part == edit::Part::PathPoint) {
            edit::AlignmentGeometry alignment;
            std::string error;
            if (edit::BuildAlignment (path->points, alignment, error))
                for (size_t i = 0; i < path->points.size (); ++i)
                    if (path->points[i].id == part.item)
                        station = alignment.pointStations[i];
        }
        else if (part.part == edit::Part::Break) {
            for (const auto& point : path->profile)
                if (point.id == part.item)
                    station = point.station;
        }
        else if (part.part == edit::Part::Range) {
            for (const auto& range : path->sections)
                if (range.id == part.item)
                    station = range.station;
        }
    }
    if (!station) {
        s.message = "Choose an existing path control point, grade point or range start; whole paths need a station.";
        return false;
    }
    return UsePathPlane (s, part.entity, *station);
}

bool BeginPlaneEntry (Session& s)
{
    if (s.drag != Drag::None || s.edits.InGesture () || s.penDown || !s.drawing.empty () || s.labelOpen ||
        s.typedHandle >= 0 || s.planeTyping || s.tracker.State () == edit::TrackerState::Editing) {
        s.message = "Finish or cancel the current input before entering a drawing-plane elevation.";
        return false;
    }
    s.planeTyping = true;
    s.planeEntryPath = edit::kNoId;
    s.planeEntryRevision = 0;
    s.tracker.Hide ();
    s.tracker.Show ({ { edit::Quantity::Elevation, s.plane.origin.z } });
    s.message = "Type the World XY elevation in metres; Enter sets the drawing plane to World XY, Esc cancels.";
    detail::CapturePlane (s);
    return true;
}

bool BeginPathPlaneEntry (Session& s, edit::Id path)
{
    edit::WorkingPlane candidate;
    std::string error;
    if (!edit::PlaneFromPath (s.edits.Doc (), path, 0, edit::PathPlaneKind::Plan, candidate, error)) {
        s.message = error;
        return false;
    }
    if (!BeginPlaneEntry (s))
        return false;
    s.planeEntryPath = path;
    s.planeEntryRevision = s.edits.Doc ().revision;
    const double station =
        s.plane.source == edit::PlaneSource::PathPlan && s.plane.sourceRef.entity == path ? s.plane.sourceStation : 0;
    s.tracker.Hide ();
    s.tracker.Show ({ { edit::Quantity::Distance, station } });
    s.message = "Type the distance from path start in metres; Enter sets a horizontal drawing plane there, "
                "Esc cancels.";
    return true;
}

namespace detail {

void CheckPlane (Session& s)
{
    if (s.capturedPlane && !edit::SamePlane (*s.capturedPlane, s.plane)) {
        const auto next = s.plane;
        s.plane = *s.capturedPlane; // validate an externally requested change against the captured valid frame
        SetWorkingPlane (s, next);
    }
}

void CapturePlane (Session& s)
{
    if (s.drag != Drag::None || s.penDown || !s.drawing.empty () || s.labelOpen || s.typedHandle >= 0 ||
        s.planeTyping || s.tracker.State () == edit::TrackerState::Editing) {
        if (!s.capturedPlane)
            s.capturedPlane = s.plane;
    }
    else
        s.capturedPlane.reset ();
}

void PlaneControls (Session& s)
{
    if (s.plane.source == edit::PlaneSource::Face)
        ImGui::Text ("Drawing plane: selected horizontal face (sticky): %llu / %llu r%llu",
                     static_cast<unsigned long long> (s.plane.sourceRef.entity),
                     static_cast<unsigned long long> (s.plane.sourceRef.item),
                     static_cast<unsigned long long> (s.plane.sourceRevision));
    else if (s.plane.source == edit::PlaneSource::PathPlan)
        ImGui::Text ("Drawing plane: horizontal along the path (sticky): %llu at %.6g m, r%llu",
                     static_cast<unsigned long long> (s.plane.sourceRef.entity), s.plane.sourceStation,
                     static_cast<unsigned long long> (s.plane.sourceRevision));
    else
        ImGui::TextUnformatted ("Drawing plane: World XY (sticky)");
    char label[80];
    std::snprintf (label, sizeof label, "Elevation %.6g m##planeElevation", s.plane.origin.z);
    const bool elevationClicked = ImGui::SmallButton (label);
    ImGui::SetItemTooltip ("Type the World XY elevation for the drawing plane. It moves neither the path nor "
                           "the camera.");
    if (elevationClicked)
        BeginPlaneEntry (s); // the existing Tracker owns numeric text, validation and Esc
    ImGui::SameLine ();
    auto next = s.plane;
    if (ImGui::Checkbox ("Lock plane", &next.locked))
        SetWorkingPlane (s, next);
    const auto& selected = s.selection.Items ();
    ImGui::BeginDisabled (selected.size () != 1 || selected[0].part != edit::Part::Face);
    const bool faceClicked = ImGui::SmallButton ("Drawing plane at selected face");
    ImGui::SetItemTooltip ("Draws on the horizontal top or bottom face of an extrusion; the drawing plane "
                           "sits at that face's elevation. Sloped or vertical faces are not supported.");
    if (faceClicked)
        UseFacePlane (s, selected[0]);
    ImGui::EndDisabled ();
    const bool path = selected.size () == 1 && edit::FindPath (s.edits.Doc (), selected[0].entity) != nullptr;
    ImGui::BeginDisabled (!path);
    const bool stationClicked = ImGui::SmallButton ("Horizontal drawing plane along path...");
    ImGui::SetItemTooltip ("A horizontal drawing plane at the path's elevation, a typed distance from its "
                           "start. Its axes run along and across the path; it moves neither the path nor the "
                           "camera.");
    if (stationClicked)
        BeginPathPlaneEntry (s, selected[0].entity);
    ImGui::EndDisabled ();
    if (path && (selected[0].part == edit::Part::PathPoint || selected[0].part == edit::Part::Break ||
                 selected[0].part == edit::Part::Range)) {
        const char* partLabel = selected[0].part == edit::Part::Range   ? "Drawing plane at selected range start"
                                : selected[0].part == edit::Part::Break ? "Drawing plane at selected grade point"
                                                                        : "Drawing plane at selected control point";
        const bool partClicked = ImGui::SmallButton (partLabel);
        ImGui::SetItemTooltip ("A horizontal drawing plane at this path point's own station and elevation; it "
                               "moves neither the path nor the camera.");
        if (partClicked)
            UsePathPartPlane (s, selected[0]);
    }
}

} // namespace detail
} // namespace geomsrv::archviz::edithud
