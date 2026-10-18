#include "ArchViz/HudVirtualEditorDetail.hpp"

// OVERLAY-INVARIANTS.md: shared cancellation of view-owned input only; no camera or native writes.
namespace geomsrv::archviz::edithud::detail {

// Projection/window changes cancel view gestures but keep unfinished model-space drawing
// points. The profile's drags belong to its independent panel canvas.
void CancelViewEdit (Session& s, const char* reason)
{
    bool cancelled = s.typedHandle >= 0 || s.labelOpen || s.penDown;
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
