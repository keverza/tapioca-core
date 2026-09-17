#ifndef EVP_ARCHVIZ_OVERLAYCONTROLLER_HPP
#define EVP_ARCHVIZ_OVERLAYCONTROLLER_HPP

// Which overlay belongs to the window in front (PLAT-RE155,
// docs/architecture/api/HANDOFF-OverlayPatch.md stage 12).
//
// ⚠️ A FLOOR PLAN IS A DIFFERENT ENTITY, NOT A SMALLER 3D SCENE, AND ONE MENU
// ITEM WAS ROUTING BOTH THROUGH ONE RENDERER. The injected runtime finds the draw
// family carrying Archicad's 3D MODEL camera, scores candidates by projecting a
// world-space triangle, composes against model depth, and occludes against
// extracted host geometry. A plan window has no model camera, no model depth and
// no perspective: every one of those mechanisms is meaningless there. The
// portable renderer measures the plan's own zoom into a top-down orthographic
// camera and draws it correctly, and it is the renderer for plans -- not a lesser
// version of the injected one.
//
// ⚠️ TWO SESSIONS, NOT ONE FLAG. `overlayActive` as a single global is what let a
// 3D runtime's state reach into plan rendering; the regression that prompted this
// file was a suppression written as "if the injected overlay is Active, do not
// draw the portable wireframe", which is true of the 3D window and says nothing
// whatever about a plan. Each view kind owns its own session and neither can
// reach the other.
//
// ⚠️ AND THERE IS NO SILENT FALLBACK. A 3D start that is refused reports its
// code and stops; it does not quietly open a different renderer and leave the
// user to guess which one they are looking at. Run sixty read `BuildNotPinned`
// off the wrong flag and fell back on every click -- correctly implementing a
// wrong conclusion, invisibly.
//
// THREAD. Every entry point is MAIN THREAD.

#include <cstdint>
#include <string>

namespace geomsrv {
namespace archviz {
namespace overlaycontrol {

enum class ViewKind : uint32_t {
    Unknown = 0, // the window could not be read
    ThreeD,
    FloorPlan,
    Other, // section, elevation, detail, worksheet, layout...
};
const char* ViewKindName (ViewKind kind);

// MAIN THREAD. What Archicad has in front, right now.
ViewKind CurrentView ();

// MAIN THREAD. The menu item: turn the overlay that belongs to THIS view on, or
// off if it is already on. Never touches the other view's session.
void Toggle ();

// MAIN THREAD. Turn off whatever is on, for teardown.
void StopAll ();

struct Status {
    ViewKind view = ViewKind::Unknown;
    bool injectedRunning = false; // the 3D session
    bool portableRunning = false; // the plan/fallback session
    std::string lastCode;         // the real StartError name, never "None"
    std::string lastMessage;
};
Status GetStatus ();

// ⚠️ ASKED BY THE PORTABLE RENDERER BEFORE IT SUPPRESSES ANYTHING, AND IT MUST
// NAME THE VIEW IT IS DRAWING. True only when the injected runtime is drawing the
// 3D model AND the caller is drawing the 3D model too. A plan asks with
// `ViewKind::FloorPlan` and always gets false, whatever the 3D session is doing.
bool InjectedOwnsView (ViewKind kind);

} // namespace overlaycontrol
} // namespace archviz
} // namespace geomsrv

#endif
