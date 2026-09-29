// NativeCommands/OverlayLayerCommands -- the caller's own content on the overlays.
// See the header.
//
//   Tapioca.SetOverlayLayer {layer, views?, occluded?, polylines?, points?, meshes?,
//                            texts?, dimensions?, legends?}
//       add or replace the named layer; coordinates are MODEL METRES, x, y, z flat
//   Tapioca.ClearOverlayLayer {layer} | {all: true}
//   Tapioca.OverlayLayers {}          what is set, in draw order
//
// ⚠️ A LAYER IS GEOMETRY AND STYLE ONLY. Each overlay projects it through its own
// transform (OverlayLayers.hpp): the 2D overlay drops z and reads the plan's
// transform at its Present, the 3D overlay reads Archicad's camera at the model's draw.
//
// ⚠️ WHAT THE SCHEMA CANNOT SAY, THE HANDLER CHECKS: colours are 8 hex digits (the
// validator has no `pattern`), a text is anchored `at` a model point or on the
// `screen`, never both, and everything the store's `Validate` refuses is refused
// here with its sentence, before anything is set.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/OverlayLayerCommands.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/CommandRegistration.hpp"
#include "NativeCommands/CommandUtils.hpp" // ReadReal/ReadReals: a JSON whole number is a number too

#include "ArchViz/OverlayController.hpp"
#include "ArchViz/OverlayLayers.hpp"

#include <string>
#include <vector>

namespace geomsrv {

namespace {

namespace control = archviz::overlaycontrol;
namespace layers = archviz::overlaylayers;

GS::UniString Utf8 (const std::string& text)
{
    return GS::UniString (text.c_str (), CC_UTF8);
}

GS::UniString Text (uint64_t value)
{
    return GS::UniString (std::to_string (value).c_str (), CC_UTF8);
}

std::string Utf8Of (const GS::UniString& text)
{
    return std::string (text.ToCStr (0, MaxUSize, CC_UTF8).Get ());
}

std::string StringOf (const GS::ObjectState& item, const char* key)
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
    const std::string hex = StringOf (item, key);
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
    const std::string which = StringOf (item, "behind");
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
        const std::string preset = item.Contains ("preset") ? StringOf (item, "preset") : std::string ("viridis");
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
        const std::string shading = style.Contains ("shading") ? StringOf (style, "shading") : std::string ("flat");
        mesh.style.shading = shading == "lit"     ? layers::Shading::Lit
                             : shading == "ghost" ? layers::Shading::Ghost
                             : shading == "xray"  ? layers::Shading::Xray
                                                  : layers::Shading::Flat;
        ReadFloat (style, "opacity", mesh.style.opacity);
        mesh.style.behind = BehindOf (style);
        mesh.style.cullBack = style.Contains ("cull") && StringOf (style, "cull") == "back";
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
    text.text = StringOf (item, "text");
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
        const std::string align = StringOf (item, "align");
        text.align = align == "left"    ? layers::Align::Left
                     : align == "right" ? layers::Align::Right
                                        : layers::Align::Center;
    }
    if (item.Contains ("baseline")) {
        const std::string baseline = StringOf (item, "baseline");
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
        dimension.text = StringOf (item, "text");
    ReadCount (item, "decimals", dimension.decimals);
    if (item.Contains ("unit")) {
        const std::string unit = StringOf (item, "unit");
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
        const std::string terminator = StringOf (item, "terminator");
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
        legend.title = StringOf (item, "title");
    if (item.Contains ("unit"))
        legend.unit = StringOf (item, "unit");
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
        const std::string corner = StringOf (item, "corner");
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

bool ReadLayer (const GS::ObjectState& params, layers::Layer& layer, std::string& error)
{
    layer.name = StringOf (params, "layer");
    if (layers::Reserved (layer.name)) {
        error = std::string ("names starting \"") + layers::kReservedPrefix +
                "\" are the add-on's own layers, switched by their own verbs";
        return false;
    }
    if (params.Contains ("views")) {
        const std::string which = StringOf (params, "views");
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

const char* ViewsName (layers::Views views)
{
    return views == layers::Views::TwoD ? "2d" : views == layers::Views::ThreeD ? "3d" : "both";
}

GS::ObjectState SummaryRecord (const layers::Summary& summary)
{
    GS::ObjectState os;
    os.Add ("layer", Utf8 (summary.name));
    os.Add ("views", Utf8 (ViewsName (summary.views)));
    os.Add ("occluded", summary.occluded);
    os.Add ("polylines", (GS::Int32) summary.polylines);
    os.Add ("lineVertices", (GS::Int32) summary.lineVertices);
    os.Add ("points", (GS::Int32) summary.points);
    os.Add ("meshes", (GS::Int32) summary.meshes);
    os.Add ("triangles", (GS::Int32) summary.triangles);
    os.Add ("texts", (GS::Int32) summary.texts);
    os.Add ("dimensions", (GS::Int32) summary.dimensions);
    os.Add ("legends", (GS::Int32) summary.legends);
    return os;
}

class SetOverlayLayerCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "SetOverlayLayer";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        layers::Layer layer;
        std::string error;
        if (!ReadLayer (params, layer, error))
            return NativeCommandResult::Failure (EVP_FAIL (Utf8 (error), "setting an overlay layer"));
        const layers::Summary summary = layers::Summarise (layer);
        const uint64_t generation = layers::Set (std::move (layer));
        control::PublishLayers ();
        GS::ObjectState os = SummaryRecord (summary);
        os.Add ("generation", Text (generation));
        return os;
    }
};

class ClearOverlayLayerCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "ClearOverlayLayer";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        bool all = false;
        if (params.Contains ("all"))
            params.Get ("all", all);
        GS::Int32 cleared = 0;
        if (all) {
            for (const auto& layer : layers::Layers ())
                cleared += layers::Reserved (layer->name) ? 0 : 1;
            layers::ClearAll ();
        }
        else if (params.Contains ("layer")) {
            const std::string name = StringOf (params, "layer");
            if (layers::Reserved (name))
                return NativeCommandResult::Failure (
                    EVP_FAIL ("the add-on's own layers are switched by their own verbs", "clearing an overlay layer"));
            cleared = layers::Clear (name) ? 1 : 0;
        }
        else {
            return NativeCommandResult::Failure (
                EVP_FAIL ("name the layer to clear, or pass all: true", "clearing an overlay layer"));
        }
        if (cleared > 0)
            control::PublishLayers ();
        GS::ObjectState os;
        os.Add ("cleared", cleared);
        os.Add ("layers", (GS::Int32) layers::Layers ().size ());
        os.Add ("generation", Text (layers::Generation ()));
        return os;
    }
};

GS::ObjectState GuestRecord (const archviz::overlaycontrol::GuestCounts& counts)
{
    GS::ObjectState os;
    os.Add ("attached", counts.attached);
    os.Add ("attachMilliseconds", (GS::Int32) counts.attachMilliseconds);
    os.Add ("buildMilliseconds", (GS::Int32) counts.buildMilliseconds);
    os.Add ("uploads", Text (counts.uploads));
    os.Add ("draws", Text (counts.draws));
    os.Add ("drawCalls", Text (counts.drawCalls));
    os.Add ("declinedNoCamera", Text (counts.declinedNoCamera));
    os.Add ("declinedNoViewport", Text (counts.declinedNoViewport));
    os.Add ("declinedNoTransform", Text (counts.declinedNoTransform));
    os.Add ("declinedFailed", Text (counts.declinedFailed));
    os.Add ("fills", (GS::Int32) counts.fills);
    os.Add ("lines", (GS::Int32) counts.lines);
    os.Add ("glyphVertices", (GS::Int32) counts.glyphVertices);
    os.Add ("pages", (GS::Int32) counts.pages);
    os.Add ("failure", GS::UniString (counts.failure.c_str (), CC_UTF8));
    return os;
}

class OverlayLayersCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "OverlayLayers";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        GS::Array<GS::ObjectState> records;
        for (const auto& layer : layers::Layers ())
            records.Push (SummaryRecord (layers::Summarise (*layer)));
        GS::ObjectState os;
        os.Add ("layers", records);
        os.Add ("generation", Text (layers::Generation ()));
        // What the Diligent guest has drawn of them, per overlay: totals, so a
        // caller asking whether it draws NOW compares two readings.
        const archviz::overlaycontrol::GuestReport guest = archviz::overlaycontrol::Guest ();
        GS::ObjectState guestRecord;
        guestRecord.Add ("plan", GuestRecord (guest.plan));
        guestRecord.Add ("scene", GuestRecord (guest.scene));
        os.Add ("guest", guestRecord);
        return os;
    }
};

// clang-format off
// Colours are 8 hex digits RRGGBBAA, checked by the handler (the validator has no
// pattern keyword). Coordinates are model metres, flat x, y, z. A colour ramp is a
// preset or stops, with an optional range, bands and isolines -- written out twice
// below, for meshes and for legends, because a schema constant is one literal.
constexpr const char kSetOverlayLayerInput[] = R"json({"type":"object","properties":{
    "layer":{"type":"string","minLength":1,"maxLength":64},
    "views":{"type":"string","enum":["2d","3d","both"]},
    "occluded":{"type":"boolean"},
    "polylines":{"type":"array","maxItems":20000,"items":{"type":"object","properties":{
        "points":{"type":"array","minItems":6,"maxItems":300000,"items":{"type":"number"}},
        "closed":{"type":"boolean"},
        "color":{"type":"string","minLength":8,"maxLength":8},
        "widthPixels":{"type":"number","exclusiveMinimum":0,"maximum":64},
        "dashPixels":{"type":"number","minimum":0,"maximum":512},
        "dashDuty":{"type":"number","minimum":0.05,"maximum":0.95},
        "behind":{"type":"string","enum":["hide","fade","dash","show"]}},
      "additionalProperties":false,"required":["points"]}},
    "points":{"type":"array","maxItems":1000,"items":{"type":"object","properties":{
        "points":{"type":"array","minItems":3,"maxItems":300000,"items":{"type":"number"}},
        "color":{"type":"string","minLength":8,"maxLength":8},
        "sizePixels":{"type":"number","exclusiveMinimum":0,"maximum":256},
        "sizeMetres":{"type":"number","exclusiveMinimum":0,"maximum":1000}},
      "additionalProperties":false,"required":["points"]}},
    "meshes":{"type":"array","maxItems":1000,"items":{"type":"object","properties":{
        "points":{"type":"array","minItems":9,"maxItems":300000,"items":{"type":"number"}},
        "indices":{"type":"array","minItems":3,"maxItems":600000,"items":{"type":"integer","minimum":0}},
        "color":{"type":"string","minLength":8,"maxLength":8},
        "vertexColors":{"type":"array","maxItems":100000,"items":{"type":"string","minLength":8,"maxLength":8}},
        "normals":{"type":"array","minItems":9,"maxItems":300000,"items":{"type":"number"}},
        "values":{"type":"array","minItems":3,"maxItems":100000,"items":{"type":"number"}},
        "colormap":{"type":"object","properties":{
            "preset":{"type":"string","enum":["viridis","inferno","magma","plasma","turbo","coolwarm","greys","sunhours","slope","clearance"]},
            "stops":{"type":"array","minItems":2,"maxItems":16,"items":{"type":"object","properties":{
                "at":{"type":"number","minimum":0,"maximum":1},"color":{"type":"string","minLength":8,"maxLength":8}},
              "additionalProperties":false,"required":["at","color"]}},
            "min":{"type":"number"},"max":{"type":"number"},
            "bands":{"type":"integer","minimum":0,"maximum":64},
            "isolines":{"type":"object","properties":{
                "step":{"type":"number","exclusiveMinimum":0},"color":{"type":"string","minLength":8,"maxLength":8},
                "widthPixels":{"type":"number","minimum":0.25,"maximum":16}},
              "additionalProperties":false,"required":["step"]}},
          "additionalProperties":false},
        "style":{"type":"object","properties":{
            "shading":{"type":"string","enum":["flat","lit","ghost","xray"]},
            "opacity":{"type":"number","minimum":0,"maximum":1},
            "behind":{"type":"string","enum":["hide","fade","show"]},
            "cull":{"type":"string","enum":["none","back"]},
            "edges":{"type":"object","properties":{
                "color":{"type":"string","minLength":8,"maxLength":8},
                "widthPixels":{"type":"number","minimum":0.25,"maximum":16},
                "angleDegrees":{"type":"number","minimum":0,"maximum":180}},
              "additionalProperties":false}},
          "additionalProperties":false}},
      "additionalProperties":false,"required":["points","indices"]}},
    "texts":{"type":"array","maxItems":5000,"items":{"type":"object","properties":{
        "text":{"type":"string","minLength":1,"maxLength":512},
        "at":{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number"}},
        "screen":{"type":"array","minItems":2,"maxItems":2,"items":{"type":"number"}},
        "offsetPixels":{"type":"array","minItems":2,"maxItems":2,"items":{"type":"number"}},
        "sizePixels":{"type":"number","minimum":4,"maximum":256},
        "color":{"type":"string","minLength":8,"maxLength":8},
        "halo":{"type":"string","minLength":8,"maxLength":8},
        "haloPixels":{"type":"number","minimum":0,"maximum":8},
        "background":{"type":"string","minLength":8,"maxLength":8},
        "align":{"type":"string","enum":["left","center","right"]},
        "baseline":{"type":"string","enum":["top","middle","bottom","alphabetic"]},
        "rotationDegrees":{"type":"number","minimum":-360,"maximum":360},
        "behind":{"type":"string","enum":["hide","fade","show"]}},
      "additionalProperties":false,"required":["text"]}},
    "dimensions":{"type":"array","maxItems":5000,"items":{"type":"object","properties":{
        "from":{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number"}},
        "to":{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number"}},
        "offsetMetres":{"type":"number","minimum":-10000,"maximum":10000},
        "direction":{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number"}},
        "normal":{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number"}},
        "text":{"type":"string","maxLength":512},
        "decimals":{"type":"integer","minimum":0,"maximum":6},
        "unit":{"type":"string","enum":["m","cm","mm"]},
        "showUnit":{"type":"boolean"},
        "color":{"type":"string","minLength":8,"maxLength":8},
        "widthPixels":{"type":"number","exclusiveMinimum":0,"maximum":16},
        "textSizePixels":{"type":"number","minimum":4,"maximum":128},
        "terminator":{"type":"string","enum":["tick","arrow","dot"]},
        "behind":{"type":"string","enum":["hide","fade","dash","show"]}},
      "additionalProperties":false,"required":["from","to"]}},)json"
    R"json("legends":{"type":"array","maxItems":8,"items":{"type":"object","properties":{
        "title":{"type":"string","maxLength":512},
        "unit":{"type":"string","maxLength":64},
        "mesh":{"type":"integer","minimum":0},
        "colormap":{"type":"object","properties":{
            "preset":{"type":"string","enum":["viridis","inferno","magma","plasma","turbo","coolwarm","greys","sunhours","slope","clearance"]},
            "stops":{"type":"array","minItems":2,"maxItems":16,"items":{"type":"object","properties":{
                "at":{"type":"number","minimum":0,"maximum":1},"color":{"type":"string","minLength":8,"maxLength":8}},
              "additionalProperties":false,"required":["at","color"]}},
            "min":{"type":"number"},"max":{"type":"number"},
            "bands":{"type":"integer","minimum":0,"maximum":64},
            "isolines":{"type":"object","properties":{
                "step":{"type":"number","exclusiveMinimum":0},"color":{"type":"string","minLength":8,"maxLength":8},
                "widthPixels":{"type":"number","minimum":0.25,"maximum":16}},
              "additionalProperties":false,"required":["step"]}},
          "additionalProperties":false},
        "corner":{"type":"string","enum":["top-left","top-right","bottom-left","bottom-right"]},
        "offsetPixels":{"type":"array","minItems":2,"maxItems":2,"items":{"type":"number"}},
        "lengthPixels":{"type":"number","minimum":20,"maximum":4000},
        "widthPixels":{"type":"number","minimum":2,"maximum":200},
        "ticks":{"type":"integer","minimum":2,"maximum":32},
        "decimals":{"type":"integer","minimum":0,"maximum":6},
        "sizePixels":{"type":"number","minimum":4,"maximum":128},
        "color":{"type":"string","minLength":8,"maxLength":8},
        "halo":{"type":"string","minLength":8,"maxLength":8}},
      "additionalProperties":false}}},
  "additionalProperties":false,"required":["layer"]})json";

constexpr const char kSetOverlayLayerOutput[] = R"json({"type":"object","properties":{
    "layer":{"type":"string"},"views":{"type":"string","enum":["2d","3d","both"]},"occluded":{"type":"boolean"},
    "polylines":{"type":"integer","minimum":0},"lineVertices":{"type":"integer","minimum":0},
    "points":{"type":"integer","minimum":0},"meshes":{"type":"integer","minimum":0},
    "triangles":{"type":"integer","minimum":0},"texts":{"type":"integer","minimum":0},
    "dimensions":{"type":"integer","minimum":0},"legends":{"type":"integer","minimum":0},
    "generation":{"type":"string"}},
  "additionalProperties":false,
  "required":["layer","views","occluded","polylines","lineVertices","points","meshes","triangles","texts",
              "dimensions","legends","generation"]})json";

constexpr const char kClearOverlayLayerInput[] = R"json({"type":"object","properties":{
    "layer":{"type":"string","minLength":1,"maxLength":64},"all":{"type":"boolean"}},
  "additionalProperties":false})json";

constexpr const char kClearOverlayLayerOutput[] = R"json({"type":"object","properties":{
    "cleared":{"type":"integer","minimum":0},"layers":{"type":"integer","minimum":0},"generation":{"type":"string"}},
  "additionalProperties":false,"required":["cleared","layers","generation"]})json";

constexpr const char kOverlayLayersInput[] = R"json({"type":"object","properties":{},"additionalProperties":false})json";

constexpr const char kOverlayLayersOutput[] = R"json({"type":"object","properties":{
    "layers":{"type":"array","items":{"type":"object","properties":{
        "layer":{"type":"string"},"views":{"type":"string","enum":["2d","3d","both"]},"occluded":{"type":"boolean"},
        "polylines":{"type":"integer","minimum":0},"lineVertices":{"type":"integer","minimum":0},
        "points":{"type":"integer","minimum":0},"meshes":{"type":"integer","minimum":0},
        "triangles":{"type":"integer","minimum":0},"texts":{"type":"integer","minimum":0},
        "dimensions":{"type":"integer","minimum":0},"legends":{"type":"integer","minimum":0}},
      "additionalProperties":false,
      "required":["layer","views","occluded","polylines","lineVertices","points","meshes","triangles","texts",
                  "dimensions","legends"]}},
    "generation":{"type":"string"},)json"
    R"json("guest":{"type":"object","properties":{
        "plan":{"type":"object","properties":{
            "attached":{"type":"boolean"},"attachMilliseconds":{"type":"integer","minimum":0},
            "buildMilliseconds":{"type":"integer","minimum":0},"uploads":{"type":"string"},"draws":{"type":"string"},
            "drawCalls":{"type":"string"},"declinedNoCamera":{"type":"string"},"declinedNoViewport":{"type":"string"},
            "declinedNoTransform":{"type":"string"},"declinedFailed":{"type":"string"},
            "fills":{"type":"integer","minimum":0},"lines":{"type":"integer","minimum":0},
            "glyphVertices":{"type":"integer","minimum":0},"pages":{"type":"integer","minimum":0},
            "failure":{"type":"string"}},
          "additionalProperties":false,
          "required":["attached","attachMilliseconds","buildMilliseconds","uploads","draws","drawCalls",
                      "declinedNoCamera","declinedNoViewport","declinedNoTransform","declinedFailed","fills","lines",
                      "glyphVertices","pages","failure"]},)json"
    R"json("scene":{"type":"object","properties":{
            "attached":{"type":"boolean"},"attachMilliseconds":{"type":"integer","minimum":0},
            "buildMilliseconds":{"type":"integer","minimum":0},"uploads":{"type":"string"},"draws":{"type":"string"},
            "drawCalls":{"type":"string"},"declinedNoCamera":{"type":"string"},"declinedNoViewport":{"type":"string"},
            "declinedNoTransform":{"type":"string"},"declinedFailed":{"type":"string"},
            "fills":{"type":"integer","minimum":0},"lines":{"type":"integer","minimum":0},
            "glyphVertices":{"type":"integer","minimum":0},"pages":{"type":"integer","minimum":0},
            "failure":{"type":"string"}},
          "additionalProperties":false,
          "required":["attached","attachMilliseconds","buildMilliseconds","uploads","draws","drawCalls",
                      "declinedNoCamera","declinedNoViewport","declinedNoTransform","declinedFailed","fills","lines",
                      "glyphVertices","pages","failure"]}},
      "additionalProperties":false,"required":["plan","scene"]}},
  "additionalProperties":false,"required":["layers","generation","guest"]})json";

const NativeCommandRegistration kOverlayLayerCommandRegistrations[] = {
    { "SetOverlayLayer", &MakeRegisteredNativeCommand<SetOverlayLayerCommand>, false, kSetOverlayLayerInput,
      kSetOverlayLayerOutput },
    { "ClearOverlayLayer", &MakeRegisteredNativeCommand<ClearOverlayLayerCommand>, false, kClearOverlayLayerInput,
      kClearOverlayLayerOutput },
    { "OverlayLayers", &MakeRegisteredNativeCommand<OverlayLayersCommand>, false, kOverlayLayersInput,
      kOverlayLayersOutput },
};
// clang-format on

} // namespace

NativeCommandRegistrations GetOverlayLayerCommandRegistrations ()
{
    return MakeRegistrationView (kOverlayLayerCommandRegistrations);
}

} // namespace geomsrv
