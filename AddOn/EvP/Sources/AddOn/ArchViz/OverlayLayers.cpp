// ArchViz/OverlayLayers -- see the header.

#include "ArchViz/OverlayLayers.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace geomsrv {
namespace archviz {
namespace overlaylayers {

namespace {

constexpr size_t kMaxNameLength = 64;
// Two points closer than this are one point: a segment between them has no
// direction, and the stroke shader would draw it as a marker.
constexpr double kSamePoint = 1e-9;
constexpr size_t kMaxTextBytes = 512;

std::vector<std::shared_ptr<const Layer>> g_layers; // MAIN THREAD
uint64_t g_generation = 0;

bool FiniteTriples (const std::vector<double>& points)
{
    if (points.size () % 3 != 0)
        return false;
    for (const double value : points)
        if (!std::isfinite (value))
            return false;
    return true;
}

bool Positive (float value, float most)
{
    return std::isfinite (value) && value > 0.0f && value <= most;
}

std::string Numbered (const char* what, size_t index, const std::string& problem)
{
    return std::string (what) + " " + std::to_string (index) + ": " + problem;
}

template <typename Visit> void EachPoint (const Layer& layer, Visit visit)
{
    for (const Polyline& polyline : layer.polylines)
        for (size_t i = 0; i + 2 < polyline.points.size (); i += 3)
            visit (polyline.points[i], polyline.points[i + 1], polyline.points[i + 2]);
    for (const PointSet& set : layer.points)
        for (size_t i = 0; i + 2 < set.points.size (); i += 3)
            visit (set.points[i], set.points[i + 1], set.points[i + 2]);
    for (const Mesh& mesh : layer.meshes)
        for (size_t i = 0; i + 2 < mesh.points.size (); i += 3)
            visit (mesh.points[i], mesh.points[i + 1], mesh.points[i + 2]);
}

void Split (double value, double origin, float& hi, float& lo)
{
    plancontent::Split (value - origin, hi, lo);
}

bool Finite3 (const double (&value)[3])
{
    return std::isfinite (value[0]) && std::isfinite (value[1]) && std::isfinite (value[2]);
}

bool InRange (float value, float least, float most)
{
    return std::isfinite (value) && value >= least && value <= most;
}

std::string ValidateColormap (const Colormap& colormap)
{
    if (colormap.stops.size () < 2 || colormap.stops.size () > kMaxStops)
        return "a colour ramp has 2 to 16 stops";
    for (size_t i = 0; i < colormap.stops.size (); ++i) {
        if (!InRange (colormap.stops[i].at, 0.0f, 1.0f))
            return "a stop's position is 0 to 1";
        if (i > 0 && colormap.stops[i].at < colormap.stops[i - 1].at)
            return "stops are in ascending order";
    }
    if (!colormap.autoRange &&
        !(std::isfinite (colormap.min) && std::isfinite (colormap.max) && colormap.min < colormap.max))
        return "a ramp's min is below its max";
    if (colormap.bands > 64)
        return "bands is 0 (smooth) to 64";
    if (!(std::isfinite (colormap.isolineStep) && colormap.isolineStep >= 0.0))
        return "isolines' step is 0 (none) or positive";
    if (!InRange (colormap.isolineWidthPixels, 0.25f, 16.0f))
        return "isolines' widthPixels is 0.25 to 16";
    return std::string ();
}

// The ramps, as the matplotlib and ColorBrewer originals at nine even stops -- enough
// that a linear interpolation between them is within a shade of the real curve.
struct Preset {
    const char* name;
    uint32_t rgb[9];
};
constexpr Preset kPresets[] = {
    { "viridis", { 0x440154, 0x472D7B, 0x3B528B, 0x2C728E, 0x21918C, 0x28AE80, 0x5EC962, 0xADDC30, 0xFDE725 } },
    { "inferno", { 0x000004, 0x1F0C48, 0x550F6D, 0x88226A, 0xBA3655, 0xE35933, 0xF98E09, 0xF8C932, 0xFCFFA4 } },
    { "magma", { 0x000004, 0x1C1044, 0x4F127B, 0x812581, 0xB5367A, 0xE55064, 0xFB8761, 0xFEC287, 0xFCFDBF } },
    { "plasma", { 0x0D0887, 0x4C02A1, 0x7E03A8, 0xA92395, 0xCC4778, 0xE56B5D, 0xF89441, 0xFDC328, 0xF0F921 } },
    { "turbo", { 0x30123B, 0x4662D7, 0x36AAF9, 0x1AE4B6, 0x72FE5E, 0xC8EF34, 0xFABA39, 0xF66B19, 0x7A0403 } },
    { "coolwarm", { 0x3B4CC0, 0x6282EA, 0x8DB0FE, 0xB8D0F9, 0xDDDDDD, 0xF5C4AD, 0xF49A7B, 0xDE604D, 0xB40426 } },
    { "greys", { 0x000000, 0x202020, 0x404040, 0x606060, 0x808080, 0x9F9F9F, 0xBFBFBF, 0xDFDFDF, 0xFFFFFF } },
    // Few hours of sun reads cold and dark, many reads warm and light.
    { "sunhours", { 0x081D58, 0x253494, 0x225EA8, 0x1D91C0, 0x41B6C4, 0x7FCDBB, 0xC7E9B4, 0xFEE391, 0xFEC44F } },
    // Flat to steep: ColorBrewer RdYlGn, reversed.
    { "slope", { 0x1A9850, 0x66BD63, 0xA6D96A, 0xD9EF8B, 0xFFFFBF, 0xFEE08B, 0xFDAE61, 0xF46D43, 0xD73027 } },
    // Too low to clear: the same ramp the other way -- red is the problem.
    { "clearance", { 0xD73027, 0xF46D43, 0xFDAE61, 0xFEE08B, 0xFFFFBF, 0xD9EF8B, 0xA6D96A, 0x66BD63, 0x1A9850 } },
};

// What a HUD panel may hold. Its budgets keep one panel to a size ImGui lays out in
// well under a frame, on the main thread, whenever the layers change.
std::string ValidatePanel (const Panel& panel)
{
    if (panel.title.size () > kMaxTextBytes)
        return "title is at most 512 bytes";
    if (!panel.tab.empty () && panel.tab != "stats")
        return "tab is \"stats\" -- a card on the HUD's Stats page -- or absent";
    if (!std::isfinite (panel.offsetPixels[0]) || !std::isfinite (panel.offsetPixels[1]) ||
        !InRange (panel.widthPixels, 0.0f, 4000.0f) || !InRange (panel.sizePixels, 6.0f, 96.0f) ||
        !InRange (panel.roundingPixels, 0.0f, 64.0f) || !InRange (panel.paddingPixels, 0.0f, 64.0f))
        return "offsetPixels finite, widthPixels 0 to 4000, sizePixels 6 to 96, rounding and padding 0 to 64";
    if (panel.items.size () > 200)
        return "at most 200 items";
    std::set<std::string> ids;
    size_t tabs = 0;
    for (size_t k = 0; k < panel.items.size (); ++k) {
        const PanelItem& item = panel.items[k];
        const std::string at = "item " + std::to_string (k) + ": ";
        const bool control = item.kind == ItemKind::Checkbox || item.kind == ItemKind::Slider ||
                             item.kind == ItemKind::Combo || item.kind == ItemKind::Button ||
                             item.kind == ItemKind::SitePlan;
        if (control && (item.id.empty () || item.id.size () > kMaxControlId))
            return at + "a checkbox, slider, dropdown or button needs its id, at most 64 bytes: its value and "
                        "its events are named by it";
        if (item.kind == ItemKind::Tab && tabs == 0 && item.id.size () > kMaxControlId)
            return at + "the tab bar's id is at most 64 bytes";
        if (control && !ids.insert (item.id).second)
            return at + "two controls of one panel share the id \"" + item.id + "\"";
        if (item.kind == ItemKind::SitePlan) {
            const size_t count = item.outlineXY.size () / 2;
            if (item.id.size () > 44 || count < 3 || count > 256 || item.outlineXY.size () % 2 != 0 ||
                item.offsetXY.size () % 2 != 0 || item.offsetXY.size () > 512 ||
                (!item.offsetXY.empty () && item.offsetXY.size () < 6) || item.selected >= count ||
                item.setbackDistances.size () != count || item.setbackModes.size () != count ||
                !std::isfinite (item.number) || item.number < 0 || item.number > 1000)
                return at + "sitePlan needs a 3..256 vertex XY ring, matching setbacks/modes, selected vertex and id "
                            "<=44 bytes";
            for (double value : item.outlineXY)
                if (!std::isfinite (value))
                    return at + "sitePlan coordinates must be finite";
            for (double value : item.offsetXY)
                if (!std::isfinite (value))
                    return at + "sitePlan coordinates must be finite";
            for (double value : item.setbackDistances)
                if (!std::isfinite (value) || value < 0 || value > 1000)
                    return at + "sitePlan setbacks are 0..1000 m";
            for (uint32_t mode : item.setbackModes)
                if (mode > 2)
                    return at + "sitePlan modes are Default, Custom or None";
            for (uint32_t vertex : item.referenceVertices)
                if (vertex >= count)
                    return at + "sitePlan reference vertex out of range";
        }
        if (item.kind == ItemKind::Slider &&
            (item.autoRange || !std::isfinite (item.number) || !std::isfinite (item.step) || item.step < 0.0))
            return at + "a slider needs its min and max, min below max, a finite number and a step of 0 or more";
        if (item.kind == ItemKind::Combo &&
            (item.labels.empty () || item.labels.size () > kMaxOptions || item.selected >= item.labels.size ()))
            return at + "a dropdown has 1 to 64 options, and starts on one of them";
        if ((item.kind == ItemKind::Tab || item.kind == ItemKind::Button || item.kind == ItemKind::Checkbox) &&
            item.text.empty ())
            return at + "a tab, a button and a checkbox need their text: it is what the user presses";
        if (item.kind == ItemKind::Tab && ++tabs > kMaxTabs)
            return at + "a panel has at most 16 tabs";
        if (item.text.size () > 4096 || item.value.size () > kMaxTextBytes || item.unit.size () > 64)
            return at + "text is at most 4096 bytes, value 512 and unit 64";
        if (!InRange (item.sizePixels, 0.0f, 96.0f) || !InRange (item.widthPixels, 0.0f, 4000.0f) ||
            !InRange (item.heightPixels, 0.0f, 2000.0f))
            return at + "sizePixels 0 to 96, widthPixels 0 to 4000, heightPixels 0 to 2000";
        if (!std::isfinite (item.fraction))
            return at + "fraction is finite";
        if (item.kind == ItemKind::Section && item.text.empty ())
            return at + "a section needs its title: it is what the user clicks";
        if (item.info.size () > kMaxTextBytes || item.perRow > 4)
            return at + "info is at most 512 bytes and perRow at most 4";
        if (item.colors.size () > 256 || item.labels.size () > 256)
            return at + "at most 256 colors and labels";
        for (const std::string& label : item.labels)
            if (label.size () > kMaxTextBytes)
                return at + "a label is at most 512 bytes";
        if (item.kind == ItemKind::Stack) {
            if (item.values.size () > 64)
                return at + "a stack has at most 64 segments";
            double total = 0.0;
            for (const double value : item.values) {
                if (!std::isfinite (value) || value < 0.0)
                    return at + "a stack's values are finite and not negative";
                total += value;
            }
            if (!(total > 0.0))
                return at + "a stack needs a value above zero";
        }
        if (item.kind == ItemKind::Bars && (item.values.empty () || item.values.size () > 256))
            return at + "a histogram has 1 to 256 values";
        if (item.kind == ItemKind::Ramp) {
            const std::string ramp = ValidateColormap (item.colormap);
            if (!ramp.empty ())
                return at + ramp;
            if (item.colormap.autoRange)
                return at + "a ramp needs its min and max";
            if (item.ticks > 32 || item.decimals > 6 || item.tickValues.size () > 32 || item.tickLabels.size () > 32)
                return at + "a ramp has at most 32 ticks and 6 decimals";
            for (const double value : item.tickValues)
                if (!std::isfinite (value))
                    return at + "tickValues are finite";
        }
        if (item.values.size () > 4096)
            return at + "a plot has at most 4096 values";
        for (const double value : item.values)
            if (!std::isfinite (value))
                return at + "a plot's values are finite";
        if (!item.autoRange && !(std::isfinite (item.min) && std::isfinite (item.max) && item.max > item.min))
            return at + "a plot's max is above its min";
        if (item.columns.size () > 16 || item.rows.size () > 200)
            return at + "a table has at most 16 columns and 200 rows";
        for (const std::vector<std::string>& row : item.rows) {
            if (row.size () > 16)
                return at + "a table row has at most 16 cells";
            for (const std::string& cell : row)
                if (cell.size () > kMaxTextBytes)
                    return at + "a table cell is at most 512 bytes";
        }
    }
    // The tab bar starts on one of its tabs.
    for (const PanelItem& item : panel.items)
        if (item.kind == ItemKind::Tab) {
            if (item.selected >= tabs)
                return "the tab bar starts on one of its " + std::to_string (tabs) + " tabs";
            if (!item.id.empty () && ids.count (item.id) != 0)
                return "the tab bar's id \"" + item.id + "\" is a control's too";
            break;
        }
    return std::string ();
}

} // namespace

void ApplyTheme (Panel& panel, PanelTheme theme)
{
    panel.theme = theme;
    if (theme != PanelTheme::Light)
        return;
    // The design's card: near-white, dark text, a hairline border, rounded well.
    panel.textRgba = 0x1F2328FFu;
    panel.backgroundRgba = 0xFFFFFFF2u;
    panel.borderRgba = 0xD0D7DEFFu;
    panel.accentRgba = 0x2F6FEBFFu;
    panel.roundingPixels = 10.0f;
    panel.paddingPixels = 8.0f;
}

bool DrawnIn (Views views, Views view)
{
    return (uint32_t (views) & uint32_t (view)) != 0;
}

bool PresetStops (const std::string& name, std::vector<ColourStop>& stops)
{
    for (const Preset& preset : kPresets) {
        if (name != preset.name)
            continue;
        stops.clear ();
        for (size_t i = 0; i < 9; ++i)
            stops.push_back ({ float (i) / 8.0f, (preset.rgb[i] << 8) | 0xFFu });
        return true;
    }
    return false;
}

namespace {

bool Styled (const HiddenLine& hidden)
{
    return (hidden.rgba & 0xFFu) != 0 || hidden.widthPixels > 0.0f || !hidden.dashMetres.empty ();
}

// At most kMaxDashEntries lengths, each finite and 0 to 1000 m, some of them above 0.
bool ValidDash (const std::vector<float>& lengths)
{
    if (lengths.empty ())
        return true;
    if (lengths.size () > kMaxDashEntries)
        return false;
    float total = 0.0f;
    for (const float length : lengths) {
        if (!std::isfinite (length) || length < 0.0f || length > 1000.0f)
            return false;
        total += length;
    }
    return total > 0.0f;
}

// What the raw pipelines can draw: hidden behind the building, or over it.
bool RawOcclusion (const Layer& layer)
{
    return layer.occlusion == Behind::Hide || layer.occlusion == Behind::Show;
}

} // namespace

bool DrawnByGuest (const Polyline& polyline, const Layer& layer)
{
    return !polyline.dashMetres.empty () || Styled (polyline.hidden) || polyline.behind != Behind::Layer ||
           !RawOcclusion (layer) ||
           (!polyline.closed && (polyline.startArrow != Terminator::None || polyline.endArrow != Terminator::None));
}

bool DrawnByGuest (const Mesh& mesh, const Layer& layer)
{
    return mesh.styled || !mesh.values.empty () || !mesh.normals.empty () || !RawOcclusion (layer);
}

bool NeedsGuest (const Layer& layer)
{
    if (!layer.texts.empty () || !layer.dimensions.empty () || !layer.legends.empty () || !layer.panels.empty ())
        return true;
    for (const Polyline& polyline : layer.polylines)
        if (DrawnByGuest (polyline, layer))
            return true;
    for (const Mesh& mesh : layer.meshes)
        if (DrawnByGuest (mesh, layer))
            return true;
    return false;
}

Behind Resolve (Behind behind, const Layer& layer)
{
    if (behind != Behind::Layer)
        return behind;
    return layer.occlusion == Behind::Layer ? Behind::Hide : layer.occlusion;
}

std::string Validate (const Layer& layer)
{
    if (layer.name.empty () || layer.name.size () > kMaxNameLength)
        return "a layer needs a name of 1 to 64 characters";
    for (size_t i = 0; i < layer.polylines.size (); ++i) {
        const Polyline& polyline = layer.polylines[i];
        if (!FiniteTriples (polyline.points))
            return Numbered ("polyline", i, "points are finite x, y, z triples");
        if (polyline.points.size () < 6)
            return Numbered ("polyline", i, "needs at least two points");
        if (!Positive (polyline.widthPixels, 64.0f))
            return Numbered ("polyline", i, "widthPixels must be above 0 and at most 64");
        if (!ValidDash (polyline.dashMetres) || !ValidDash (polyline.hidden.dashMetres))
            return Numbered ("polyline", i, "a dash pattern is at most 8 lengths of 0 to 1000 m, not all 0");
        if (!InRange (polyline.hidden.widthPixels, 0.0f, 64.0f))
            return Numbered ("polyline", i, "hidden widthPixels is 0 to 64");
        if (!InRange (polyline.arrowSizePixels, 1.0f, 64.0f))
            return Numbered ("polyline", i, "arrowSizePixels is 1 to 64");
    }
    for (size_t i = 0; i < layer.points.size (); ++i) {
        const PointSet& set = layer.points[i];
        if (!FiniteTriples (set.points) || set.points.empty ())
            return Numbered ("point set", i, "points are finite x, y, z triples, at least one");
        if (!Positive (set.sizePixels, 256.0f))
            return Numbered ("point set", i, "sizePixels must be above 0 and at most 256");
        if (!Positive (set.sizeMetres, 1000.0f))
            return Numbered ("point set", i, "sizeMetres must be above 0 and at most 1000");
    }
    for (size_t i = 0; i < layer.meshes.size (); ++i) {
        const Mesh& mesh = layer.meshes[i];
        if (!FiniteTriples (mesh.points) || mesh.points.size () < 9)
            return Numbered ("mesh", i, "points are finite x, y, z triples, at least three");
        if (mesh.indices.empty () || mesh.indices.size () % 3 != 0)
            return Numbered ("mesh", i, "indices come in threes, one triple per triangle");
        const size_t vertices = mesh.points.size () / 3;
        for (const uint32_t index : mesh.indices)
            if (index >= vertices)
                return Numbered ("mesh", i,
                                 "index " + std::to_string (index) + " is past its " + std::to_string (vertices) +
                                     " vertices");
        if (!mesh.vertexRgba.empty () && mesh.vertexRgba.size () != vertices)
            return Numbered ("mesh", i, "vertexColors has one colour per vertex or none");
        if (!mesh.normals.empty () && (mesh.normals.size () != vertices * 3 || !FiniteTriples (mesh.normals)))
            return Numbered ("mesh", i, "normals has one finite x, y, z per vertex or none");
        if (!mesh.values.empty ()) {
            if (mesh.values.size () != vertices)
                return Numbered ("mesh", i, "values has one number per vertex or none");
            for (const double value : mesh.values)
                if (!std::isfinite (value))
                    return Numbered ("mesh", i, "values are finite");
            const std::string ramp = ValidateColormap (mesh.colormap);
            if (!ramp.empty ())
                return Numbered ("mesh", i, ramp);
        }
        const MeshStyle& style = mesh.style;
        if (!InRange (style.opacity, 0.0f, 1.0f) || !InRange (style.edgeWidthPixels, 0.25f, 16.0f) ||
            !InRange (style.edgeAngleDegrees, 0.0f, 180.0f))
            return Numbered ("mesh", i, "opacity is 0 to 1, edge widthPixels 0.25 to 16, angleDegrees 0 to 180");
    }
    for (size_t i = 0; i < layer.texts.size (); ++i) {
        const Text& text = layer.texts[i];
        if (text.text.empty () || text.text.size () > kMaxTextBytes)
            return Numbered ("text", i, "text is 1 to 512 bytes of UTF-8");
        if (!Finite3 (text.at) || !std::isfinite (text.offsetPixels[0]) || !std::isfinite (text.offsetPixels[1]))
            return Numbered ("text", i, "its position and offset are finite");
        if (text.screen && !(InRange (float (text.at[0]), -1.0f, 2.0f) && InRange (float (text.at[1]), -1.0f, 2.0f)))
            return Numbered ("text", i, "a screen position is a fraction of the view, about 0 to 1");
        if (!InRange (text.sizePixels, 4.0f, 256.0f) ||
            (text.haloPixels != kAutoHalo && !InRange (text.haloPixels, 0.0f, 8.0f)) ||
            !InRange (text.haloScale, 0.0f, 8.0f) || !std::isfinite (text.rotationDegrees))
            return Numbered ("text", i, "sizePixels is 4 to 256, haloPixels 0 to 8 and haloScale 0 to 8");
        if (text.planar) {
            if (text.screen)
                return Numbered ("text", i, "lies on a plane in the model or on the screen, not both");
            if (!Finite3 (text.direction) || !Finite3 (text.normal) || !std::isfinite (text.sizeMetres) ||
                text.sizeMetres < 0.001 || text.sizeMetres > 1000.0)
                return Numbered ("text", i, "a plane's direction and normal are finite, sizeMetres 0.001 to 1000");
            const double* d = text.direction;
            const double* n = text.normal;
            const double cx = n[1] * d[2] - n[2] * d[1], cy = n[2] * d[0] - n[0] * d[2], cz = n[0] * d[1] - n[1] * d[0];
            const double lengths =
                std::sqrt ((d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) * (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]));
            if (!(lengths > 1e-12) || std::sqrt (cx * cx + cy * cy + cz * cz) < 1e-6 * lengths)
                return Numbered ("text", i, "a plane's direction lies across its normal, neither of them zero");
        }
    }
    for (size_t i = 0; i < layer.dimensions.size (); ++i) {
        const Dimension& dimension = layer.dimensions[i];
        if (!Finite3 (dimension.from) || !Finite3 (dimension.to) || !Finite3 (dimension.direction) ||
            !Finite3 (dimension.normal) || !std::isfinite (dimension.offsetMetres) ||
            std::fabs (dimension.offsetMetres) > 1.0e4)
            return Numbered ("dimension", i, "its points, direction, normal and offset are finite");
        const double dx = dimension.to[0] - dimension.from[0], dy = dimension.to[1] - dimension.from[1],
                     dz = dimension.to[2] - dimension.from[2];
        if (dx * dx + dy * dy + dz * dz < kSamePoint * kSamePoint)
            return Numbered ("dimension", i, "measures between two different points");
        if (dimension.decimals > 6 || dimension.text.size () > kMaxTextBytes)
            return Numbered ("dimension", i, "decimals is 0 to 6 and text at most 512 bytes");
        if (!Positive (dimension.widthPixels, 16.0f) || !InRange (dimension.textSizePixels, 4.0f, 128.0f))
            return Numbered ("dimension", i, "widthPixels is above 0 to 16 and textSizePixels 4 to 128");
        if (!InRange (dimension.terminatorSizePixels, 1.0f, 64.0f) ||
            (dimension.haloPixels != kAutoHalo && !InRange (dimension.haloPixels, 0.0f, 8.0f)))
            return Numbered ("dimension", i, "terminatorSizePixels is 1 to 64 and haloPixels 0 to 8");
    }
    for (size_t i = 0; i < layer.legends.size (); ++i) {
        const Legend& legend = layer.legends[i];
        const std::string ramp = ValidateColormap (legend.colormap);
        if (!ramp.empty ())
            return Numbered ("legend", i, ramp);
        if (legend.colormap.autoRange)
            return Numbered ("legend", i, "needs min and max, or the mesh it describes");
        if (!InRange (legend.lengthPixels, 20.0f, 4000.0f) || !InRange (legend.widthPixels, 2.0f, 200.0f) ||
            legend.ticks > 32 || legend.decimals > 6 || !InRange (legend.sizePixels, 4.0f, 128.0f) ||
            !std::isfinite (legend.offsetPixels[0]) || !std::isfinite (legend.offsetPixels[1]))
            return Numbered ("legend", i, "lengthPixels 20 to 4000, widthPixels 2 to 200, ticks at most 32");
        if (legend.title.size () > kMaxTextBytes || legend.unit.size () > 64)
            return Numbered ("legend", i, "title is at most 512 bytes and unit 64");
        if (legend.haloPixels != kAutoHalo && !InRange (legend.haloPixels, 0.0f, 8.0f))
            return Numbered ("legend", i, "haloPixels is 0 to 8");
        if (legend.placed && !(InRange (legend.screen[0], -1.0f, 2.0f) && InRange (legend.screen[1], -1.0f, 2.0f)))
            return Numbered ("legend", i, "a screen position is a fraction of the view, about 0 to 1");
        if (legend.tickValues.size () > 32 || legend.tickLabels.size () > 32)
            return Numbered ("legend", i, "at most 32 tickValues and tickLabels");
        for (const double value : legend.tickValues)
            if (!std::isfinite (value))
                return Numbered ("legend", i, "tickValues are finite");
        for (const std::string& label : legend.tickLabels)
            if (label.size () > 128)
                return Numbered ("legend", i, "a tick label is at most 128 bytes");
        if (!InRange (legend.titleSizePixels, 0.0f, 128.0f) || !InRange (legend.paddingPixels, 0.0f, 64.0f))
            return Numbered ("legend", i, "titleSizePixels is 0 to 128 and paddingPixels 0 to 64");
    }
    for (size_t i = 0; i < layer.panels.size (); ++i) {
        const std::string refused = ValidatePanel (layer.panels[i]);
        if (!refused.empty ())
            return Numbered ("panel", i, refused);
    }
    return std::string ();
}

Summary Summarise (const Layer& layer)
{
    Summary summary;
    summary.name = layer.name;
    summary.views = layer.views;
    summary.occlusion = layer.occlusion;
    summary.polylines = uint32_t (layer.polylines.size ());
    for (const Polyline& polyline : layer.polylines)
        summary.lineVertices += uint32_t (polyline.points.size () / 3);
    for (const PointSet& set : layer.points)
        summary.points += uint32_t (set.points.size () / 3);
    summary.meshes = uint32_t (layer.meshes.size ());
    for (const Mesh& mesh : layer.meshes)
        summary.triangles += uint32_t (mesh.indices.size () / 3);
    summary.texts = uint32_t (layer.texts.size ());
    summary.dimensions = uint32_t (layer.dimensions.size ());
    summary.legends = uint32_t (layer.legends.size ());
    summary.panels = uint32_t (layer.panels.size ());
    return summary;
}

uint64_t Set (Layer layer)
{
    std::shared_ptr<const Layer> shared = std::make_shared<const Layer> (std::move (layer));
    for (std::shared_ptr<const Layer>& existing : g_layers) {
        if (existing->name == shared->name) {
            existing = std::move (shared);
            return ++g_generation;
        }
    }
    g_layers.push_back (std::move (shared));
    return ++g_generation;
}

bool Clear (const std::string& name)
{
    const auto end =
        std::remove_if (g_layers.begin (), g_layers.end (),
                        [&name] (const std::shared_ptr<const Layer>& layer) { return layer->name == name; });
    if (end == g_layers.end ())
        return false;
    g_layers.erase (end, g_layers.end ());
    ++g_generation;
    return true;
}

void ClearAll ()
{
    const auto end =
        std::remove_if (g_layers.begin (), g_layers.end (),
                        [] (const std::shared_ptr<const Layer>& layer) { return !Reserved (layer->name); });
    if (end == g_layers.end ())
        return;
    g_layers.erase (end, g_layers.end ());
    ++g_generation;
}

void ClearEverything ()
{
    if (g_layers.empty ())
        return;
    g_layers.clear ();
    ++g_generation;
}

bool Reserved (const std::string& name)
{
    return name.compare (0, std::strlen (kReservedPrefix), kReservedPrefix) == 0;
}

std::vector<std::shared_ptr<const Layer>> Layers ()
{
    return g_layers;
}

uint64_t Touch ()
{
    return ++g_generation;
}

uint64_t Generation ()
{
    return g_generation;
}

uint32_t ToUnorm (uint32_t rgba)
{
    const uint32_t red = (rgba >> 24) & 0xFFu;
    const uint32_t green = (rgba >> 16) & 0xFFu;
    const uint32_t blue = (rgba >> 8) & 0xFFu;
    const uint32_t alpha = rgba & 0xFFu;
    return red | (green << 8) | (blue << 16) | (alpha << 24);
}

Prepared2D Prepare2D (const std::vector<std::shared_ptr<const Layer>>& layers)
{
    Prepared2D out;
    double sumX = 0.0, sumY = 0.0;
    size_t count = 0;
    for (const std::shared_ptr<const Layer>& layer : layers) {
        if (!DrawnIn (layer->views, Views::TwoD))
            continue;
        EachPoint (*layer, [&] (double x, double y, double) {
            sumX += x;
            sumY += y;
            ++count;
        });
    }
    if (count == 0)
        return out;
    out.originX = sumX / double (count);
    out.originY = sumY / double (count);

    auto stroke = [&out] (double ax, double ay, double bx, double by, uint32_t rgba, float width) {
        StrokeInstance instance;
        Split (ax, out.originX, instance.segment.x0, instance.segment.x0Lo);
        Split (ay, out.originY, instance.segment.y0, instance.segment.y0Lo);
        Split (bx, out.originX, instance.segment.x1, instance.segment.x1Lo);
        Split (by, out.originY, instance.segment.y1, instance.segment.y1Lo);
        instance.rgba = ToUnorm (rgba);
        instance.widthPixels = width;
        out.strokes.push_back (instance);
    };

    // ⚠️ FILLS FIRST, STROKES OVER THEM: an outline drawn under its own fill is an
    // outline nobody sees.
    for (const std::shared_ptr<const Layer>& layer : layers) {
        if (!DrawnIn (layer->views, Views::TwoD))
            continue;
        for (const Mesh& mesh : layer->meshes) {
            if (DrawnByGuest (mesh, *layer))
                continue;
            for (const uint32_t index : mesh.indices) {
                FillVertex vertex;
                Split (mesh.points[size_t (index) * 3], out.originX, vertex.x, vertex.xLo);
                Split (mesh.points[size_t (index) * 3 + 1], out.originY, vertex.y, vertex.yLo);
                vertex.rgba = ToUnorm (mesh.vertexRgba.empty () ? mesh.rgba : mesh.vertexRgba[index]);
                out.fills.push_back (vertex);
            }
        }
    }
    for (const std::shared_ptr<const Layer>& layer : layers) {
        if (!DrawnIn (layer->views, Views::TwoD))
            continue;
        for (const Polyline& polyline : layer->polylines) {
            if (DrawnByGuest (polyline, *layer))
                continue;
            const size_t points = polyline.points.size () / 3;
            const size_t segments = polyline.closed ? points : points - 1;
            for (size_t i = 0; i < segments; ++i) {
                const size_t a = i, b = (i + 1) % points;
                const double ax = polyline.points[a * 3], ay = polyline.points[a * 3 + 1];
                const double bx = polyline.points[b * 3], by = polyline.points[b * 3 + 1];
                if (std::fabs (bx - ax) < kSamePoint && std::fabs (by - ay) < kSamePoint)
                    continue;
                stroke (ax, ay, bx, by, polyline.rgba, polyline.widthPixels);
            }
        }
        // A marker is a stroke of no length: the shader squares it, `sizePixels` wide.
        for (const PointSet& set : layer->points)
            for (size_t i = 0; i + 2 < set.points.size (); i += 3)
                stroke (set.points[i], set.points[i + 1], set.points[i], set.points[i + 1], set.rgba, set.sizePixels);
    }
    return out;
}

Prepared3D Prepare3D (const std::vector<std::shared_ptr<const Layer>>& layers)
{
    Prepared3D out;
    auto vertex = [] (double x, double y, double z, uint32_t rgba) {
        ColourVertex v;
        v.x = float (x);
        v.y = float (y);
        v.z = float (z);
        v.rgba = rgba;
        return v;
    };
    for (const std::shared_ptr<const Layer>& layer : layers) {
        if (!DrawnIn (layer->views, Views::ThreeD))
            continue;
        const bool occluded = layer->occlusion != Behind::Show;
        std::vector<ColourVertex>& lines = occluded ? out.occludedLines : out.overLines;
        std::vector<ColourVertex>& fills = occluded ? out.occludedFills : out.overFills;
        for (const Polyline& polyline : layer->polylines) {
            if (DrawnByGuest (polyline, *layer))
                continue;
            const uint32_t rgba = ToUnorm (polyline.rgba);
            const size_t points = polyline.points.size () / 3;
            const size_t segments = polyline.closed ? points : points - 1;
            for (size_t i = 0; i < segments; ++i) {
                const size_t a = i * 3, b = ((i + 1) % points) * 3;
                lines.push_back (vertex (polyline.points[a], polyline.points[a + 1], polyline.points[a + 2], rgba));
                lines.push_back (vertex (polyline.points[b], polyline.points[b + 1], polyline.points[b + 2], rgba));
            }
        }
        for (const PointSet& set : layer->points) {
            const uint32_t rgba = ToUnorm (set.rgba);
            const double half = double (set.sizeMetres) * 0.5;
            for (size_t i = 0; i + 2 < set.points.size (); i += 3) {
                const double x = set.points[i], y = set.points[i + 1], z = set.points[i + 2];
                lines.push_back (vertex (x - half, y, z, rgba));
                lines.push_back (vertex (x + half, y, z, rgba));
                lines.push_back (vertex (x, y - half, z, rgba));
                lines.push_back (vertex (x, y + half, z, rgba));
                lines.push_back (vertex (x, y, z - half, rgba));
                lines.push_back (vertex (x, y, z + half, rgba));
            }
        }
        for (const Mesh& mesh : layer->meshes) {
            if (DrawnByGuest (mesh, *layer))
                continue;
            for (const uint32_t index : mesh.indices) {
                const size_t at = size_t (index) * 3;
                const uint32_t rgba = ToUnorm (mesh.vertexRgba.empty () ? mesh.rgba : mesh.vertexRgba[index]);
                fills.push_back (vertex (mesh.points[at], mesh.points[at + 1], mesh.points[at + 2], rgba));
            }
        }
    }
    return out;
}

} // namespace overlaylayers
} // namespace archviz
} // namespace geomsrv
