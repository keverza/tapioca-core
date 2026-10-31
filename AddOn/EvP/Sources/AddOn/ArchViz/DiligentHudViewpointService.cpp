// Native per-frame worker owner, not part of shared workflow presentation.
#include "ArchViz/DiligentHudViewpointDetail.hpp"
#include "ArchViz/HudGumball.hpp"
#include "ArchViz/InputRingBuffer.hpp"
#include "ArchViz/PointGumball.hpp"
#include "ArchViz/VisibilityStudyCapture.hpp"
#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::viewerhud {
using namespace viewpointdetail;
void ServiceViewpointInteractions (HudState& state, const InputSnapshot& input, const float viewProj[16],
                                   uint32_t width, uint32_t height, const float rayOrigin[3],
                                   const float rayDirection[3])
{
    const auto shared = viewpointstudy::GetSettings ();
    if (shared->revision != state.viewpointSeenRevision) {
        // Command changes discover the page once; our own gumball updates record
        // their revision below and never steal another analysis tab each frame.
        state.viewpointSeenRevision = shared->revision;
        state.viewpointDragging = false;
        if (shared->enabled)
            analysistabs::Select (state.analysisFocus, analysistabs::kViewpointTab);
    }
    state.viewpointOwnsMouse = false;
    state.viewpointHotPart = -1;
    const bool point = !state.readOnly && shared->pointValid && ShowsPointGumball (state, *shared);
    const bool target = ShowsTargetPoint (state);
    // Last, after this frame's move: the bearing faces where the point now is.
    const auto tick = [&state] {
        auto settings = *viewpointstudy::GetSettings ();
        if (FaceTarget (state, settings))
            ConfigureViewpoint (state, std::move (settings));
        viewpointstudy::Tick (visibilitystudy::ViewerCapture ());
    };
    // -1 is nothing selected: both handles show as markers, for a clean view.
    if (state.viewpointActiveHandle == 1 && !target)
        state.viewpointActiveHandle = point ? 0 : -1;
    if (state.viewpointActiveHandle == 0 && !point)
        state.viewpointActiveHandle = target ? 1 : -1;
    if (state.viewpointPlacing || state.viewpointPlacingTarget || (!point && !target)) {
        state.viewpointDragging = false;
        tick ();
        return;
    }
    // Two handles, the shared point and the target. Only the selected one has a
    // gumball; the other is a marker, and pressing it selects it and drags it in
    // plan at once, the way the square would. A click on neither deselects both.
    const float x = float (CursorTargetX (input, width)), y = float (CursorTargetY (input, height));
    const bool shown[2] = { point, target };
    const double* const position[2] = { shared->options.point.data (), state.visibilityTarget };
    const int active = state.viewpointActiveHandle;
    int hit = -1, handle = -1;
    if (input.inside && active >= 0 && shown[active]) {
        hit = HitGumball (MakeGumball (position[active], viewProj, width, height, true), x, y);
        handle = hit >= 0 ? active : -1;
    }
    for (int other = 0; other < 2 && input.inside && hit < 0; ++other) {
        if (other == active || !shown[other])
            continue;
        const auto marker = MakeGumball (position[other], viewProj, width, height);
        if (marker.valid && std::hypot (x - marker.center.x, y - marker.center.y) <= kGumballMarkerPickPixels) {
            hit = kGumballPlane;
            handle = other;
        }
    }
    const double origin[3] = { rayOrigin[0], rayOrigin[1], rayOrigin[2] };
    const double direction[3] = { rayDirection[0], rayDirection[1], rayDirection[2] };
    const bool pressed = LeftTransition (input, true), released = LeftTransition (input, false);
    if (!state.viewpointDragging && !state.wantsMouse && hit >= 0 && pressed) {
        state.viewpointActiveHandle = handle;
        state.viewpointDragAxis = hit;
        state.viewpointDragHandle = handle;
        std::copy (position[handle], position[handle] + 3, state.viewpointDragPoint);
        state.viewpointDragging = PointGumballParameter (state.viewpointDragPoint, DragAxis (hit), origin, direction,
                                                         state.viewpointDragParameter) ||
                                  hit == kGumballCentre;
    }
    // A click off every handle -- pressed and released in place -- deselects.
    if (!state.wantsMouse && input.inside && hit < 0 && pressed && !state.viewpointDragging) {
        state.viewpointPressedOff = true;
        state.viewpointPressAt[0] = x;
        state.viewpointPressAt[1] = y;
    }
    if (state.viewpointPressedOff && (released || !(input.buttons & kMouseLeft))) {
        if (released && std::hypot (x - state.viewpointPressAt[0], y - state.viewpointPressAt[1]) <= 4.0f)
            state.viewpointActiveHandle = -1;
        state.viewpointPressedOff = false;
    }
    state.viewpointOwnsMouse =
        state.viewpointDragging || (!state.wantsMouse && hit >= 0 && (pressed || (input.buttons & kMouseLeft)));
    if (state.viewpointDragging) {
        double moved[3];
        bool ok = state.viewpointDragAxis == kGumballCentre &&
                  SnapUnderCursor (origin, direction, state.viewpointDragHandle == 0, moved);
        double parameter[3];
        if (!ok && PointGumballParameter (state.viewpointDragPoint, DragAxis (state.viewpointDragAxis), origin,
                                          direction, parameter)) {
            for (int axis = 0; axis < 3; ++axis)
                moved[axis] = state.viewpointDragPoint[axis] + parameter[axis] - state.viewpointDragParameter[axis];
            ok = true;
        }
        if (ok && state.viewpointDragHandle == 1) {
            std::copy (moved, moved + 3, state.visibilityTarget);
            state.visibilityTargetEdited = true; // moved by hand: it no longer follows Focus
        }
        else if (ok && state.viewpointDragHandle == 0) {
            auto settings = *shared;
            std::copy (moved, moved + 3, settings.options.point.begin ());
            if (settings.options.point != shared->options.point) {
                FaceTarget (state, settings);
                ConfigureViewpoint (state, std::move (settings));
            }
        }
        if (released || !(input.buttons & kMouseLeft))
            state.viewpointDragging = false;
    }
    state.viewpointHotPart = state.viewpointDragging ? state.viewpointDragAxis : (state.wantsMouse ? -1 : hit);
    state.viewpointHotHandle = state.viewpointDragging ? state.viewpointDragHandle : handle;
    tick ();
}
} // namespace geomsrv::archviz::viewerhud
