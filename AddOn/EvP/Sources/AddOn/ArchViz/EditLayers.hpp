#ifndef EVP_ARCHVIZ_EDITLAYERS_HPP
#define EVP_ARCHVIZ_EDITLAYERS_HPP

// The virtual editor drawn as one overlay layer: its geometry and paths (surface,
// centreline, edges, control polygon, grade breaks, downhill grade arrows with their
// percentages, crossfall arrows, elevations), what is selected and hovered, the snap
// marker with its guides and hint, live dimensions, and the gizmo. Rebuilt from the
// session whenever its revision or the feedback changes; the layer is command-scoped,
// so it disappears with the editing command. Both overlays draw it through their own
// transforms; nothing here knows a camera.
#include "ArchViz/OverlayLayers.hpp"
#include "Edit/EditGizmo.hpp"
#include "Edit/EditPath.hpp"
#include "Edit/EditPathContours.hpp"
#include "Edit/EditSnap.hpp"
#include <string>
#include <vector>

namespace geomsrv::archviz::editlayers {

constexpr char kLayer[] = "virtual-editor";

// ⚠️ NOTHING WHITE (the user, 2026-10-14: Archicad's plan is white, and white lines and
// text were illegible on it). Text is dark ink on a light pill, which reads on the white
// plan and on a dark 3D background alike; strokes are mid-to-dark colours.
struct Style {
    uint32_t geometry = 0x2B8FE0FFu, selected = 0xF59E0BFFu, hover = 0x0E62C9FFu;
    uint32_t solid = 0x2B8FE03Cu;
    uint32_t guide = 0x9B5DE5FFu, snap = 0xFF7A00FFu;
    uint32_t axis[3] = { 0xE0404FFFu, 0x5EA524FFu, 0x2F6FE4FFu };
    uint32_t activeHandle = 0xFF7A00FFu;
    uint32_t text = 0x1E2228FFu, paper = 0xF7F8FAEBu, dimension = 0x5B6573FFu;
    uint32_t pathSurface = 0xC9A66B70u, pathEdge = 0xA8834AFFu, controlPolygon = 0x7D8796FFu;
    uint32_t gradeBreak = 0x1F7FC4FFu, fixedBreak = 0xD93A49FFu, fall = 0x0F7F84FFu;
    uint32_t junctionBreak = 0x7A3FC4FFu;                      // a grade break shared with a crossing path
    uint32_t contour = 0x8C939CB4u, contourText = 0x6E7682FFu; // grading guides: thin, grey
    uint32_t limitedRadius = 0x7A3FC4FFu; // editor-radius-limited token: fitted kerb dimension text only
    double pathStep = 0.5;                // metres between drawn path samples
    float lineWidth = 2.0f, selectedWidth = 3.5f, vertexPixels = 7.0f;
    float textPixels = 12.0f;
};

struct GizmoView {
    bool shown = false;
    edit::Frame frame;
    edit::GizmoMode mode = edit::GizmoMode::Move;
    double size = 1.0; // metres: the host's metres per pixel times the wanted pixel size
    int active = -1;   // the handle being hovered or dragged
};

struct Feedback {
    // The view places the paths' labels itself (PathLabels), clear of each other and of
    // its own; the layer then leaves them out.
    bool labelsInView = false;
    const edit::Selection* selection = nullptr;
    const edit::SnapResult* snap = nullptr;
    // The paths' contour lines (PathContours), drawn under their lines; the view writes
    // their heights. None when null.
    const std::vector<edit::Contour>* contours = nullptr;
    std::vector<edit::Dimension> dimensions;
    GizmoView gizmo;
};

// "4.20 m", "2.0 %", "53.1°": what a dimension label says.
std::string FormatValue (edit::Quantity quantity, double value);

// A live dimension as the locked view draws it for the selection: what it says and the
// model point it hangs from; a length also has the line it measures. A flat slope says
// nothing, and what the layer already labels (a grade break's height, a range's
// crossfall) is not said twice.
struct LiveLabel {
    edit::Dimension value; // what it says, and what typing over it sets
    edit::Quantity quantity = edit::Quantity::Length;
    std::string text;
    edit::Vec3 at;
    bool line = false;
    edit::Vec3 from, to;
    // A segment's length or angle: the end a typed value keeps and the end it moves.
    bool ends = false;
    edit::Vec3 fixed, moving;
};
// `fromEnd`: the segment's end stays put (edit::Dimension::fromEnd).
std::vector<LiveLabel> LiveDimensions (const edit::Document& doc, const edit::Ref& ref, bool fromEnd = false);

// A label's rectangle on screen, in pixels, as it reads before it is turned; then turned
// about its centre so that it reads along the unit screen direction (ux, uy).
struct Box {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    float ux = 1.0f, uy = 0.0f;
};
// Whether two boxes, turned as they are, come within `margin` pixels of each other.
bool Overlaps (const Box& a, const Box& b, float margin = 3.0f);
// The way a label reads along screen direction (dx, dy): left to right, or bottom to top
// when it stands upright -- never upside down. Level when the direction is none.
void Upright (float& dx, float& dy);
// Where a `w` by `h` label goes: centred `away` pixels from (ax, ay) along the unit screen
// direction (dx, dy), then further out step by step -- or on the other side -- until it
// covers nothing in `taken` (the gizmo, the tracker, labels already placed). The chosen
// box is added to `taken`. With `found`, a label with no free place is not added and
// `found` says so: the caller leaves it out rather than draw it over another.
Box PlaceLabel (float ax, float ay, float w, float h, float dx, float dy, float away, std::vector<Box>& taken,
                bool* found = nullptr);
// The same for a label turned to read along unit screen direction (ux, uy).
Box PlaceAlong (float ax, float ay, float w, float h, float ux, float uy, float dx, float dy, float away,
                std::vector<Box>& taken, bool* found = nullptr);

// A path's own label -- a grade break's height, a tangent's grade, a range's crossfall and
// width, a bend's or a kerb fillet's radius -- for a view that places it in pixels
// (PlaceAlong): it hangs from `at`, stands off towards the plan direction `side` and reads
// along `along`, the way what it measures runs (the path for a height or a grade, across it
// for a crossfall or a width, out from its centre for a radius). `dy` is where the layer
// puts it when it draws it.
struct MarkLabel {
    edit::Vec3 at, side, along;
    std::string text;
    uint32_t rgba = 0;
    uint32_t textRgba = 0; // optional text-only override; the dimension's line keeps rgba
    bool limitedRadius = false;
    float dy = 0.0f;
    // A dimension's own line, `from` to `to`: ticked at both ends (a width across the path),
    // or with an arrow at `to` only (a radius, from its arc's centre). Worth showing only when
    // `extent` metres -- what it measures -- come to enough pixels to read.
    bool line = false, radial = false;
    edit::Vec3 from, to;
    double extent = 0.0;
    // What typing over it sets, when `typed`: a dimension of a path (WriteDimension; a
    // grade, shown unsigned, keeps the way it falls), or a kerb corner's radius when
    // `kerbPath` is set (that path keeps the corner).
    bool typed = false;
    edit::Dimension value;
    edit::Id kerbPath = edit::kNoId;
    edit::KerbCorner kerb;
};
// The layer's own labels for its paths (heights, grades, crossfalls), and, for the view
// only, their dimensions (widths, bend and kerb radii).
std::vector<MarkLabel> PathLabels (const edit::Document& doc, const Style& style = {});

// Triangles of a counter-clockwise plan polygon (ear clipping), as index triples.
std::vector<uint32_t> TriangulateXY (const std::vector<edit::Vec3>& polygon);

overlaylayers::Layer Build (const edit::Document& doc, const Feedback& feedback, const Style& style = {},
                            const std::string& name = kLayer);

} // namespace geomsrv::archviz::editlayers
#endif
