// NativeCommands/OverlayLayerCommands -- the caller's own geometry on the overlays.
// See the header.
//
//   Tapioca.SetOverlayLayer {layer, views?, occluded?, polylines?, points?, meshes?}
//       add or replace the named layer; coordinates are MODEL METRES, x, y, z flat
//   Tapioca.ClearOverlayLayer {layer} | {all: true}
//   Tapioca.OverlayLayers {}          what is set, in draw order
//
// ⚠️ A LAYER IS GEOMETRY AND STYLE ONLY. Each overlay projects it through its own
// transform (OverlayLayers.hpp): the 2D overlay drops z and reads the plan's
// transform at its Present, the 3D overlay reads Archicad's camera at the model's draw.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/OverlayLayerCommands.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/CommandRegistration.hpp"

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

// "RRGGBBAA", as SetPlanAnchors takes it: what a script writes by hand and what a
// log line can be compared against by eye.
bool ReadColour (const GS::ObjectState& item, const char* key, uint32_t& rgba, std::string& error)
{
    if (!item.Contains (key))
        return true;
    GS::UniString text;
    item.Get (key, text);
    const std::string hex = Utf8Of (text);
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
    GS::Array<double> values;
    if (item.Get (key, values))
        for (const double value : values)
            out.push_back (value);
}

void ReadFloat (const GS::ObjectState& item, const char* key, float& out)
{
    if (item.Contains (key)) {
        double value = 0.0;
        item.Get (key, value);
        out = float (value);
    }
}

bool ReadLayer (const GS::ObjectState& params, layers::Layer& layer, std::string& error)
{
    GS::UniString name;
    params.Get ("layer", name);
    layer.name = Utf8Of (name);
    if (params.Contains ("views")) {
        GS::UniString views;
        params.Get ("views", views);
        const std::string which = Utf8Of (views);
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
            layer.meshes.push_back (std::move (mesh));
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
            cleared = (GS::Int32) layers::Layers ().size ();
            layers::ClearAll ();
        }
        else if (params.Contains ("layer")) {
            GS::UniString name;
            params.Get ("layer", name);
            cleared = layers::Clear (Utf8Of (name)) ? 1 : 0;
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
        return os;
    }
};

// clang-format off
// Colours are 8 hex digits RRGGBBAA, checked by the handler (the validator has no
// pattern keyword). Coordinates are model metres, flat x, y, z.
constexpr const char kSetOverlayLayerInput[] = R"json({"type":"object","properties":{
    "layer":{"type":"string","minLength":1,"maxLength":64},
    "views":{"type":"string","enum":["2d","3d","both"]},
    "occluded":{"type":"boolean"},
    "polylines":{"type":"array","maxItems":20000,"items":{"type":"object","properties":{
        "points":{"type":"array","minItems":6,"maxItems":300000,"items":{"type":"number"}},
        "closed":{"type":"boolean"},
        "color":{"type":"string","minLength":8,"maxLength":8},
        "widthPixels":{"type":"number","exclusiveMinimum":0,"maximum":64}},
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
        "vertexColors":{"type":"array","maxItems":100000,"items":{"type":"string","minLength":8,"maxLength":8}}},
      "additionalProperties":false,"required":["points","indices"]}}},
  "additionalProperties":false,"required":["layer"]})json";

constexpr const char kSetOverlayLayerOutput[] = R"json({"type":"object","properties":{
    "layer":{"type":"string"},"views":{"type":"string","enum":["2d","3d","both"]},"occluded":{"type":"boolean"},
    "polylines":{"type":"integer","minimum":0},"lineVertices":{"type":"integer","minimum":0},
    "points":{"type":"integer","minimum":0},"meshes":{"type":"integer","minimum":0},
    "triangles":{"type":"integer","minimum":0},"generation":{"type":"string"}},
  "additionalProperties":false,
  "required":["layer","views","occluded","polylines","lineVertices","points","meshes","triangles","generation"]})json";

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
        "triangles":{"type":"integer","minimum":0}},
      "additionalProperties":false,
      "required":["layer","views","occluded","polylines","lineVertices","points","meshes","triangles"]}},
    "generation":{"type":"string"}},
  "additionalProperties":false,"required":["layers","generation"]})json";

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
