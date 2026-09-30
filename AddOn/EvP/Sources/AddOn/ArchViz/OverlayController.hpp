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
// no perspective: every one of those mechanisms is meaningless there. The plan's
// renderer is its own session (PlanOverlayRuntime): it composes at the plan's
// Present like the 3D overlay, but with the transform ACAPI reads at that Present
// (finding 14) -- not a lesser version of the injected one, and not the portable
// window, whose poll the frame record measured a frame behind.
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
// ⚠️ TWO INTENTS, TWO MENU ITEMS, TWO VERBS (2026-09-28). One item that toggled
// "the overlay of whatever view is in front" made the user's intent a function of
// where they last clicked, and made it impossible to want the 2D overlay while
// working in 3D. Each overlay now has its own item (`Tapioca 3D Overlay`, `Tapioca
// 2D Overlay`), its own Tapioca verb (`Overlay3D`, `Overlay2D`) and its own intent;
// each starts when its view is in front, stops when its view is left, and returns
// with its view. Turning one on or off never touches the other (§12).
//
// THREAD. Every entry point is MAIN THREAD.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

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

// The two overlays a user can ask for. The 2D overlay serves the floor plan; other
// 2D views (sections, elevations, layouts) have no overlay defined yet.
enum class Overlay : uint32_t {
    ThreeD = 0,
    TwoD = 1,
};
const char* OverlayName (Overlay which);

// What asking for an overlay came to -- the menu narrates it, the verb returns it.
struct Outcome {
    bool ok = true;       // false when a start in front of the user was refused
    bool wanted = false;  // the intent after the call
    bool running = false; // its renderer is serving the view in front right now
    std::string code;     // the refusal's name; "Waiting" while its view is not in front; "None"
    std::string message;
    bool retryable = false; // a refusal that may clear by itself (a view not ready yet)
};

// MAIN THREAD. Turn one overlay on or off. On starts it at once when its view is in
// front and waits for that view otherwise; off stops its renderer. `how` names the
// caller for the log ("menu", "api").
Outcome SetWanted (Overlay which, bool wanted, const char* how);

// MAIN THREAD. The menu item: the opposite of what is wanted now.
Outcome Toggle (Overlay which, const char* how);

// MAIN THREAD. The state, unchanged.
Outcome Describe (Overlay which);

// MAIN THREAD. Turn off both, intents and renderers, for teardown. No ACAPI.
void StopAll ();

// MAIN THREAD. The caller's overlay layers changed (ArchViz/OverlayLayers.hpp): hand
// the 3D overlay its prepared copy, and redraw the 3D window when it is serving it --
// a still window does not present by itself. The 2D overlay reads the store on its
// own tick and redraws the plan when it has taken the change.
void PublishLayers ();

// MAIN THREAD. Put an operator-supplied label on the overlay log's timeline.
//
// ⚠️ IT EXISTS BECAUSE ARCHICAD DOES NOT EXPOSE WHICH
// NAVIGATION TOOL IS ACTIVE, AND THE DEVKIT WAS GREPPED TO BE SURE. There is no
// Orbit and no Explore anywhere in it: `ACAPI_Navigator_*` is the project tree,
// and `API_3DWindowInfo` carries size, zoom and projection but no navigation
// state. A log that has to separate stationary from Orbit from Explore therefore
// cannot label itself, and the three-column comparison is unreadable without
// labels -- the 12:24 run recorded both frame paths and no way to say which
// minute was which.
//
// ⚠️ AND IT CANNOT BE DONE BY APPENDING TO THE LOG FROM
// OUTSIDE. `ArchVizLog` holds the file open for the session with FILE_SHARE_READ,
// so a second WRITER is refused -- which is why this goes through the command
// rather than around it.
//
// It writes one line and touches nothing else. Marking is not a mode.
void Mark (const std::string& note);

// MAIN THREAD. Arm or disarm the marker ladder (ArchViz/Dxgi/MarkerLadder.hpp).
//
// ⚠️ IT IS A PROOF PRIMITIVE, SO IT IS OFF UNLESS SOMEONE
// ASKS (section 10), AND IT ROUTES THROUGH HERE FOR THE SAME REASON `Mark` DOES:
// the include gate refuses a NativeCommands file reaching sideways into
// ArchViz/Dxgi, and the controller is already the operator's front door.
void SetMarkerLadder (bool enabled);

// ⚠️ THE SCENE EPOCH GATE (CameraFreshness.hpp). Counting is
// always on; this enables ENFORCEMENT, which skips the composite when the
// camera and the presented image came from different scene passes. Off by
// default and at every arm (section 10).
void SetEpochGate (bool enabled);
bool EpochGateEnabled ();
struct EpochGateCounts {
    uint64_t matched = 0;
    uint64_t mismatched = 0;
    uint64_t suppressed = 0;
    uint64_t behindMax = 0;
    uint64_t aheadMax = 0;
    bool enabled = false;
};
EpochGateCounts EpochGate ();
bool MarkerLadderEnabled ();

// What the ladder painted, per rung, carried across the NativeCommands boundary
// so the command never has to reach into ArchViz/Dxgi itself.
struct LadderCounts {
    bool enabled = false;
    uint64_t a = 0, b = 0, c = 0, e = 0;
    uint64_t failures = 0;
};
LadderCounts MarkerLadderCounts ();

// What the Diligent guest has done on one overlay (Dxgi/PlanGuest.hpp,
// Dxgi/SceneGuest.hpp), carried across the NativeCommands boundary like the ladder's
// counts. ⚠️ TOTALS SINCE THE GUEST ATTACHED: a question about NOW is two readings
// and their difference (section 7). A decline the overlay cannot have is 0.
struct GuestCounts {
    bool attached = false;
    uint32_t attachMilliseconds = 0;
    uint32_t buildMilliseconds = 0;
    uint64_t uploads = 0;
    uint64_t draws = 0; // Presents the guest drew in
    uint64_t drawCalls = 0;
    uint64_t declinedNoCamera = 0;    // 3D: no camera for the frame
    uint64_t declinedNoViewport = 0;  // 3D: no viewport bound
    uint64_t declinedNoTransform = 0; // plan: no transform read at the Present
    uint64_t declinedFailed = 0;      // 3D: the attach or a pipeline failed this session
    uint32_t fills = 0;               // what is uploaded now
    uint32_t lines = 0;
    uint32_t glyphVertices = 0;
    uint32_t pages = 0;
    std::string failure;
    // What the content costs: preparing it on the main thread (layers built and reused,
    // OverlayScene.hpp), the bytes it holds on the GPU, and drawing it at Present on the
    // render thread -- the last draw, and a running mean over about the last sixteen.
    uint32_t prepareMicroseconds = 0;
    uint32_t layersBuilt = 0;
    uint32_t layersReused = 0;
    uint64_t vertexBytes = 0;
    uint64_t pageBytes = 0;
    uint32_t lastDrawMicroseconds = 0;
    uint32_t drawMicroseconds = 0;
    // The HUD panels' own stream (OverlayScene.hpp PrepareSceneHud).
    uint64_t hudUploads = 0;
    uint32_t hudGlyphVertices = 0;
    uint32_t hudPrepareMicroseconds = 0;
};
struct GuestReport {
    GuestCounts plan;
    GuestCounts scene;
};
GuestReport Guest ();

// What the HUD's mouse input took and passed (OverlayInput.hpp), carried across the
// NativeCommands boundary. ⚠️ TOTALS SINCE ARCHICAD STARTED: a question about now is two
// readings and their difference (section 7).
struct InputCounts {
    bool installed = false;
    bool attached3D = false;
    bool attachedPlan = false;
    uint32_t regions3D = 0;
    uint32_t regionsPlan = 0;
    uint64_t seen = 0;  // mouse messages to an attached canvas
    uint64_t taken = 0; // never reached Archicad
    uint64_t takenPresses = 0;
    uint64_t takenMoves = 0;
    uint64_t passedOverHud = 0;  // over the HUD but Archicad's: a gesture begun in the view, or navigation
    uint64_t declinedHidden = 0; // over a region while its HUD was not on screen
    uint64_t refreshes = 0;      // the HUD laid out again for the pointer
    uint64_t changes = 0;        // ...and what it draws changed
    uint64_t redraws = 0;        // views asked to draw again for it
    uint32_t lastRedrawMicroseconds = 0;
    uint32_t maxRedrawMicroseconds = 0;
    uint32_t lastRefreshMicroseconds = 0;
    uint32_t maxRefreshMicroseconds = 0;
    bool canvasOnThread3D = false; // the hook sees a canvas's messages only when it is this thread's
    bool canvasOnThreadPlan = false;
    bool cursor3D = false; // the canvas answers WM_SETCURSOR with the HUD's cursor
    bool cursorPlan = false;
    uint64_t cursorsSet = 0; // the HUD's arrow or hand shown
    uint64_t handsShown = 0;
};
InputCounts Input ();

// What the user did to the HUD, both views': its text size, and every panel of the layers
// set now with a title or a control -- its layer, its place, its title, whether it is in
// the dock, its controls' values. MAIN THREAD.
struct HudPanel {
    std::string layer;
    uint32_t panel = 0;
    std::string title;
    bool docked = false;
    std::vector<std::pair<std::string, double>> values; // its controls', by id; the tab bar's
};
struct HudReport {
    float fontScale = 1.0f;
    std::vector<HudPanel> panels;
};
HudReport Hud ();
// The HUD's text size, the nearest step (0.8 to 2), laid out again in both views. MAIN
// THREAD.
void SetHudFontScale (float scale);

// MAIN THREAD, periodic. While an overlay is wanted, keep it on the window the
// user is looking at: tear down the session for the view being left and start
// the one the new view needs. Intent and renderer are tracked apart -- see the
// implementation.
void FollowView ();

// MAIN THREAD, from Archicad's `APINotify_ChangeWindow`: the same as `FollowView`,
// at the moment the window changes rather than at the next heartbeat.
//
// ⚠️ THE HEARTBEAT ALONE LEFT UP TO HALF A SECOND IN WHICH THE 3D SESSION STILL
// HELD ARCHICAD'S CONTEXT TABLE WHILE ANOTHER VIEW DREW THROUGH IT -- a floor plan,
// a section, a layout, a schedule all render on the same immediate context. The
// heartbeat stays, as the fallback for a change the notification does not report.
void OnWindowChanged ();

// MAIN THREAD, from Archicad's project events (close, and open or new, which
// replace the document): every session ends, renderers AND intent. The windows
// they covered belong to the project that went; a new project starts clean,
// one menu click away (section 8).
void OnProjectClosed ();

struct Status {
    ViewKind view = ViewKind::Unknown;
    bool want3D = false; // the user's intent, apart from what runs
    bool want2D = false;
    bool injectedRunning = false; // the 3D session
    bool planRunning = false;     // the plan session, drawn at the plan's Present
    bool portableRunning = false; // the portable window (no longer the menu's plan renderer)
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
