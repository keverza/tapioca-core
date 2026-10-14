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
#include <string>
#include <vector>

namespace geomsrv::archviz::overlayhud {
struct Input;
}

namespace geomsrv::archviz::edithud {

enum class Tool : uint8_t { Select, Polyline, Path };
enum class Drag : uint8_t { None, Part, Handle, Marquee, ProfileBreak, ProfileTangent };

struct Session {
    edit::Session edits;
    edit::Selection selection;
    edit::Tracker tracker;
    edit::SnapContext context; // host geometry snapshot, filled by the starting command
    Tool tool = Tool::Select;
    edit::GizmoMode gizmo = edit::GizmoMode::Move;
    uint32_t filter = edit::kPickAll;
    double pathWidth = 1.5;
    std::string role; // the starting command's word for what it edits

    // The gesture in progress.
    Drag drag = Drag::None;
    edit::Ref target;
    int handle = -1, hoverHandle = -1;
    edit::Frame frame;
    edit::Ray startRay;
    edit::Vec3 anchor, startCursor;
    float startX = 0.0f, startY = 0.0f;
    bool dragPlanar = false;
    edit::SnapResult snap;
    std::vector<edit::Vec3> drawing; // points placed by a drawing tool
    double workZ = 0.0;              // the working plane's height when nothing gives one

    // The profile canvas.
    edit::Id profilePath = edit::kNoId;
    edit::ProfileView profileView; // frozen while a profile drag runs
    size_t profileIndex = 0;
    double profileStartZ = 0.0;
    float profileStartY = 0.0f;

    // Lifecycle.
    uint64_t token = 0;
    bool previousLock = false, close = false;
    uint64_t publishedRevision = UINT64_MAX;
    std::vector<edit::Ref> publishedSelection;
    std::string message;
    roofcanvas::Frame canvas;

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

// The path the profile canvas shows: the selected one, else the only one.
const edit::Path* ProfilePath (const Session& session);
// The tracker's fields for the current selection or drawing.
std::vector<edit::TrackerItem> TrackerFields (const Session& session, std::vector<edit::Dimension>& dimensions);

} // namespace geomsrv::archviz::edithud
#endif
