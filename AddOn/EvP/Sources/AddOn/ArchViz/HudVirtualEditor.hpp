#ifndef EVP_ARCHVIZ_HUDVIRTUALEDITOR_HPP
#define EVP_ARCHVIZ_HUDVIRTUALEDITOR_HPP

// The virtual editor's two hosts. On the locked view (OverlayHudLocked.cpp) it picks,
// snaps, drags and draws its transient feedback -- hover, snap marker, guides, gizmo,
// marquee, tracker -- with ImGui, anchored to model points so the Present reprojects
// it; the model itself is the command-scoped layer EditLayers builds. In the host
// panel (the miniGUI) it shows the tools, the context actions and a path's profile,
// edited on its own canvas.
//
// Overlay contract: private/docs/architecture/diligent/OVERLAY-INVARIANTS.md. Nothing
// here reads the model or a camera: the plan arrives as the view's transform and 3D as
// the projection the input layer hands over (§12, §14), and a session's state dies with
// its session (§8).
#include "ArchViz/OverlayLayers.hpp"
#include "ArchViz/RoofSlopeCanvas.hpp"
#include "Edit/EditGizmo.hpp"
#include "Edit/EditPolyline.hpp"
#include "Edit/EditProfileView.hpp"
#include "Edit/EditSnap.hpp"
#include "Edit/EditTracker.hpp"
#include <optional>
#include <string>
#include <vector>

namespace geomsrv::archviz::overlayhud {
struct Input;
}

namespace geomsrv::archviz::edithud {

enum class Tool : uint8_t { Select, Polyline, Path, Curve };
enum class Drag : uint8_t { None, Part, Handle, Marquee, ProfileBreak, ProfileTangent, Corner, Bezier, Bend };

// What a right click landed on, which decides what its menu offers.
enum class MenuTarget : uint8_t { View, Drawing, Vertex, Segment, Object, Path, PathPoint, GradeBreak, Range };
struct MenuItem {
    std::string id, label, shortcut;
    bool enabled = true, checked = false;
    bool ruleAfter = false; // a separator follows
};
struct ContextMenu {
    MenuTarget target = MenuTarget::View;
    edit::Ref ref; // the item under the pointer (now the selection)
    edit::Vec3 at; // the model point under the pointer: where "here" is
    bool open = false;
};

struct Session {
    edit::Session edits;
    edit::Selection selection;
    edit::Tracker tracker;
    edit::SnapContext context; // host geometry snapshot, filled by the starting command
    Tool tool = Tool::Select;
    edit::GizmoMode gizmo = edit::GizmoMode::Move;
    bool localAxes = true; // the gumball along the shape (EditGizmo SelectionFrame), else the world's
    bool fromEnd = false;  // a typed length or angle keeps the segment's end, not its start (F)
    // Snapping: which snaps are on (edit::SnapMode), the construction angle step, whether a
    // moved item's edges snap flush to others, and the edges the work has touched lately --
    // parallels and squares run to them.
    uint32_t snapModes = edit::kSnapAll;
    double angleStep = 15.0;
    bool edgeSnap = true;
    std::vector<std::pair<edit::Vec3, edit::Vec3>> references;
    // A pivot dragged off the selection's centre stays put for that selection and travels
    // with it.
    std::optional<edit::Vec3> pivot;
    std::vector<edit::Ref> pivotFor;
    uint32_t filter = edit::kPickAll;
    double pathWidth = 1.5;
    std::string role;  // the starting command's word for what it edits
    std::string owner; // the palette command that opened it: its own later runs keep it

    // The gesture in progress.
    Drag drag = Drag::None;
    edit::Ref target;
    int handle = -1, hoverHandle = -1;
    edit::Frame frame;
    edit::Transform dragTransform; // the handle drag's transform so far: the gumball rides it
    // A gumball handle clicked without a drag: the tracker asks for its number (a distance,
    // an angle, a factor or a width) and Enter applies it about `typedFrame`.
    int typedHandle = -1;
    edit::GizmoMode typedMode = edit::GizmoMode::Move;
    edit::Frame typedFrame;
    edit::Ray startRay;
    edit::Vec3 anchor, startCursor;
    float startX = 0.0f, startY = 0.0f;
    bool dragPlanar = false;
    edit::SnapResult snap;
    // Curves: the pen's handles for the points it placed (parallel to `drawing`) and its
    // press; an anchor's handle hovered or dragged; a curved segment being bent.
    std::vector<edit::Vec3> drawHandles;
    float penX = 0.0f, penY = 0.0f;
    bool penDown = false;
    int hoverBezier = -1;
    edit::Id bezierEntity = edit::kNoId, bezierVertex = edit::kNoId;
    bool bezierOut = true;
    edit::Id bendEntity = edit::kNoId, bendSegment = edit::kNoId;
    double bendT = 0.5;
    edit::Vec3 bendStart;
    // A live corner's grip: hovered, and the corners a drag of it sets.
    int hoverCorner = -1;
    edit::Id cornerEntity = edit::kNoId;
    size_t cornerIndex = 0;
    std::vector<edit::Id> cornerSet;
    bool wholeGrab = false;               // the press grabbed a selected object by its own part
    std::vector<edit::PickHit> pressHits; // what was under that press, for the click it may be
    std::vector<edit::Vec3> drawing;      // points placed by a drawing tool
    double workZ = 0.0;                   // the working plane's height when nothing gives one

    // The profile canvas.
    edit::Id profilePath = edit::kNoId;
    edit::ProfileView profileView; // frozen while a profile drag runs
    size_t profileIndex = 0;
    double profileStartZ = 0.0;
    float profileStartY = 0.0f;

    // Lifecycle.
    uint64_t token = 0;
    // The cursor's fight with Archicad, for the log: at the start, and at the drag's start.
    uint64_t cursorBase = 0, dragCursorBase = 0;
    bool dragWatched = false;
    bool previousLock = false, close = false;
    uint64_t publishedRevision = UINT64_MAX;
    std::vector<edit::Ref> publishedSelection;
    std::string message;
    roofcanvas::Frame canvas;
    ContextMenu menu;

    explicit Session (edit::Document doc = {}) : edits (std::move (doc))
    {
    }
};

constexpr char kPanelTitle[] = "Virtual editor";

// The locked view: picking, snapping, gestures and transient drawing. `hovered` is the
// pointer over the view and not over a panel.
void OnView (Session& session, const overlayhud::Input& view, bool hovered, float scale);
// Keys and edit actions, once per layout (not once per ImGui frame).
void Keys (Session& session, const overlayhud::Input& view, bool textInput);
// The host panel: tools, context actions, the selected path's profile.
void Panel (Session& session);
// The layer to publish when the document or the selection moved on; false when the
// published one is current.
bool Follow (Session& session, overlaylayers::Layer& layer);

// The snap groups the panel and the menu switch together.
constexpr uint32_t kSnapPoints =
    edit::kSnapEndpoint | edit::kSnapMidpoint | edit::kSnapCentre | edit::kSnapIntersection;
constexpr uint32_t kSnapEdges = edit::kSnapNearest | edit::kSnapPerpendicular | edit::kSnapExtension;
constexpr uint32_t kSnapGuides = edit::kSnapAxis | edit::kSnapParallel | edit::kSnapAngle;

// The right-click menu: the items for what it was opened on, and what one of them does.
std::vector<MenuItem> MenuItems (const Session& session);
void RunMenuItem (Session& session, const std::string& id);
// Opens it on `target` (inside the locked view's window) and draws it while open.
void OpenMenu (Session& session, MenuTarget target, edit::Ref ref, edit::Vec3 at);
void Menu (Session& session);

// Where the gumball stands and how it is turned for the current selection and mode.
edit::Frame GizmoFrame (const Session& session);

// The path the profile canvas shows: the selected one, else the only one.
const edit::Path* ProfilePath (const Session& session);
// The tracker's fields for the current selection or drawing.
std::vector<edit::TrackerItem> TrackerFields (const Session& session, std::vector<edit::Dimension>& dimensions);

} // namespace geomsrv::archviz::edithud
#endif
