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
#include "ArchViz/EditLayers.hpp"
#include "ArchViz/OverlayLayers.hpp"
#include "ArchViz/RoofSlopeCanvas.hpp"
#include "Edit/EditGizmo.hpp"
#include "Edit/EditPathNetwork.hpp"
#include "Edit/EditPolyline.hpp"
#include "Edit/EditProfileView.hpp"
#include "Edit/EditSnap.hpp"
#include "Edit/EditTracker.hpp"
#include "Edit/EditWorkingPlane.hpp"
#include <optional>
#include <memory>
#include <array>
#include <string>
#include <vector>

namespace geomsrv::archviz::overlayhud {
struct Input;
}

namespace geomsrv::archviz::edithud {

struct TerrainDesignState;
struct TerrainCaptureRequest;
struct TerrainSectionDraft;
struct TerrainDesignBaseline;

enum class Tool : uint8_t { Select, Polyline, Path, Curve, Rectangle, Polygon, Ellipse };
// The tools that place a closed shape from two clicks (edit::Primitive), not a run of points.
constexpr bool ShapeTool (Tool tool)
{
    return tool == Tool::Rectangle || tool == Tool::Polygon || tool == Tool::Ellipse;
}
enum class LandscapeMode : uint8_t { Select, DrawSource, Face, Boolean, Entrance };
struct LandscapeCommand {
    tapioca::geometry::OperationKind kind;
    uint64_t revision = 0;
    bool second = false;
    std::vector<tapioca::geometry::RegionDescriptor> first;
};
enum class Drag : uint8_t {
    None,
    Part,
    Handle,
    Marquee,
    ProfileBreak,
    ProfileTangent,
    Corner,
    Bezier,
    Bend,
    Kerb,
    ProfileCurve // a grade break's smoothing nub (EditProfileView.hpp CurveNubs)
};
// A drag on the panel's profile canvas, not the view: native navigation and a view change leave it be.
constexpr bool ProfileDrag (Drag drag)
{
    return drag == Drag::ProfileBreak || drag == Drag::ProfileTangent || drag == Drag::ProfileCurve;
}

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
    std::shared_ptr<TerrainDesignState> terrainDesign; // Derived worker/cache follows this session between hosts.
    // Explicit UI capture action; the controller posts native reads out of layout.
    std::shared_ptr<const TerrainCaptureRequest> terrainCapture;
    // The terrain set from Archicad before there is anything to grade: held until a design path
    // exists, then the design starts on it (SetTerrainBaseline). Null once a design owns it.
    std::shared_ptr<TerrainDesignBaseline> terrainBaseline;
    bool terrainCaptureQueued = false;
    // Explicit UI request to add the Archicad selection's drafting; posted out of layout like the capture.
    bool draftingImport = false, draftingImportQueued = false;
    // What the queued import makes of Archicad's selection: "lines", "property", "building" or
    // "entrances" (the selected doors) -- Tapioca.VirtualEditor's importAs.
    std::string draftingImportAs = "lines";
    edit::Id terrainPath = edit::kNoId;
    std::shared_ptr<TerrainSectionDraft> terrainSectionDraft; // Uncommitted typed fields; not document state.
    // A cross-section slider is held: its gesture applies every change live (the terrain worker
    // keeps building on it) and its release is the one undo step.
    bool terrainSectionDrag = false;
    // Grade along the chosen design path's band only, instead of every path's paved outline.
    bool terrainBand = false;
    edit::Selection selection;
    // Model-space outlines only; bounded to this session and reprojected for every pick.
    edit::PickGeometryCache pickCache;
    edit::Tracker tracker;
    edit::SnapContext context; // host geometry snapshot, filled by the starting command
    Tool tool = Tool::Select;
    LandscapeMode landscapeMode = LandscapeMode::Select;
    tapioca::geometry::SourceRole landscapeRole = tapioca::geometry::SourceRole::ReferenceLine;
    tapioca::geometry::LandscapeResult landscape;
    uint64_t landscapeRevision = UINT64_MAX, landscapeBuilds = 0;
    double publishedLandscapeZ = 0.0;
    edit::Id landscapeObject = edit::kNoId;
    edit::Id landscapeSet = edit::kNoId, landscapeOperandSet = edit::kNoId;
    std::string landscapeSaved;
    std::optional<LandscapeCommand> landscapeCommand;
    edit::GizmoMode gizmo = edit::GizmoMode::Gumball; // all applicable transform handles
    bool localAxes = true; // the gumball along the shape (EditGizmo SelectionFrame), else the world's
    bool fromEnd = false;  // a typed length or angle keeps the segment's end, not its start (F)
    // Snapping: which snaps are on (edit::SnapMode), the construction angle step, whether a
    // moved item's edges snap flush to others, and the edges the work has touched lately --
    // parallels and squares run to them.
    uint32_t snapModes = edit::kSnapAll & ~(edit::kSnapTerrain | edit::kSnapGrid); // terrain Z and grid are opt-in
    bool snapEnabled = true; // suspends snapping without clearing the selected modes
    double snapGrid = 1.0;
    double snapProximity = 1.0; // metres: a target farther than this in 3D is not "here"
    double angleStep = 15.0;
    bool edgeSnap = true;
    std::vector<std::pair<edit::Vec3, edit::Vec3>> references;
    // A pivot dragged off the selection's centre stays put for that selection and travels
    // with it.
    std::optional<edit::Vec3> pivot;
    std::vector<edit::Ref> pivotFor;
    std::vector<edit::Ref> gumballRepositionFor;
    uint64_t gumballRepositionRevision = 0;
    uint32_t filter = edit::kPickAll;
    double pathWidth = 1.5;
    int polygonSides = 6; // the polygon tool's, Up and Down while it is placed
    std::string role;     // the starting command's word for what it edits
    std::string owner;    // the palette command that opened it: its own later runs keep it

    // The gesture in progress.
    Drag drag = Drag::None;
    edit::Ref target;
    int handle = -1, hoverHandle = -1;
    // The control a gesture captured, resolved once where it began: the drag, its drawing, a
    // typed number and a navigation rebase all name the same handle, whatever the mode or the
    // selection says afterwards. `facePush` says its refs are the pressed face (not the
    // selection): a face pushes or pulls along its normal through the mode's own face control.
    edit::Handle heldControl;
    bool facePush = false;
    edit::Frame frame;
    edit::Transform dragTransform; // the handle drag's transform so far: the gumball rides it
    // The handle drag's value so far, in its quantity -- the tracker's field while it runs, so a
    // number typed mid-drag replaces it -- and whether the button came up while that number was
    // being typed (the number, not the release, ends the gesture then).
    double dragValue = 0.0;
    bool dragReleased = false;
    // After leaving numeric entry, keep its preview until the pointer actually moves again.
    std::optional<std::pair<float, float>> dragPointerPause;
    struct Navigation {
        bool blocked = false, pending = false, handleRebased = false;
        uint64_t revision = 0;
        std::optional<std::array<float, 12>> projection;
        edit::Vec3 reference, offset;
        std::optional<edit::Vec3> point; // last successful operation input, not a new document baseline
        edit::Ray ray;
        edit::DragResult handle;
        double gripInset = -1.0; // captured with the grip, unaffected by zoom
        std::string notice;      // clear only feedback this navigation lifecycle owns
    } navigation;
    uint64_t viewIdentity = 0;
    bool marqueeAdditive = false;
    // The marquee as a free-hand outline (screen pixels, as dragged): the panel's choice, or
    // Ctrl held at the press; and the region pressed inside, which a release without a drag
    // selects -- a drag from inside an unselected region is a marquee, not a move.
    bool lasso = false, marqueeLasso = false;
    std::vector<std::pair<double, double>> lassoPoints;
    // A part drag that moves its items (not a path's width or grade break): the way they have
    // gone so far, in plan, which a typed distance keeps -- unset until the pointer has moved.
    bool dragTypable = false;
    std::optional<edit::Vec3> dragDirection;
    // A gumball handle clicked without a drag: the tracker asks for its number (a distance,
    // an angle, a factor or a width) and Enter applies it about `typedFrame`.
    int typedHandle = -1;
    edit::GizmoMode typedMode = edit::GizmoMode::Move;
    edit::Frame typedFrame;
    edit::Handle typedControl; // the clicked handle itself, kept for the number it takes
    edit::Ray startRay;
    edit::Vec3 anchor, startCursor;
    float startX = 0.0f, startY = 0.0f;
    std::optional<bool> viewPlanar; // the projection the view showed last frame: plan or 3D
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
    // A kerb corner's grip (a paved corner of the selected path): hovered; the corner picked
    // by a click (then typing or the panel sets it) or being dragged -- either way that corner
    // alone. The paved corners, per revision.
    int hoverKerb = -1;
    bool kerbPicked = false;
    edit::PavedCorner kerb;
    edit::PavedArea paved;
    uint64_t pavedRevision = UINT64_MAX;
    double pavedStep = 0, publishedPathStep = 0, contoursPathStep = 0;
    std::vector<edit::Id> quietPaths, publishedQuietPaths;
    bool wholeGrab = false;               // the press grabbed a selected object by its own part
    bool dragStarted = false;             // the pointer has left the press by the drag threshold
    std::vector<edit::PickHit> pressHits; // what was under that press, for the click it may be
    std::vector<edit::Vec3> drawing;      // points placed by a drawing tool
    edit::WorkingPlane plane;
    std::optional<edit::WorkingPlane> capturedPlane;
    bool planeTyping = false;
    edit::Id planeEntryPath = edit::kNoId; // while planeTyping: station entry for this path, else World XY elevation
    uint64_t planeEntryRevision = 0;

    // The profile canvas.
    edit::Id profilePath = edit::kNoId;
    edit::ProfileView profileView; // frozen while a profile drag runs
    size_t profileIndex = 0;
    double profileStartZ = 0.0;
    float profileStartY = 0.0f;
    // The value a click on one of the profile's labels bound: the exact dimension typing over
    // it writes (WriteDimension), the document revision it was bound at (an edit since makes
    // it stale, as a view label's does), and whether the canvas owns the number. The binding
    // belongs to the panel, not the view or a drag: a view change does not move it, Esc and
    // the revision check do.
    bool profileTyping = false;
    edit::Dimension profileTarget;
    uint64_t profileRevision = 0;
    // The profile's labels as placed this frame, each with what a click on it binds. Kept
    // apart from `placed`, the locked view's own set (HudVirtualEditorLabels.cpp), so neither
    // can be hit by the other's pointer.
    struct PlacedProfileLabel {
        editlayers::Box box;
        edit::Dimension target;
    };
    std::vector<PlacedProfileLabel> profilePlaced;

    // Lifecycle.
    uint64_t token = 0;
    // The cursor's fight with Archicad, for the log: at the start, and at the drag's start.
    uint64_t cursorBase = 0, dragCursorBase = 0;
    bool dragWatched = false;
    bool previousLock = false, close = false;
    uint64_t publishedRevision = UINT64_MAX;
    // The paths' labels the view places (EditLayers PathLabels), for `marksRevision`.
    std::vector<editlayers::MarkLabel> marks;
    uint64_t marksRevision = UINT64_MAX;
    // The paths' contour lines, a grading guide: shown or not, the interval asked (0: a round
    // one for the heights there are), and the lines traced at `contoursInterval` for
    // `contoursRevision` (HudVirtualEditorDraw.cpp Contours).
    bool contours = true;
    double contourStep = 0.0, contoursInterval = 0.0, contoursAsked = -1.0;
    std::vector<edit::Contour> contourLines;
    uint64_t contoursRevision = UINT64_MAX;
    // The labels as placed this frame, each with what typing over it sets, for a click to
    // open one (HudVirtualEditorLabels.cpp); the one open, the document revision it opened on
    // (an edit since makes it stale), its text as typed, and whether that text is still all
    // selected (the first key then replaces it).
    struct PlacedLabel {
        editlayers::Box box;
        editlayers::MarkLabel target;
    };
    std::vector<PlacedLabel> placed;
    // Where each live dimension stood as a gesture began: the side of its line it took and its
    // distance out (HudVirtualEditorDraw.cpp). A drag keeps them, so a label whose text or
    // neighbours change every frame does not hop between free places; refreshed at rest.
    struct LabelHold {
        edit::Dimension value;
        float side = 1.0f, distance = 0.0f;
    };
    std::vector<LabelHold> labelHolds;
    bool labelOpen = false, labelSelected = false;
    editlayers::MarkLabel labelTarget;
    uint64_t labelRevision = 0;
    std::string labelText;
    std::vector<edit::Ref> publishedSelection;
    std::string message;
    bool debugExportRequested = false;
    uint64_t debugExportRevision = 0;
    std::string debugExportPath, debugExportError;
    roofcanvas::Frame canvas;
    ContextMenu menu;

    explicit Session (edit::Document doc = {}) : edits (std::move (doc))
    {
    }
};

// The host supports World XY and horizontal face/path-plan snapshots. Explicit changes cancel dependent
// unfinished input, not committed geometry; an automatic change respects the plane's lock.
bool SetWorkingPlane (Session& s, const edit::WorkingPlane& plane, bool automatic = false);
bool UseFacePlane (Session& s, edit::Ref face);
bool UsePathPlane (Session& s, edit::Id path, double station, edit::PathPlaneKind kind = edit::PathPlaneKind::Plan);
bool UsePathPartPlane (Session& s, edit::Ref part);
bool BeginPlaneEntry (Session& s);
bool BeginPathPlaneEntry (Session& s, edit::Id path);

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
// Revision-local face locators use the existing Selection; operations retain semantic descriptors instead.
const tapioca::geometry::LandscapeResult& Landscape (Session& session);
bool SetLandscapeMode (Session& session, LandscapeMode mode);
bool PickLandscapeFace (Session& session, edit::Vec3 point, bool click, bool additive = false);
bool KeepLandscapeFaces (Session& session, bool subtract = false);
bool BooleanLandscape (Session& session, tapioca::geometry::OperationKind kind, edit::Id cutter);
bool IsLandscapeFaceSet (const tapioca::geometry::LandscapeOperation& operation);
bool CreateLandscapeFaceSet (Session& session);
bool EditLandscapeFaceSet (Session& session, bool remove = false);
bool SelectLandscapeFaceSet (Session& session);
bool BooleanLandscapeFaceSets (Session& session, tapioca::geometry::OperationKind kind);
bool BeginLandscapeCommand (Session& session, tapioca::geometry::OperationKind kind);
bool AdvanceLandscapeCommand (Session& session);
bool CancelLandscapeCommand (Session& session);
const char* LandscapeCommandPrompt (const Session& session);
bool ReloadLandscape (Session& session, const std::string& definition);
// Every selected line (an entity, or a part of one) takes `role` in the landscape, in one undo
// step: a region's boundary (Guide), a cutter, a material boundary, the property line (one closed
// line). False with a message when there is none or the landscape refuses it.
bool SetSelectedSourceRole (Session& session, tapioca::geometry::SourceRole role);
// The role the selected lines share, or none when they have no source or differ.
std::optional<tapioca::geometry::SourceRole> SelectedSourceRole (const Session& session);
void LandscapePanel (Session& session);
void RegionPanel (Session& session, bool materialsOnly = false);
// A building entrance where `point` lies within `reach` metres of a building outline, at the
// working plane's height; one undo step (HudVirtualEditorEntrances.cpp).
bool PlaceEntrance (Session& session, edit::Vec3 point, double reach);
// The selected path's nearer end onto the entrance nearest either of its ends, fixed at its
// threshold; one undo step.
bool ReachSelectedPathEntrance (Session& session);
void EntrancesPanel (Session& session);

// The snap groups the panel and the menu switch together.
constexpr uint32_t kSnapPoints =
    edit::kSnapEndpoint | edit::kSnapMidpoint | edit::kSnapCentre | edit::kSnapIntersection;
constexpr uint32_t kSnapEdges = edit::kSnapNearest | edit::kSnapPerpendicular | edit::kSnapExtension;
constexpr uint32_t kSnapGuides = edit::kSnapAxis | edit::kSnapParallel | edit::kSnapAngle;
struct SnapOption {
    uint32_t mode;
    const char* label;
};
const std::vector<SnapOption>& SnapOptions ();

// The right-click menu: the items for what it was opened on, and what one of them does.
std::vector<MenuItem> MenuItems (const Session& session);
void RunMenuItem (Session& session, const std::string& id);
// Opens it on `target` (inside the locked view's window) and draws it while open.
void OpenMenu (Session& session, MenuTarget target, edit::Ref ref, edit::Vec3 at);
void Menu (Session& session);

// Where the gumball stands and how it is turned for the current selection and mode.
edit::Frame GizmoFrame (const Session& session);
std::vector<edit::Handle> GizmoHandles (const Session& session, edit::GizmoMode mode);
bool GizmoAvailable (const Session& session, edit::GizmoMode mode);
// The slot of a mode's face push/pull control in that table (Extrude's own handle, the
// gumball's normal arrow), or -1 when the mode has none.
int GizmoFaceSlot (const std::vector<edit::Handle>& handles);
// The control a gesture captured, and the refs it moves: both are resolved where the gesture
// began, so its drawing, its typed number and a navigation rebase all name the same control
// whatever the mode or the selection says afterwards. `HeldRefs` is the pressed face for a
// face push/pull and the selection otherwise.
edit::Handle HeldControl (const Session& session);
std::vector<edit::Ref> HeldRefs (const Session& session);
// Takes the mode's face control for the face just pressed: the frame, its slot and the gesture.
// False (with a message) when this view cannot push that face at all.
namespace detail {
struct Map;
}
bool BeginFacePush (Session& session, const detail::Map& view, float x, float y, const edit::Ref& face);
// True while a gesture or a numeric entry owns the input: a mode change waits for it rather
// than re-naming the control the gesture captured.
bool GizmoModeLocked (const Session& session);
bool GumballRepositioning (const Session& session);
bool CanRepositionGumball (const Session& session);
bool BeginGumballReposition (Session& session);
bool CancelGumballReposition (Session& session);
bool SetGizmoMode (Session& session, edit::GizmoMode mode);

// The path the profile canvas shows: the selected one, else the only one.
const edit::Path* ProfilePath (const Session& session);
// The tracker's fields for the current selection or drawing.
std::vector<edit::TrackerItem> TrackerFields (const Session& session, std::vector<edit::Dimension>& dimensions);

} // namespace geomsrv::archviz::edithud
#endif
