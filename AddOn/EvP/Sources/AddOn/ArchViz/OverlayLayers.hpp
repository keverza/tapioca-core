#ifndef EVP_ARCHVIZ_OVERLAYLAYERS_HPP
#define EVP_ARCHVIZ_OVERLAYLAYERS_HPP

// ArchViz/OverlayLayers -- arbitrary geometry a caller puts on the overlays: named
// layers of polylines, points and triangle meshes in MODEL METRES, drawn by the 2D
// overlay (the floor plan, at its Present) and the 3D overlay (the 3D window, at its
// Present) through each one's own transform. `Tapioca.SetOverlayLayer` fills it.
//
// ⚠️ ONE VOCABULARY, TWO PROJECTIONS, NO CAMERA LOGIC SHARED (§12). A layer is
// geometry and style only. The 2D overlay projects it with the transform ACAPI
// reads at the plan's Present, dropping z; the 3D overlay projects it with the
// camera copied at the model's draw. Neither projection lives here.
//
// ⚠️ DOUBLE UNTIL IT IS UPLOADED. The 2D preparation splits every coordinate into
// hi/lo floats relative to a centre, exactly as the wall outlines are
// (PlanOverlayContent.hpp): a georeferenced project keeps its millimetres. The 3D
// preparation hands world floats to the same camera the building's own geometry
// is drawn with, because that camera expects nothing else (HostOccluders.hpp).
//
// ⚠️ TWO RENDERERS DRAW A LAYER, AND EACH PRIMITIVE HAS EXACTLY ONE (§12b). Plain
// polylines, point markers and single-colour meshes are drawn by the raw D3D11
// layer pipelines (`Prepare2D`, `Prepare3D`) -- the path that needs nothing
// attached to Archicad's device. Everything that needs a glyph atlas, a colour ramp,
// a style or a depth policy -- texts, dimensions, legends, HUD panels, dashed or
// `behind`-styled polylines, styled or heatmap meshes -- is drawn by the Diligent guest
// (ArchViz/OverlayScene.hpp). `DrawnByGuest` decides, and both preparations ask it,
// so nothing is drawn twice and nothing is dropped between them.
//
// THREADS. `Set`/`Clear` and the preparations are MAIN THREAD; the 3D overlay
// receives what it draws through its own lock-free hand-over, never this store.

#include "ArchViz/PlanOverlayContent.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace overlaylayers {

enum class Views : uint32_t {
    TwoD = 1,
    ThreeD = 2,
    Both = 3,
};

bool DrawnIn (Views views, Views view);

// What a 3D item does where the building is in front of it -- the wire's `occlusion`:
// "hide", "fade", "dash", "always" (Show). The 2D overlay has no depth (finding 13)
// and draws every item whole.
enum class Behind : uint8_t {
    Layer = 0, // the layer's own `occlusion`
    Hide = 1,  // not drawn there -- the reference wireframe's rule
    Fade = 2,  // drawn faint
    Dash = 3,  // lines dashed there; other items faint
    Show = 4,  // drawn as if nothing were in front: "always"
};

// ⚠️ A DASH BELONGS TO THE MODEL. A pattern is lengths in MODEL METRES along the line --
// on, off, on, off... -- anchored at the line's start in the model, so a dash stays where
// it is on the building as the camera moves. A period in screen pixels, measured from
// wherever the line's start happened to project, slid along the line with every orbit and
// read as an animation (the user, 2026-09-29). Empty is solid.
constexpr size_t kMaxDashEntries = 8;

// A "dash" line's pattern behind the building when it names none: 0.5 m on, 0.3 m off.
constexpr float kDefaultHiddenDash[2] = { 0.5f, 0.3f };

// How the part of a line behind the building is drawn, where its occlusion is "dash" or
// "fade": a colour, a width and a pattern of its own.
struct HiddenLine {
    uint32_t rgba = 0;             // alpha 0: the line's own -- faint where it fades
    float widthPixels = 0.0f;      // 0: the line's own
    std::vector<float> dashMetres; // empty: kDefaultHiddenDash for "dash", solid for "fade"
};

// What ends a dimension line, or either end of an open polyline.
enum class Terminator : uint8_t { Tick = 0, Arrow = 1, Dot = 2, None = 3 };

struct Polyline {
    std::vector<double> points; // x, y, z model metres; at least two points
    bool closed = false;
    uint32_t rgba = 0xFF3B30FFu; // 0xRRGGBBAA
    float widthPixels = 2.0f;
    // Guest only: a dash pattern in model metres, measured along the whole polyline so a
    // corner does not restart it; and how its part behind the building is drawn.
    std::vector<float> dashMetres;
    HiddenLine hidden;
    Behind behind = Behind::Layer;
    // Guest only: what ends an open polyline, turned with its end segment on screen and
    // `arrowSizePixels` long. A closed one has no ends.
    Terminator startArrow = Terminator::None;
    Terminator endArrow = Terminator::None;
    float arrowSizePixels = 10.0f;
};

// A marker at each point: a square `sizePixels` wide in 2D, an axis cross with arms
// `sizeMetres` long in 3D -- a 3D marker has no pixel size until a camera gives it
// one, and the 3D camera lives on the GPU.
struct PointSet {
    std::vector<double> points; // x, y, z
    uint32_t rgba = 0xFF3B30FFu;
    float sizePixels = 6.0f;
    float sizeMetres = 0.1f;
};

// A value -> colour ramp: sun hours, shadow, clearance, slope, visibility. Stops are
// positions 0..1 along [min, max], ascending, at most `kMaxStops`.
struct ColourStop {
    float at = 0.0f;
    uint32_t rgba = 0xFFFFFFFFu;
};
constexpr size_t kMaxStops = 16;

struct Colormap {
    std::vector<ColourStop> stops;
    double min = 0.0;
    double max = 1.0;
    bool autoRange = true;  // min and max from the values; false when the caller set them
    uint32_t bands = 0;     // 0 is smooth; N is N flat steps
    double isolineStep = 0; // 0 is none; otherwise a contour every `step` value units
    uint32_t isolineRgba = 0x000000A0u;
    float isolineWidthPixels = 1.0f;
};

// The named ramps a caller can ask for instead of writing stops. False for a name
// this does not know; `stops` is untouched then.
bool PresetStops (const std::string& name, std::vector<ColourStop>& stops);

enum class Shading : uint8_t {
    Flat = 0,  // the mesh's colour as given
    Lit = 1,   // darkened away from the eye: reads as a solid
    Ghost = 2, // translucent, opaque only at its silhouette: reads as a volume
    Xray = 3,  // faint and over everything, silhouette kept
};

struct MeshStyle {
    Shading shading = Shading::Flat;
    float opacity = 1.0f;         // multiplies every alpha
    uint32_t edgeRgba = 0;        // alpha 0: no edges
    float edgeWidthPixels = 1.0f; // feature edges: boundaries and creases
    float edgeAngleDegrees = 30.0f;
    bool cullBack = false;
    Behind behind = Behind::Layer;
};

struct Mesh {
    std::vector<double> points;       // x, y, z
    std::vector<uint32_t> indices;    // three per triangle
    uint32_t rgba = 0xFF3B3080u;      // one colour for the mesh
    std::vector<uint32_t> vertexRgba; // optional, one per vertex; overrides `rgba`
    // Guest only. Per-vertex normals (computed from the triangles when absent),
    // per-vertex values for a heatmap, and how the whole mesh is drawn.
    std::vector<double> normals;
    std::vector<double> values;
    Colormap colormap;
    MeshStyle style;
    bool styled = false; // the caller gave a style: the guest draws it
};

// A text's halo width that grows with the text as it is drawn -- see `Text::haloPixels`.
constexpr float kAutoHalo = -1.0f;

enum class Align : uint8_t { Left = 0, Center = 1, Right = 2 };
enum class Baseline : uint8_t { Top = 0, Middle = 1, Bottom = 2, Alphabetic = 3 };

// A label: HarfBuzz-shaped, MTSDF-rendered, a fixed size in LOGICAL pixels at any zoom.
// Anchored to a model point, or -- `screen` -- to the viewport, where `at` x and y are
// fractions of its width and height from the top left: a text layer fixed to the view.
// Or -- `planar` -- lying on a plane in the model, scaling with it.
struct Text {
    std::string text; // UTF-8, one line
    bool screen = false;
    double at[3] = {};
    float offsetPixels[2] = {}; // logical pixels, x right, y down, after the anchor
    float sizePixels = 13.0f;
    uint32_t rgba = 0xFFFFFFFFu;
    uint32_t haloRgba = 0x000000C0u;
    // Logical pixels, fixed; or `kAutoHalo`, which grows with the text as it is drawn --
    // 0.5 px round a 24 px em, 2 px round a 72 px one, none under 8 px -- times
    // `haloScale` (the guest's HaloReach, GuestShaderSources.hpp).
    float haloPixels = kAutoHalo;
    float haloScale = 1.0f;
    uint32_t backgroundRgba = 0; // alpha 0: no panel
    Align align = Align::Center;
    Baseline baseline = Baseline::Middle;
    float rotationDegrees = 0.0f; // on screen, counter-clockwise
    Behind behind = Behind::Layer;
    // The font file (OverlayFonts.hpp resolved the caller's name to it); empty is the
    // bundled font. A layer's `font` fills it in when the item names none.
    std::string font;
    // Lying on a plane in the model instead of facing the view, as a room name painted
    // on a floor: the baseline runs along `direction`, the glyphs rise towards
    // `normal` x `direction`, and the text is `sizeMetres` high. `sizePixels` then sets
    // only the layout's resolution, `offsetPixels` moves the text in that layout, and
    // `rotationDegrees` is unused.
    bool planar = false;
    double direction[3] = { 1.0, 0.0, 0.0 };
    double normal[3] = { 0.0, 0.0, 1.0 };
    double sizeMetres = 0.5;
};

enum class LengthUnit : uint8_t { Metres = 0, Centimetres = 1, Millimetres = 2 };

// An aligned dimension, resolved by the annotation layer's own geometry
// (Annotation/DimensionGeometry.hpp): the line is `offsetMetres` from the measured
// points, towards `direction` when given, in the plane `normal` names when given.
struct Dimension {
    double from[3] = {};
    double to[3] = {};
    double offsetMetres = 0.5;
    double direction[3] = {}; // zero: automatic
    double normal[3] = {};    // zero: horizontal dimensions measure in plan, others upright
    std::string text;         // empty: the measured length
    uint32_t decimals = 2;
    LengthUnit unit = LengthUnit::Metres;
    bool showUnit = false;
    uint32_t rgba = 0xFFFFFFFFu;
    float widthPixels = 1.25f;
    float textSizePixels = 12.0f;
    uint32_t textRgba = 0; // alpha 0: the line's colour
    uint32_t haloRgba = 0x000000A0u;
    float haloPixels = kAutoHalo; // as a text's
    Terminator terminator = Terminator::Tick;
    float terminatorSizePixels = 10.0f;
    Behind behind = Behind::Layer;
    std::string font; // as a text's
};

enum class Corner : uint8_t { TopLeft = 0, TopRight = 1, BottomLeft = 2, BottomRight = 3 };

// A colour bar fixed to the view, with its range written at `ticks` places -- or at
// `tickValues`, saying `tickLabels`. For a layout of one's own (several ramps, swatches,
// rows of figures), a HUD panel's `ramp` item is the same bar inside a panel.
struct Legend {
    std::string title;
    std::string unit;
    Colormap colormap;
    Corner corner = Corner::BottomRight;
    float offsetPixels[2] = { 16.0f, 16.0f }; // inwards from the corner
    // Anywhere instead: the legend's `corner` put at this fraction of the view.
    bool placed = false;
    float screen[2] = { 0.0f, 0.0f };
    bool horizontal = false; // the bar runs across, low values on the left
    float lengthPixels = 160.0f;
    float widthPixels = 12.0f;
    uint32_t ticks = 5;
    uint32_t decimals = 1;
    std::vector<double> tickValues;      // empty: `ticks` evenly over the range
    std::vector<std::string> tickLabels; // empty: each tick's value
    float sizePixels = 11.0f;
    float titleSizePixels = 0.0f; // 0: a little larger than the ticks
    uint32_t rgba = 0xFFFFFFFFu;
    // ⚠️ A PANEL, NOT A HALO, BY DEFAULT. Small labels over a busy model read on a
    // panel; a halo that small has almost no distance to work with (OverlayText.cpp).
    uint32_t haloRgba = 0x00000000u;
    float haloPixels = kAutoHalo;          // as a text's
    uint32_t backgroundRgba = 0x1E2228C8u; // alpha 0: no panel
    float paddingPixels = 8.0f;
    uint32_t barBorderRgba = 0; // a frame round the bar; alpha 0: none
    std::string font;           // as a text's
};

// ---- HUD panels: Dear ImGui over the view (OverlayHud.hpp) ------------------------------
// ⚠️ THE AREA UNDER A PANEL IS THE HUD'S: a click there never reaches Archicad, and the
// pointer over it is ImGui's (OverlayInput.hpp).

enum class PanelAnchor : uint8_t {
    TopLeft = 0,
    Top = 1,
    TopRight = 2,
    Left = 3,
    Center = 4,
    Right = 5,
    BottomLeft = 6,
    Bottom = 7,
    BottomRight = 8,
};

enum class ItemKind : uint8_t {
    Text = 0,      // a line; wrapped at the panel's width when `wrap`
    Row = 1,       // `text` and `value` in two aligned columns with the rows beside it
    Separator = 2, // a rule
    Spacing = 3,   // `heightPixels` of nothing
    Progress = 4,  // a bar filled to `fraction`, `text` over it
    Swatch = 5,    // a square of `rgba` and its `text`: a key entry
    Ramp = 6,      // a colour bar over `colormap`'s range with its ticks: a legend
    Plot = 7,      // `values` as a line
    Table = 8,     // `columns` over `rows`
    Section = 9,   // a heading with a chevron, `value` at its right: the items after it, up to
                   // the next section, fold under it; `open` is how it starts; `info` behind (i)
    Metrics = 10,  // figures in a grid, `perRow` to a row: `rows` are [label, value] cells
    Stack = 11,    // `values` as shares of one bar, `colors` and `labels` each
    Bars = 12,     // `values` as a histogram, `colors` and `labels` each, `text` its caption
    // ---- controls: the add-on holds their values and reports each change (OverlayHudEvents)
    Checkbox = 13, // `text` beside a box, `checked` how it starts
    Slider = 14,   // `text` over a bar from `min` to `max`, `number` where it starts, `step` 0: any
    Combo = 15,    // a dropdown: `text` over it, `labels` its options, `selected` the one it starts on
    Tab = 16,      // a tab of the panel's one tab bar, `text` its title: the items after it, up to
                   // the next tab, are its page; the first tab's `id` and `selected` are the bar's
    Button = 17,   // `text` on it; each press is reported
};

// A control's `id` names its value and its events: at most this long, one per panel.
constexpr size_t kMaxControlId = 64;
// A dropdown's options, a panel's tabs.
constexpr size_t kMaxOptions = 64;
constexpr size_t kMaxTabs = 16;

struct PanelItem {
    ItemKind kind = ItemKind::Text;
    std::string text;        // the line, a row's label, a bar's caption, a swatch's name, a ramp's title
    std::string value;       // a row's value
    uint32_t rgba = 0;       // alpha 0: the panel's text colour; a swatch's, a bar's, a line's colour
    float sizePixels = 0.0f; // a text's or a row's font; 0: the panel's
    bool wrap = false;       // a text
    double fraction = 0.0;   // a progress bar, 0 to 1
    Colormap colormap;       // a ramp, its min and max given
    uint32_t ticks = 5;      // a ramp
    uint32_t decimals = 1;   // a ramp's tick values
    std::string unit;        // after a ramp's last tick
    std::vector<double> tickValues;
    std::vector<std::string> tickLabels;
    float widthPixels = 0.0f;   // a ramp, a bar, a plot; 0: the panel's width
    float heightPixels = 0.0f;  // a ramp's bar, a bar, a plot, a spacing; 0: its own default
    std::vector<double> values; // a plot
    double min = 0.0;           // a plot's range...
    double max = 0.0;
    bool autoRange = true;                      // ...or its values' own
    std::vector<std::string> columns;           // a table's header; empty: none
    std::vector<std::vector<std::string>> rows; // a table's cells, a grid's [label, value]
    bool open = true;                           // a section, until the user folds it
    std::string info;                           // a section's help, behind its (i)
    uint32_t perRow = 0;                        // a grid's and a key's cells to a row; 0: 2, 1
    std::vector<uint32_t> colors;               // a stack's segments, a histogram's bars
    std::vector<std::string> labels;            // ...and what each is; a dropdown's options
    // A control's: its name in its value and its events, and where it starts -- a
    // checkbox's `checked`, a slider's `number` (on `step`s from `min`, 0 any), a
    // dropdown's or a tab bar's `selected`. The add-on keeps what the user sets; a value
    // set again only wins when it differs from the one set before (OverlayHud.hpp).
    std::string id;
    bool checked = false;
    double number = 0.0;
    double step = 0.0;
    uint32_t selected = 0;
};

// A panel's look: the dark glass the HUD began with, or the light card of the design.
// It sets the colours, rounding and padding a panel does not give (the reader applies it).
enum class PanelTheme : uint8_t { Dark = 0, Light = 1 };

// A panel, anchored to the view: its own `anchor` point put at the same point of the
// view, `offsetPixels` inwards. Laid out by Dear ImGui (auto-sized unless
// `widthPixels`), drawn by the guest (§12b: ImGui through Diligent). A panel with a
// title is a tab of the HUD's one floating panel (OverlayHud.hpp), which starts where the
// tab it shows asks and in its look; `collapsed` on the first titled panel starts that
// floating panel closed, to the dock's tab. What the user does outlives the layer being
// set again.
struct Panel {
    std::string title;
    PanelAnchor anchor = PanelAnchor::TopLeft;
    float offsetPixels[2] = { 16.0f, 16.0f };
    float widthPixels = 0.0f; // 0: as wide as its content
    float sizePixels = 14.0f; // the font
    uint32_t textRgba = 0xF0F0F0FFu;
    uint32_t backgroundRgba = 0x1E2228D8u;
    uint32_t borderRgba = 0; // alpha 0: none
    float roundingPixels = 6.0f;
    float paddingPixels = 10.0f;
    // What the pointer can press is tinted with it when pointed at and pressed: a
    // button, a section's row, the title bar's arrow.
    uint32_t accentRgba = 0x3D8BFDFFu;
    std::string font; // as a text's; ImGui rasterises it (OverlayHud.hpp)
    bool collapsed = false;
    PanelTheme theme = PanelTheme::Dark;
    std::vector<PanelItem> items;
};

// A theme's colours, rounding and padding, over `panel`'s own defaults.
void ApplyTheme (Panel& panel, PanelTheme theme);

struct Layer {
    std::string name;
    Views views = Views::Both;
    // 3D only: what an item that names no occlusion of its own does behind the
    // building's opaque surfaces (the host occluder). Hidden, as the reference wireframe
    // is, unless the caller says otherwise. Never `Behind::Layer`.
    Behind occlusion = Behind::Hide;
    std::vector<Polyline> polylines;
    std::vector<PointSet> points;
    std::vector<Mesh> meshes;
    std::vector<Text> texts;
    std::vector<Dimension> dimensions;
    std::vector<Legend> legends;
    std::vector<Panel> panels;
};

// Which renderer draws a primitive -- see the header's second note. The raw pipelines
// know only hidden and over-everything, so an item a layer fades or dashes is the guest's.
bool DrawnByGuest (const Polyline& polyline, const Layer& layer);
bool DrawnByGuest (const Mesh& mesh, const Layer& layer);
// True when anything in the layer needs the guest.
bool NeedsGuest (const Layer& layer);

// `Behind::Layer` resolved against the layer.
Behind Resolve (Behind behind, const Layer& layer);

// What `Validate` refused, in a sentence a caller can act on. Empty when valid.
std::string Validate (const Layer& layer);

struct Summary {
    std::string name;
    Views views = Views::Both;
    Behind occlusion = Behind::Hide;
    uint32_t polylines = 0;
    uint32_t lineVertices = 0;
    uint32_t points = 0;
    uint32_t meshes = 0;
    uint32_t triangles = 0;
    uint32_t texts = 0;
    uint32_t dimensions = 0;
    uint32_t legends = 0;
    uint32_t panels = 0;
};
Summary Summarise (const Layer& layer);

// ---- the store, MAIN THREAD --------------------------------------------------

// ⚠️ NAMES STARTING `tapioca.` ARE THE ADD-ON'S OWN LAYERS -- the storey slices and
// the Watch annotations -- which their own verbs switch. A caller cannot set one
// (the verb refuses the prefix), and `ClearAll` leaves them to those verbs.
constexpr const char* kReservedPrefix = "tapioca.";
bool Reserved (const std::string& name);

// Replace (or add) the layer of this name. Returns the store's new generation.
uint64_t Set (Layer layer);
// Remove one layer; false when there was none of that name.
bool Clear (const std::string& name);
// Every caller layer; the reserved ones stay.
void ClearAll ();
// Every layer, reserved ones too: a project close (§8).
void ClearEverything ();
// Every layer, in the order they were first set -- the draw order.
std::vector<std::shared_ptr<const Layer>> Layers ();
// Moves on every change; a renderer rebuilds when it differs from what it holds.
uint64_t Generation ();

// ---- preparation for the 2D overlay (pure) ------------------------------------

// One stroke instance: the segment's hi/lo halves (PlanOverlayContent's layout),
// then its colour and width. Six corners are made of it in the vertex shader.
struct StrokeInstance {
    plancontent::Segment segment;
    uint32_t rgba = 0; // R in the low byte: DXGI_FORMAT_R8G8B8A8_UNORM
    float widthPixels = 0.0f;
};
static_assert (sizeof (StrokeInstance) == 40, "the 2D layer stroke is 40 bytes");

// One filled vertex: its hi/lo halves and its colour.
struct FillVertex {
    float x = 0.0f, y = 0.0f;     // hi
    float xLo = 0.0f, yLo = 0.0f; // lo
    uint32_t rgba = 0;
};
static_assert (sizeof (FillVertex) == 20, "the 2D layer fill vertex is 20 bytes");

struct Prepared2D {
    double originX = 0.0; // model metres: the centre every half is relative to
    double originY = 0.0;
    std::vector<StrokeInstance> strokes; // polylines and point markers
    std::vector<FillVertex> fills;       // triangle list
};

// The layers drawn in 2D, in draw order: fills first, strokes over them. A point
// marker is four strokes of its square.
Prepared2D Prepare2D (const std::vector<std::shared_ptr<const Layer>>& layers);

// ---- preparation for the 3D overlay (pure) ------------------------------------

// World float position and colour: the building's own coordinates (HostOccluders).
struct ColourVertex {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    uint32_t rgba = 0; // R in the low byte
};
static_assert (sizeof (ColourVertex) == 16, "the 3D layer vertex is 16 bytes");

struct Prepared3D {
    std::vector<ColourVertex> occludedLines; // line list, hidden behind the building
    std::vector<ColourVertex> overLines;     // line list, over everything
    std::vector<ColourVertex> occludedFills; // triangle list
    std::vector<ColourVertex> overFills;
    uint64_t generation = 0;
};

// The layers drawn in 3D. A point marker becomes three axis segments through it,
// each `sizeMetres` long.
Prepared3D Prepare3D (const std::vector<std::shared_ptr<const Layer>>& layers);

// 0xRRGGBBAA (how callers write it) -> R in the low byte (how D3D reads it).
uint32_t ToUnorm (uint32_t rgba);

} // namespace overlaylayers
} // namespace archviz
} // namespace geomsrv

#endif
