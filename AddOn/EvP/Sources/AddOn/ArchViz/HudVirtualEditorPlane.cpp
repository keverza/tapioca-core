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
    const bool horizontalFace = edit::ValidPlane (plane) && plane.source == edit::PlaneSource::Face && plane.u.z == 0 &&
                                plane.v.z == 0 && std::abs (plane.normal.z) == 1;
    if ((!edit::IsWorldXY (plane) && !horizontalFace) || !plane.sticky) {
        s.message = "Use World XY or a horizontal virtual face; side/oblique plane authoring is unavailable.";
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
    detail::CancelViewEdit (s, "The working plane changed; unfinished input was cancelled.");
    // Unlike a view switch, a plane change would mix authoring frames inside one unfinished shape.
    s.edits.Cancel ();
    s.drag = Drag::None;
    s.drawing.clear ();
    s.drawHandles.clear ();
    s.references.clear ();
    s.tracker.Hide ();
    s.capturedPlane.reset ();
    s.plane = next;
    s.message = unfinished ? "The working plane changed; unfinished input was cancelled." : "";
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

bool BeginPlaneEntry (Session& s)
{
    if (s.drag != Drag::None || s.penDown || !s.drawing.empty () || s.labelOpen || s.typedHandle >= 0 ||
        s.tracker.State () == edit::TrackerState::Editing) {
        s.message = "Finish or cancel the current input before entering a working-plane elevation.";
        return false;
    }
    s.planeTyping = true;
    s.tracker.Hide ();
    s.tracker.Show ({ { edit::Quantity::Elevation, s.plane.origin.z } });
    s.message = "Type World XY elevation in metres; Enter sets the plane to World XY, Esc cancels.";
    detail::CapturePlane (s);
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
        ImGui::Text ("Face snapshot (sticky): %llu / %llu r%llu",
                     static_cast<unsigned long long> (s.plane.sourceRef.entity),
                     static_cast<unsigned long long> (s.plane.sourceRef.item),
                     static_cast<unsigned long long> (s.plane.sourceRevision));
    else
        ImGui::TextUnformatted ("Working plane: World XY (sticky)");
    char label[80];
    std::snprintf (label, sizeof label, "Elevation %.6g m##planeElevation", s.plane.origin.z);
    if (ImGui::SmallButton (label))
        BeginPlaneEntry (s); // the existing Tracker owns numeric text, validation and Esc
    ImGui::SameLine ();
    auto next = s.plane;
    if (ImGui::Checkbox ("Lock plane", &next.locked))
        SetWorkingPlane (s, next);
    const auto& selected = s.selection.Items ();
    ImGui::BeginDisabled (selected.size () != 1 || selected[0].part != edit::Part::Face);
    if (ImGui::SmallButton ("Use selected face plane"))
        UseFacePlane (s, selected[0]);
    ImGui::EndDisabled ();
}

} // namespace detail
} // namespace geomsrv::archviz::edithud
