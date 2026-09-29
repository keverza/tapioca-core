// NativeCommands/OverlayLayerReading -- Tapioca.SetOverlayLayer's parameters read into a
// layer (ArchViz/OverlayLayers.hpp). See the header.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/OverlayLayerReading.hpp"
#include "NativeCommands/CommandUtils.hpp" // ReadReal/ReadReals: a JSON whole number is a number too

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

// "RRGGBBAA", as SetPlanAnchors takes it: what a script writes by hand and what a
// log line can be compared against by eye.
bool ReadColour (const GS::ObjectState& item, const char* key, uint32_t& rgba, std::string& error)
{
    if (!item.Contains (key))
        return true;
    const std::string hex = StringValue (item, key);
    uint32_t parsed = 0;
    bool valid = hex.size () == 8;
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
        error = std::string (key) + " must be 8 hex digits RRGGBBAA, got \"" + hex + "\"";
        return false;
    }
    rgba = parsed;
    return true;
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

template <size_t N> bool ReadFixed (const GS::ObjectState& item, const char* key, double (&out)[N])
{
    std::vector<double> values;
    ReadNumbers (item, key, values);
    if (values.size () != N)
        return false;
    for (size_t i = 0; i < N; ++i)
        out[i] = values[i];
    return true;
}

layers::Behind BehindOf (const GS::ObjectState& item)
{
    if (!item.Contains ("behind"))
        return layers::Behind::Layer;
    const std::string which = StringValue (item, "behind");
    return which == "hide"   ? layers::Behind::Hide
           : which == "fade" ? layers::Behind::Fade
           : which == "dash" ? layers::Behind::Dash
                             : layers::Behind::Show;
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
        mesh.style.behind = BehindOf (style);
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
    if (at && !ReadFixed (item, "at", text.at)) {
        error = "at is x, y, z";
        return false;
    }
    if (screen) {
        double fraction[2] = {};
        if (!ReadFixed (item, "screen", fraction)) {
            error = "screen is x, y: fractions of the view from its top left";
            return false;
        }
        text.screen = true;
        text.at[0] = fraction[0];
        text.at[1] = fraction[1];
    }
    double offset[2] = {};
    if (item.Contains ("offsetPixels") && ReadFixed (item, "offsetPixels", offset)) {
        text.offsetPixels[0] = float (offset[0]);
        text.offsetPixels[1] = float (offset[1]);
    }
    ReadFloat (item, "sizePixels", text.sizePixels);
    if (!ReadColour (item, "color", text.rgba, error) || !ReadColour (item, "halo", text.haloRgba, error) ||
        !ReadColour (item, "background", text.backgroundRgba, error))
        return false;
    ReadFloat (item, "haloPixels", text.haloPixels);
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
    text.behind = BehindOf (item);
    return true;
}

bool ReadDimension (const GS::ObjectState& item, layers::Dimension& dimension, std::string& error)
{
    if (!ReadFixed (item, "from", dimension.from) || !ReadFixed (item, "to", dimension.to)) {
        error = "a dimension's from and to are x, y, z each";
        return false;
    }
    if (item.Contains ("direction") && !ReadFixed (item, "direction", dimension.direction)) {
        error = "direction is x, y, z";
        return false;
    }
    if (item.Contains ("normal") && !ReadFixed (item, "normal", dimension.normal)) {
        error = "normal is x, y, z";
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
    if (!ReadColour (item, "color", dimension.rgba, error))
        return false;
    ReadFloat (item, "widthPixels", dimension.widthPixels);
    ReadFloat (item, "textSizePixels", dimension.textSizePixels);
    if (item.Contains ("terminator")) {
        const std::string terminator = StringValue (item, "terminator");
        dimension.terminator = terminator == "arrow" ? layers::Terminator::Arrow
                               : terminator == "dot" ? layers::Terminator::Dot
                                                     : layers::Terminator::Tick;
    }
    dimension.behind = BehindOf (item);
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
    double offset[2] = {};
    if (item.Contains ("offsetPixels") && ReadFixed (item, "offsetPixels", offset)) {
        legend.offsetPixels[0] = float (offset[0]);
        legend.offsetPixels[1] = float (offset[1]);
    }
    ReadFloat (item, "lengthPixels", legend.lengthPixels);
    ReadFloat (item, "widthPixels", legend.widthPixels);
    ReadCount (item, "ticks", legend.ticks);
    ReadCount (item, "decimals", legend.decimals);
    ReadFloat (item, "sizePixels", legend.sizePixels);
    return ReadColour (item, "color", legend.rgba, error) && ReadColour (item, "halo", legend.haloRgba, error);
}

} // namespace

std::string StringOf (const GS::ObjectState& item, const char* key)
{
    return StringValue (item, key);
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
    if (params.Contains ("occluded"))
        params.Get ("occluded", layer.occluded);

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
            ReadFloat (item, "dashPixels", polyline.dashPixels);
            ReadFloat (item, "dashDuty", polyline.dashDuty);
            polyline.behind = BehindOf (item);
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
    error = layers::Validate (layer);
    return error.empty ();
}

} // namespace overlayreading
} // namespace geomsrv
