// NativeCommands/OverlayLayerReading -- Tapioca.SetOverlayLayer's parameters read into a
// layer (ArchViz/OverlayLayers.hpp). See the header.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/OverlayLayerReading.hpp"
#include "NativeCommands/CommandUtils.hpp" // ReadReal/ReadReals: a JSON whole number is a number too

#include "ArchViz/OverlayFonts.hpp"
#include "ArchViz/OverlayLayers.hpp"

#include <string>
#include <vector>

namespace geomsrv {
namespace overlayreading {

namespace layers = archviz::overlaylayers;

namespace {

std::string Utf8Of (const GS::UniString& text)
{
    return std::string (text.ToCStr (0, MaxUSize, CC_UTF8).Get ());
}

std::string StringValue (const GS::ObjectState& item, const char* key)
{
    GS::UniString text;
    item.Get (key, text);
    return Utf8Of (text);
}

void ReadNumbers (const GS::ObjectState& item, const char* key, std::vector<double>& out)
{
    std::vector<double> values;
    if (ReadReals (item, key, values))
        out.insert (out.end (), values.begin (), values.end ());
}

void ReadFloat (const GS::ObjectState& item, const char* key, float& out)
{
    double value = 0.0;
    if (ReadReal (item, key, value))
        out = float (value);
}

void ReadDouble (const GS::ObjectState& item, const char* key, double& out)
{
    ReadReal (item, key, out);
}

void ReadCount (const GS::ObjectState& item, const char* key, uint32_t& out)
{
    if (item.Contains (key)) {
        GS::Int32 value = 0;
        item.Get (key, value);
        out = value < 0 ? 0u : uint32_t (value);
    }
}

void ReadStrings (const GS::ObjectState& item, const char* key, std::vector<std::string>& out)
{
    GS::Array<GS::UniString> values;
    if (item.Get (key, values))
        for (const GS::UniString& value : values)
            out.push_back (Utf8Of (value));
}

// A single coordinate is a record, #Point3D {x, y, z} or #Point2D {x, y} (SPEC.md, wire
// shapes); only bulk geometry is a flat array. False when `key` is absent or short.
bool ReadPoint (const GS::ObjectState& item, const char* key, double* out, size_t count)
{
    GS::ObjectState point;
    if (!item.Get (key, point))
        return false;
    const char* const axes[] = { "x", "y", "z" };
    for (size_t i = 0; i < count; ++i)
        if (!ReadReal (point, axes[i], out[i]))
            return false;
    return true;
}

bool ReadPoint2 (const GS::ObjectState& item, const char* key, float (&out)[2])
{
    double values[2] = {};
    if (!ReadPoint (item, key, values, 2))
        return false;
    out[0] = float (values[0]);
    out[1] = float (values[1]);
    return true;
}

// "none", "arrow", "tick" or "dot": what ends a line.
layers::Terminator TerminatorOf (const GS::ObjectState& item, const char* key, layers::Terminator fallback)
{
    if (!item.Contains (key))
        return fallback;
    const std::string which = StringValue (item, key);
    return which == "arrow"  ? layers::Terminator::Arrow
           : which == "dot"  ? layers::Terminator::Dot
           : which == "none" ? layers::Terminator::None
                             : layers::Terminator::Tick;
}

bool ReadColormap (const GS::ObjectState& item, layers::Colormap& colormap, std::string& error)
{
    if (item.Contains ("stops")) {
        GS::Array<GS::ObjectState> stops;
        item.Get ("stops", stops);
        for (const GS::ObjectState& stop : stops) {
            layers::ColourStop value;
            ReadFloat (stop, "at", value.at);
            if (!ReadColour (stop, "color", value.rgba, error))
                return false;
            colormap.stops.push_back (value);
        }
    }
    else {
        const std::string preset = item.Contains ("preset") ? StringValue (item, "preset") : std::string ("viridis");
        if (!layers::PresetStops (preset, colormap.stops)) {
            error = "no colour ramp is called \"" + preset + "\"";
            return false;
        }
    }
    if (item.Contains ("min") != item.Contains ("max")) {
        error = "a ramp's range is min and max together, or neither";
        return false;
    }
    if (item.Contains ("min")) {
        ReadDouble (item, "min", colormap.min);
        ReadDouble (item, "max", colormap.max);
        colormap.autoRange = false;
    }
    ReadCount (item, "bands", colormap.bands);
    GS::ObjectState isolines;
    if (item.Get ("isolines", isolines)) {
        ReadDouble (isolines, "step", colormap.isolineStep);
        if (!ReadColour (isolines, "color", colormap.isolineRgba, error))
            return false;
        ReadFloat (isolines, "widthPixels", colormap.isolineWidthPixels);
    }
    return true;
}

bool ReadMesh (const GS::ObjectState& item, layers::Mesh& mesh, std::string& error)
{
    ReadNumbers (item, "points", mesh.points);
    GS::Array<GS::Int32> indices;
    item.Get ("indices", indices);
    for (const GS::Int32 index : indices) {
        if (index < 0) {
            error = "mesh indices are never negative";
            return false;
        }
        mesh.indices.push_back (uint32_t (index));
    }
    if (!ReadColour (item, "color", mesh.rgba, error))
        return false;
    GS::Array<GS::UniString> colours;
    if (item.Get ("vertexColors", colours)) {
        for (const GS::UniString& colour : colours) {
            GS::ObjectState one;
            one.Add ("c", colour);
            uint32_t rgba = 0;
            if (!ReadColour (one, "c", rgba, error)) {
                error = "vertexColors: " + error;
                return false;
            }
            mesh.vertexRgba.push_back (rgba);
        }
    }
    ReadNumbers (item, "normals", mesh.normals);
    ReadNumbers (item, "values", mesh.values);
    GS::ObjectState colormap;
    if (item.Get ("colormap", colormap)) {
        if (!ReadColormap (colormap, mesh.colormap, error))
            return false;
    }
    else if (!mesh.values.empty () && !layers::PresetStops ("viridis", mesh.colormap.stops)) {
        error = "the default ramp is missing";
        return false;
    }
    GS::ObjectState style;
    if (item.Get ("style", style)) {
        mesh.styled = true;
        const std::string shading = style.Contains ("shading") ? StringValue (style, "shading") : std::string ("flat");
        mesh.style.shading = shading == "lit"     ? layers::Shading::Lit
                             : shading == "ghost" ? layers::Shading::Ghost
                             : shading == "xray"  ? layers::Shading::Xray
                                                  : layers::Shading::Flat;
        ReadFloat (style, "opacity", mesh.style.opacity);
        mesh.style.behind = OcclusionOf (style, layers::Behind::Layer);
        mesh.style.cullBack = style.Contains ("cull") && StringValue (style, "cull") == "back";
        GS::ObjectState edges;
        if (style.Get ("edges", edges)) {
            mesh.style.edgeRgba = 0x202020FFu;
            if (!ReadColour (edges, "color", mesh.style.edgeRgba, error))
                return false;
            ReadFloat (edges, "widthPixels", mesh.style.edgeWidthPixels);
            ReadFloat (edges, "angleDegrees", mesh.style.edgeAngleDegrees);
        }
    }
    return true;
}

bool ReadText (const GS::ObjectState& item, layers::Text& text, std::string& error)
{
    text.text = StringValue (item, "text");
    const bool at = item.Contains ("at"), screen = item.Contains ("screen");
    if (at == screen) {
        error = "a text is anchored at a model point (at) or on the view (screen), one of the two";
        return false;
    }
    if (at && !ReadPoint (item, "at", text.at, 3)) {
        error = "at is {x, y, z}";
        return false;
    }
    if (screen) {
        if (!ReadPoint (item, "screen", text.at, 2)) {
            error = "screen is {x, y}: fractions of the view from its top left";
            return false;
        }
        text.screen = true;
    }
    ReadPoint2 (item, "offsetPixels", text.offsetPixels);
    ReadFloat (item, "sizePixels", text.sizePixels);
    if (!ReadColour (item, "color", text.rgba, error) || !ReadColour (item, "halo", text.haloRgba, error) ||
        !ReadColour (item, "background", text.backgroundRgba, error))
        return false;
    ReadFloat (item, "haloPixels", text.haloPixels);
    ReadFloat (item, "haloScale", text.haloScale);
    if (item.Contains ("align")) {
        const std::string align = StringValue (item, "align");
        text.align = align == "left"    ? layers::Align::Left
                     : align == "right" ? layers::Align::Right
                                        : layers::Align::Center;
    }
    if (item.Contains ("baseline")) {
        const std::string baseline = StringValue (item, "baseline");
        text.baseline = baseline == "top"          ? layers::Baseline::Top
                        : baseline == "bottom"     ? layers::Baseline::Bottom
                        : baseline == "alphabetic" ? layers::Baseline::Alphabetic
                                                   : layers::Baseline::Middle;
    }
    ReadFloat (item, "rotationDegrees", text.rotationDegrees);
    if (!ReadFont (item, "font", text.font, error))
        return false;
    text.behind = OcclusionOf (item, layers::Behind::Layer);
    GS::ObjectState plane;
    if (item.Get ("plane", plane)) {
        if (screen) {
            error = "a text lies on a plane in the model (at, plane) or on the view (screen), not both";
            return false;
        }
        text.planar = true;
        if ((plane.Contains ("direction") && !ReadPoint (plane, "direction", text.direction, 3)) ||
            (plane.Contains ("normal") && !ReadPoint (plane, "normal", text.normal, 3))) {
            error = "a plane's direction and normal are {x, y, z}";
            return false;
        }
        ReadDouble (plane, "sizeMetres", text.sizeMetres);
    }
    return true;
}

bool ReadDimension (const GS::ObjectState& item, layers::Dimension& dimension, std::string& error)
{
    if (!ReadPoint (item, "from", dimension.from, 3) || !ReadPoint (item, "to", dimension.to, 3)) {
        error = "a dimension's from and to are {x, y, z} each";
        return false;
    }
    if ((item.Contains ("direction") && !ReadPoint (item, "direction", dimension.direction, 3)) ||
        (item.Contains ("normal") && !ReadPoint (item, "normal", dimension.normal, 3))) {
        error = "a dimension's direction and normal are {x, y, z}";
        return false;
    }
    ReadDouble (item, "offsetMetres", dimension.offsetMetres);
    if (item.Contains ("text"))
        dimension.text = StringValue (item, "text");
    ReadCount (item, "decimals", dimension.decimals);
    if (item.Contains ("unit")) {
        const std::string unit = StringValue (item, "unit");
        dimension.unit = unit == "cm"   ? layers::LengthUnit::Centimetres
                         : unit == "mm" ? layers::LengthUnit::Millimetres
                                        : layers::LengthUnit::Metres;
    }
    if (item.Contains ("showUnit"))
        item.Get ("showUnit", dimension.showUnit);
    if (!ReadColour (item, "color", dimension.rgba, error) ||
        !ReadColour (item, "textColor", dimension.textRgba, error) ||
        !ReadColour (item, "halo", dimension.haloRgba, error))
        return false;
    ReadFloat (item, "widthPixels", dimension.widthPixels);
    ReadFloat (item, "textSizePixels", dimension.textSizePixels);
    ReadFloat (item, "haloPixels", dimension.haloPixels);
    dimension.terminator = TerminatorOf (item, "terminator", dimension.terminator);
    ReadFloat (item, "terminatorSizePixels", dimension.terminatorSizePixels);
    if (!ReadFont (item, "font", dimension.font, error))
        return false;
    dimension.behind = OcclusionOf (item, layers::Behind::Layer);
    return true;
}

bool ReadLegend (const GS::ObjectState& item, const std::vector<layers::Mesh>& meshes, layers::Legend& legend,
                 std::string& error)
{
    if (item.Contains ("title"))
        legend.title = StringValue (item, "title");
    if (item.Contains ("unit"))
        legend.unit = StringValue (item, "unit");
    GS::ObjectState colormap;
    if (item.Contains ("mesh")) {
        // The ramp and the range of a heatmap in the same call: the legend describes it.
        GS::Int32 index = -1;
        item.Get ("mesh", index);
        if (index < 0 || size_t (index) >= meshes.size () || meshes[size_t (index)].values.empty ()) {
            error = "a legend's mesh is the index of a mesh with values in this call";
            return false;
        }
        const layers::Mesh& mesh = meshes[size_t (index)];
        legend.colormap = mesh.colormap;
        if (legend.colormap.autoRange) {
            legend.colormap.min = mesh.values.front ();
            legend.colormap.max = mesh.values.front ();
            for (const double value : mesh.values) {
                legend.colormap.min = value < legend.colormap.min ? value : legend.colormap.min;
                legend.colormap.max = value > legend.colormap.max ? value : legend.colormap.max;
            }
            if (!(legend.colormap.max > legend.colormap.min))
                legend.colormap.max = legend.colormap.min + 1.0;
            legend.colormap.autoRange = false;
        }
    }
    else if (item.Get ("colormap", colormap)) {
        if (!ReadColormap (colormap, legend.colormap, error))
            return false;
    }
    else {
        error = "a legend names its mesh, or brings its own colormap with min and max";
        return false;
    }
    if (item.Contains ("corner")) {
        const std::string corner = StringValue (item, "corner");
        legend.corner = corner == "top-left"      ? layers::Corner::TopLeft
                        : corner == "top-right"   ? layers::Corner::TopRight
                        : corner == "bottom-left" ? layers::Corner::BottomLeft
                                                  : layers::Corner::BottomRight;
    }
    ReadPoint2 (item, "offsetPixels", legend.offsetPixels);
    if (item.Contains ("screen")) {
        if (!ReadPoint2 (item, "screen", legend.screen)) {
            error = "screen is {x, y}: fractions of the view from its top left";
            return false;
        }
        legend.placed = true;
    }
    if (item.Contains ("horizontal"))
        item.Get ("horizontal", legend.horizontal);
    ReadFloat (item, "lengthPixels", legend.lengthPixels);
    ReadFloat (item, "widthPixels", legend.widthPixels);
    ReadCount (item, "ticks", legend.ticks);
    ReadCount (item, "decimals", legend.decimals);
    ReadNumbers (item, "tickValues", legend.tickValues);
    ReadStrings (item, "tickLabels", legend.tickLabels);
    ReadFloat (item, "sizePixels", legend.sizePixels);
    ReadFloat (item, "titleSizePixels", legend.titleSizePixels);
    ReadFloat (item, "paddingPixels", legend.paddingPixels);
    ReadFloat (item, "haloPixels", legend.haloPixels);
    if (!ReadFont (item, "font", legend.font, error))
        return false;
    return ReadColour (item, "color", legend.rgba, error) && ReadColour (item, "halo", legend.haloRgba, error) &&
           ReadColour (item, "background", legend.backgroundRgba, error) &&
           ReadColour (item, "barBorder", legend.barBorderRgba, error);
}

// ---- HUD panels -------------------------------------------------------------------------

bool ReadPanelItem (const GS::ObjectState& item, layers::PanelItem& out, std::string& error)
{
    const std::string kind = StringValue (item, "kind");
    out.kind = kind == "row"         ? layers::ItemKind::Row
               : kind == "separator" ? layers::ItemKind::Separator
               : kind == "spacing"   ? layers::ItemKind::Spacing
               : kind == "progress"  ? layers::ItemKind::Progress
               : kind == "swatch"    ? layers::ItemKind::Swatch
               : kind == "ramp"      ? layers::ItemKind::Ramp
               : kind == "plot"      ? layers::ItemKind::Plot
               : kind == "table"     ? layers::ItemKind::Table
                                     : layers::ItemKind::Text;
    if (item.Contains ("text"))
        out.text = StringValue (item, "text");
    if (item.Contains ("value"))
        out.value = StringValue (item, "value");
    if (item.Contains ("unit"))
        out.unit = StringValue (item, "unit");
    if (!ReadColour (item, "color", out.rgba, error))
        return false;
    ReadFloat (item, "sizePixels", out.sizePixels);
    if (item.Contains ("wrap"))
        item.Get ("wrap", out.wrap);
    ReadDouble (item, "fraction", out.fraction);
    GS::ObjectState colormap;
    if (item.Get ("colormap", colormap) && !ReadColormap (colormap, out.colormap, error))
        return false;
    if (out.kind == layers::ItemKind::Ramp && out.colormap.stops.empty () &&
        !layers::PresetStops ("viridis", out.colormap.stops)) {
        error = "the default ramp is missing";
        return false;
    }
    ReadCount (item, "ticks", out.ticks);
    ReadCount (item, "decimals", out.decimals);
    ReadNumbers (item, "tickValues", out.tickValues);
    ReadStrings (item, "tickLabels", out.tickLabels);
    ReadFloat (item, "widthPixels", out.widthPixels);
    ReadFloat (item, "heightPixels", out.heightPixels);
    ReadNumbers (item, "values", out.values);
    if (item.Contains ("min") != item.Contains ("max")) {
        error = "a plot's range is min and max together, or neither";
        return false;
    }
    if (item.Contains ("min")) {
        ReadDouble (item, "min", out.min);
        ReadDouble (item, "max", out.max);
        out.autoRange = false;
    }
    ReadStrings (item, "columns", out.columns);
    if (item.Contains ("rows")) {
        // A list of lists: each row read through its own one-field object.
        GS::Array<GS::Array<GS::UniString>> rows;
        item.Get ("rows", rows);
        for (const GS::Array<GS::UniString>& row : rows) {
            std::vector<std::string> cells;
            for (const GS::UniString& cell : row)
                cells.push_back (Utf8Of (cell));
            out.rows.push_back (std::move (cells));
        }
    }
    return true;
}

bool ReadPanel (const GS::ObjectState& item, layers::Panel& panel, std::string& error)
{
    if (item.Contains ("title"))
        panel.title = StringValue (item, "title");
    if (item.Contains ("anchor")) {
        const std::string anchor = StringValue (item, "anchor");
        const char* const names[] = { "top-left", "top",         "top-right", "left",        "center",
                                      "right",    "bottom-left", "bottom",    "bottom-right" };
        for (size_t i = 0; i < 9; ++i)
            if (anchor == names[i])
                panel.anchor = layers::PanelAnchor (i);
    }
    ReadPoint2 (item, "offsetPixels", panel.offsetPixels);
    ReadFloat (item, "widthPixels", panel.widthPixels);
    ReadFloat (item, "sizePixels", panel.sizePixels);
    ReadFloat (item, "roundingPixels", panel.roundingPixels);
    ReadFloat (item, "paddingPixels", panel.paddingPixels);
    if (!ReadFont (item, "font", panel.font, error))
        return false;
    if (!ReadColour (item, "color", panel.textRgba, error) ||
        !ReadColour (item, "background", panel.backgroundRgba, error) ||
        !ReadColour (item, "border", panel.borderRgba, error))
        return false;
    GS::Array<GS::ObjectState> items;
    if (item.Get ("items", items)) {
        for (const GS::ObjectState& entry : items) {
            layers::PanelItem value;
            if (!ReadPanelItem (entry, value, error)) {
                error = "item " + std::to_string (panel.items.size ()) + ": " + error;
                return false;
            }
            panel.items.push_back (std::move (value));
        }
    }
    return true;
}

} // namespace

// The shared #Color: "RRGGBB" or "RRGGBBAA", an optional leading '#' -- what a script
// writes by hand and what a log line can be compared against by eye. Six digits are
// opaque.
bool ReadColour (const GS::ObjectState& item, const char* key, uint32_t& rgba, std::string& error)
{
    if (!item.Contains (key))
        return true;
    const std::string given = StringValue (item, key);
    const std::string hex = !given.empty () && given[0] == '#' ? given.substr (1) : given;
    uint32_t parsed = 0;
    bool valid = hex.size () == 6 || hex.size () == 8;
    for (size_t i = 0; valid && i < hex.size (); ++i) {
        const char c = hex[i];
        const int digit = (c >= '0' && c <= '9')   ? c - '0'
                          : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                          : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                                                   : -1;
        valid = digit >= 0;
        parsed = (parsed << 4) | uint32_t (digit < 0 ? 0 : digit);
    }
    if (!valid) {
        error = std::string (key) + " is a colour \"RRGGBB\" or \"RRGGBBAA\", got \"" + given + "\"";
        return false;
    }
    rgba = hex.size () == 6 ? (parsed << 8) | 0xFFu : parsed;
    return true;
}

// `occlusion`, the vocabulary SetDiligentTextLabels already speaks: "always", "hide",
// "fade", and for lines "dash".
layers::Behind OcclusionOf (const GS::ObjectState& item, layers::Behind fallback)
{
    if (!item.Contains ("occlusion"))
        return fallback;
    const std::string which = StringValue (item, "occlusion");
    return which == "hide"   ? layers::Behind::Hide
           : which == "fade" ? layers::Behind::Fade
           : which == "dash" ? layers::Behind::Dash
                             : layers::Behind::Show;
}

std::string StringOf (const GS::ObjectState& item, const char* key)
{
    return StringValue (item, key);
}

void ReadDash (const GS::ObjectState& item, const char* key, std::vector<float>& lengths)
{
    std::vector<double> values;
    if (!ReadReals (item, key, values))
        return;
    lengths.clear ();
    for (const double value : values)
        lengths.push_back (float (value));
}

bool ReadHiddenLine (const GS::ObjectState& item, layers::HiddenLine& hidden, std::string& error)
{
    if (!ReadColour (item, "color", hidden.rgba, error))
        return false;
    ReadFloat (item, "widthPixels", hidden.widthPixels);
    ReadDash (item, "dashMetres", hidden.dashMetres);
    return true;
}

bool ReadFont (const GS::ObjectState& item, const char* key, std::string& path, std::string& error)
{
    if (!item.Contains (key))
        return true;
    if (!archviz::overlayfonts::Resolve (StringValue (item, key), path, error)) {
        error = std::string (key) + ": " + error;
        return false;
    }
    return true;
}

bool ReadLayer (const GS::ObjectState& params, layers::Layer& layer, std::string& error)
{
    layer.name = StringValue (params, "layer");
    if (layers::Reserved (layer.name)) {
        error = std::string ("names starting \"") + layers::kReservedPrefix +
                "\" are the add-on's own layers, switched by their own verbs";
        return false;
    }
    if (params.Contains ("views")) {
        const std::string which = StringValue (params, "views");
        layer.views = which == "2d" ? layers::Views::TwoD : which == "3d" ? layers::Views::ThreeD : layers::Views::Both;
    }
    const layers::Behind occlusion = OcclusionOf (params, layers::Behind::Layer);
    layer.occlusion = occlusion == layers::Behind::Layer ? layers::Behind::Hide : occlusion;
    // The layer's font is every text-bearing item's that names none of its own.
    std::string font;
    if (!ReadFont (params, "font", font, error))
        return false;

    GS::Array<GS::ObjectState> items;
    if (params.Get ("polylines", items)) {
        for (const GS::ObjectState& item : items) {
            layers::Polyline polyline;
            ReadNumbers (item, "points", polyline.points);
            if (item.Contains ("closed"))
                item.Get ("closed", polyline.closed);
            if (!ReadColour (item, "color", polyline.rgba, error))
                return false;
            ReadFloat (item, "widthPixels", polyline.widthPixels);
            ReadDash (item, "dashMetres", polyline.dashMetres);
            GS::ObjectState hidden;
            if (item.Get ("hidden", hidden) && !ReadHiddenLine (hidden, polyline.hidden, error))
                return false;
            polyline.startArrow = TerminatorOf (item, "startArrow", polyline.startArrow);
            polyline.endArrow = TerminatorOf (item, "endArrow", polyline.endArrow);
            ReadFloat (item, "arrowSizePixels", polyline.arrowSizePixels);
            polyline.behind = OcclusionOf (item, layers::Behind::Layer);
            layer.polylines.push_back (std::move (polyline));
        }
    }
    items.Clear ();
    if (params.Get ("points", items)) {
        for (const GS::ObjectState& item : items) {
            layers::PointSet set;
            ReadNumbers (item, "points", set.points);
            if (!ReadColour (item, "color", set.rgba, error))
                return false;
            ReadFloat (item, "sizePixels", set.sizePixels);
            ReadFloat (item, "sizeMetres", set.sizeMetres);
            layer.points.push_back (std::move (set));
        }
    }
    items.Clear ();
    if (params.Get ("meshes", items)) {
        for (const GS::ObjectState& item : items) {
            layers::Mesh mesh;
            if (!ReadMesh (item, mesh, error))
                return false;
            layer.meshes.push_back (std::move (mesh));
        }
    }
    items.Clear ();
    if (params.Get ("texts", items)) {
        for (const GS::ObjectState& item : items) {
            layers::Text text;
            if (!ReadText (item, text, error)) {
                error = "text " + std::to_string (layer.texts.size ()) + ": " + error;
                return false;
            }
            layer.texts.push_back (std::move (text));
        }
    }
    items.Clear ();
    if (params.Get ("dimensions", items)) {
        for (const GS::ObjectState& item : items) {
            layers::Dimension dimension;
            if (!ReadDimension (item, dimension, error)) {
                error = "dimension " + std::to_string (layer.dimensions.size ()) + ": " + error;
                return false;
            }
            layer.dimensions.push_back (std::move (dimension));
        }
    }
    items.Clear ();
    if (params.Get ("legends", items)) {
        for (const GS::ObjectState& item : items) {
            layers::Legend legend;
            if (!ReadLegend (item, layer.meshes, legend, error)) {
                error = "legend " + std::to_string (layer.legends.size ()) + ": " + error;
                return false;
            }
            layer.legends.push_back (std::move (legend));
        }
    }
    items.Clear ();
    if (params.Get ("panels", items)) {
        for (const GS::ObjectState& item : items) {
            layers::Panel panel;
            if (!ReadPanel (item, panel, error)) {
                error = "panel " + std::to_string (layer.panels.size ()) + ": " + error;
                return false;
            }
            layer.panels.push_back (std::move (panel));
        }
    }
    if (!font.empty ()) {
        for (layers::Text& text : layer.texts)
            text.font = text.font.empty () ? font : text.font;
        for (layers::Dimension& dimension : layer.dimensions)
            dimension.font = dimension.font.empty () ? font : dimension.font;
        for (layers::Legend& legend : layer.legends)
            legend.font = legend.font.empty () ? font : legend.font;
        for (layers::Panel& panel : layer.panels)
            panel.font = panel.font.empty () ? font : panel.font;
    }
    error = layers::Validate (layer);
    return error.empty ();
}

} // namespace overlayreading
} // namespace geomsrv
