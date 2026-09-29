// NativeCommands/OverlayLayerCommands -- the caller's own content on the overlays.
// See the header.
//
//   Tapioca.SetOverlayLayer {layer, views?, occlusion?, polylines?, points?, meshes?,
//                            texts?, dimensions?, legends?, panels?}
//       add or replace the named layer; coordinates are MODEL METRES: a single one a
//       #Point3D record, bulk geometry flat x, y, z
//   Tapioca.ClearOverlayLayer {layer} | {all: true}
//   Tapioca.OverlayLayers {}          what is set, in draw order
//
// ⚠️ A LAYER IS GEOMETRY AND STYLE ONLY. Each overlay projects it through its own
// transform (OverlayLayers.hpp): the 2D overlay drops z and reads the plan's
// transform at its Present, the 3D overlay reads Archicad's camera at the model's draw.
//
// ⚠️ WHAT THE SCHEMA CANNOT SAY, THE READER CHECKS (OverlayLayerReading.hpp), and
// everything the store's `Validate` refuses is refused with its sentence before
// anything is set.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/OverlayLayerCommands.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/CommandRegistration.hpp"
#include "NativeCommands/OverlayLayerReading.hpp"

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

using overlayreading::ReadLayer;
using overlayreading::StringOf;

const char* ViewsName (layers::Views views)
{
    return views == layers::Views::TwoD ? "2d" : views == layers::Views::ThreeD ? "3d" : "both";
}

const char* OcclusionName (layers::Behind occlusion)
{
    return occlusion == layers::Behind::Fade   ? "fade"
           : occlusion == layers::Behind::Dash ? "dash"
           : occlusion == layers::Behind::Show ? "always"
                                               : "hide";
}

GS::ObjectState SummaryRecord (const layers::Summary& summary)
{
    GS::ObjectState os;
    os.Add ("layer", Utf8 (summary.name));
    os.Add ("views", Utf8 (ViewsName (summary.views)));
    os.Add ("occlusion", Utf8 (OcclusionName (summary.occlusion)));
    os.Add ("polylines", (GS::Int32) summary.polylines);
    os.Add ("lineVertices", (GS::Int32) summary.lineVertices);
    os.Add ("points", (GS::Int32) summary.points);
    os.Add ("meshes", (GS::Int32) summary.meshes);
    os.Add ("triangles", (GS::Int32) summary.triangles);
    os.Add ("texts", (GS::Int32) summary.texts);
    os.Add ("dimensions", (GS::Int32) summary.dimensions);
    os.Add ("legends", (GS::Int32) summary.legends);
    os.Add ("panels", (GS::Int32) summary.panels);
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
// Colours are the shared #Color, "RRGGBB[AA]", checked by the handler (the validator has
// no pattern keyword). A single coordinate is a #Point3D or #Point2D record; bulk
// geometry is flat x, y, z in model metres, stride 3. A colour ramp is a
// preset or stops, with an optional range, bands and isolines -- written out twice
// below, for meshes and for legends, because a schema constant is one literal.
constexpr const char kSetOverlayLayerInput[] = R"json({"type":"object","properties":{
    "layer":{"type":"string","minLength":1,"maxLength":64},
    "views":{"type":"string","enum":["2d","3d","both"]},
    "occlusion":{"type":"string","enum":["hide","fade","dash","always"]},
    "polylines":{"type":"array","maxItems":20000,"items":{"type":"object","properties":{
        "points":{"type":"array","minItems":6,"maxItems":300000,"description":"Packed x, y, z model metres; stride 3.","items":{"type":"number"}},
        "closed":{"type":"boolean"},
        "color":{"$ref":"#Color"},
        "widthPixels":{"type":"number","exclusiveMinimum":0,"maximum":64},
        "dashPixels":{"type":"number","minimum":0,"maximum":512},
        "dashDuty":{"type":"number","minimum":0.05,"maximum":0.95},
        "occlusion":{"type":"string","enum":["hide","fade","dash","always"]}},
      "additionalProperties":false,"required":["points"]}},
    "points":{"type":"array","maxItems":1000,"items":{"type":"object","properties":{
        "points":{"type":"array","minItems":3,"maxItems":300000,"description":"Packed x, y, z model metres; stride 3.","items":{"type":"number"}},
        "color":{"$ref":"#Color"},
        "sizePixels":{"type":"number","exclusiveMinimum":0,"maximum":256},
        "sizeMetres":{"type":"number","exclusiveMinimum":0,"maximum":1000}},
      "additionalProperties":false,"required":["points"]}},
    "meshes":{"type":"array","maxItems":1000,"items":{"type":"object","properties":{
        "points":{"type":"array","minItems":9,"maxItems":300000,"description":"Packed x, y, z model metres; stride 3.","items":{"type":"number"}},
        "indices":{"type":"array","minItems":3,"maxItems":600000,"description":"Three vertex indices per triangle.","items":{"type":"integer","minimum":0}},
        "color":{"$ref":"#Color"},
        "vertexColors":{"type":"array","maxItems":100000,"description":"One colour per vertex.","items":{"$ref":"#Color"}},
        "normals":{"type":"array","minItems":9,"maxItems":300000,"description":"Packed x, y, z per vertex; stride 3.","items":{"type":"number"}},
        "values":{"type":"array","minItems":3,"maxItems":100000,"description":"One value per vertex.","items":{"type":"number"}},
        "colormap":{"type":"object","properties":{
            "preset":{"type":"string","enum":["viridis","inferno","magma","plasma","turbo","coolwarm","greys","sunhours","slope","clearance"]},
            "stops":{"type":"array","minItems":2,"maxItems":16,"items":{"type":"object","properties":{
                "at":{"type":"number","minimum":0,"maximum":1},"color":{"$ref":"#Color"}},
              "additionalProperties":false,"required":["at","color"]}},
            "min":{"type":"number"},"max":{"type":"number"},
            "bands":{"type":"integer","minimum":0,"maximum":64},
            "isolines":{"type":"object","properties":{
                "step":{"type":"number","exclusiveMinimum":0},"color":{"$ref":"#Color"},
                "widthPixels":{"type":"number","minimum":0.25,"maximum":16}},
              "additionalProperties":false,"required":["step"]}},
          "additionalProperties":false},
        "style":{"type":"object","properties":{
            "shading":{"type":"string","enum":["flat","lit","ghost","xray"]},
            "opacity":{"type":"number","minimum":0,"maximum":1},
            "occlusion":{"type":"string","enum":["hide","fade","always"]},
            "cull":{"type":"string","enum":["none","back"]},
            "edges":{"type":"object","properties":{
                "color":{"$ref":"#Color"},
                "widthPixels":{"type":"number","minimum":0.25,"maximum":16},
                "angleDegrees":{"type":"number","minimum":0,"maximum":180}},
              "additionalProperties":false}},
          "additionalProperties":false}},
      "additionalProperties":false,"required":["points","indices"]}},
    "texts":{"type":"array","maxItems":5000,"items":{"type":"object","properties":{
        "text":{"type":"string","minLength":1,"maxLength":512},
        "at":{"$ref":"#Point3D"},
        "screen":{"$ref":"#Point2D"},
        "offsetPixels":{"$ref":"#Point2D"},
        "sizePixels":{"type":"number","minimum":4,"maximum":256},
        "color":{"$ref":"#Color"},
        "halo":{"$ref":"#Color"},
        "haloPixels":{"type":"number","minimum":0,"maximum":8},
        "background":{"$ref":"#Color"},
        "align":{"type":"string","enum":["left","center","right"]},
        "baseline":{"type":"string","enum":["top","middle","bottom","alphabetic"]},
        "rotationDegrees":{"type":"number","minimum":-360,"maximum":360},
        "occlusion":{"type":"string","enum":["hide","fade","always"]},
        "plane":{"type":"object","properties":{
            "direction":{"$ref":"#Point3D"},
            "normal":{"$ref":"#Point3D"},
            "sizeMetres":{"type":"number","minimum":0.001,"maximum":1000}},
          "additionalProperties":false}},
      "additionalProperties":false,"required":["text"]}},
    "dimensions":{"type":"array","maxItems":5000,"items":{"type":"object","properties":{
        "from":{"$ref":"#Point3D"},
        "to":{"$ref":"#Point3D"},
        "offsetMetres":{"type":"number","minimum":-10000,"maximum":10000},
        "direction":{"$ref":"#Point3D"},
        "normal":{"$ref":"#Point3D"},
        "text":{"type":"string","maxLength":512},
        "decimals":{"type":"integer","minimum":0,"maximum":6},
        "unit":{"type":"string","enum":["m","cm","mm"]},
        "showUnit":{"type":"boolean"},
        "color":{"$ref":"#Color"},
        "widthPixels":{"type":"number","exclusiveMinimum":0,"maximum":16},
        "textSizePixels":{"type":"number","minimum":4,"maximum":128},
        "terminator":{"type":"string","enum":["tick","arrow","dot"]},
        "occlusion":{"type":"string","enum":["hide","fade","dash","always"]}},
      "additionalProperties":false,"required":["from","to"]}},)json"
    R"json("legends":{"type":"array","maxItems":8,"items":{"type":"object","properties":{
        "title":{"type":"string","maxLength":512},
        "unit":{"type":"string","maxLength":64},
        "mesh":{"type":"integer","minimum":0},
        "colormap":{"type":"object","properties":{
            "preset":{"type":"string","enum":["viridis","inferno","magma","plasma","turbo","coolwarm","greys","sunhours","slope","clearance"]},
            "stops":{"type":"array","minItems":2,"maxItems":16,"items":{"type":"object","properties":{
                "at":{"type":"number","minimum":0,"maximum":1},"color":{"$ref":"#Color"}},
              "additionalProperties":false,"required":["at","color"]}},
            "min":{"type":"number"},"max":{"type":"number"},
            "bands":{"type":"integer","minimum":0,"maximum":64},
            "isolines":{"type":"object","properties":{
                "step":{"type":"number","exclusiveMinimum":0},"color":{"$ref":"#Color"},
                "widthPixels":{"type":"number","minimum":0.25,"maximum":16}},
              "additionalProperties":false,"required":["step"]}},
          "additionalProperties":false},
        "corner":{"type":"string","enum":["top-left","top-right","bottom-left","bottom-right"]},
        "offsetPixels":{"$ref":"#Point2D"},
        "screen":{"$ref":"#Point2D"},
        "horizontal":{"type":"boolean"},
        "lengthPixels":{"type":"number","minimum":20,"maximum":4000},
        "widthPixels":{"type":"number","minimum":2,"maximum":200},
        "ticks":{"type":"integer","minimum":2,"maximum":32},
        "decimals":{"type":"integer","minimum":0,"maximum":6},
        "tickValues":{"type":"array","maxItems":32,"items":{"type":"number"}},
        "tickLabels":{"type":"array","maxItems":32,"items":{"type":"string","maxLength":128}},
        "sizePixels":{"type":"number","minimum":4,"maximum":128},
        "titleSizePixels":{"type":"number","minimum":0,"maximum":128},
        "paddingPixels":{"type":"number","minimum":0,"maximum":64},
        "color":{"$ref":"#Color"},
        "halo":{"$ref":"#Color"},
        "background":{"$ref":"#Color"},
        "barBorder":{"$ref":"#Color"}},
      "additionalProperties":false}},)json"
    R"json("panels":{"type":"array","maxItems":32,"items":{"type":"object","properties":{
        "title":{"type":"string","maxLength":512},
        "anchor":{"type":"string","enum":["top-left","top","top-right","left","center","right","bottom-left","bottom",
                                          "bottom-right"]},
        "offsetPixels":{"$ref":"#Point2D"},
        "widthPixels":{"type":"number","minimum":0,"maximum":4000},
        "sizePixels":{"type":"number","minimum":6,"maximum":96},
        "color":{"$ref":"#Color"},
        "background":{"$ref":"#Color"},
        "border":{"$ref":"#Color"},
        "roundingPixels":{"type":"number","minimum":0,"maximum":64},
        "paddingPixels":{"type":"number","minimum":0,"maximum":64},
        "items":{"type":"array","maxItems":200,"items":{"type":"object","properties":{
            "kind":{"type":"string","enum":["text","row","separator","spacing","progress","swatch","ramp","plot",
                                            "table"]},
            "text":{"type":"string","maxLength":4096},
            "value":{"type":"string","maxLength":512},
            "color":{"$ref":"#Color"},
            "sizePixels":{"type":"number","minimum":0,"maximum":96},
            "wrap":{"type":"boolean"},
            "fraction":{"type":"number"},
            "colormap":{"type":"object","properties":{
                "preset":{"type":"string","enum":["viridis","inferno","magma","plasma","turbo","coolwarm","greys","sunhours","slope","clearance"]},
                "stops":{"type":"array","minItems":2,"maxItems":16,"items":{"type":"object","properties":{
                    "at":{"type":"number","minimum":0,"maximum":1},"color":{"$ref":"#Color"}},
                  "additionalProperties":false,"required":["at","color"]}},
                "min":{"type":"number"},"max":{"type":"number"},
                "bands":{"type":"integer","minimum":0,"maximum":64}},
              "additionalProperties":false},
            "ticks":{"type":"integer","minimum":2,"maximum":32},
            "decimals":{"type":"integer","minimum":0,"maximum":6},
            "unit":{"type":"string","maxLength":64},
            "tickValues":{"type":"array","maxItems":32,"items":{"type":"number"}},
            "tickLabels":{"type":"array","maxItems":32,"items":{"type":"string","maxLength":128}},
            "widthPixels":{"type":"number","minimum":0,"maximum":4000},
            "heightPixels":{"type":"number","minimum":0,"maximum":2000},
            "values":{"type":"array","maxItems":4096,"items":{"type":"number"}},
            "min":{"type":"number"},"max":{"type":"number"},
            "columns":{"type":"array","maxItems":16,"items":{"type":"string","maxLength":512}},
            "rows":{"type":"array","maxItems":200,"items":{"type":"array","maxItems":16,
                                                          "items":{"type":"string","maxLength":512}}}},
          "additionalProperties":false,"required":["kind"]}}},
      "additionalProperties":false}}},
  "additionalProperties":false,"required":["layer"]})json";

constexpr const char kSetOverlayLayerOutput[] = R"json({"type":"object","properties":{
    "layer":{"type":"string"},"views":{"type":"string","enum":["2d","3d","both"]},
    "occlusion":{"type":"string","enum":["hide","fade","dash","always"]},
    "polylines":{"type":"integer","minimum":0},"lineVertices":{"type":"integer","minimum":0},
    "points":{"type":"integer","minimum":0},"meshes":{"type":"integer","minimum":0},
    "triangles":{"type":"integer","minimum":0},"texts":{"type":"integer","minimum":0},
    "dimensions":{"type":"integer","minimum":0},"legends":{"type":"integer","minimum":0},
    "panels":{"type":"integer","minimum":0},"generation":{"type":"string"}},
  "additionalProperties":false,
  "required":["layer","views","occlusion","polylines","lineVertices","points","meshes","triangles","texts",
              "dimensions","legends","panels","generation"]})json";

constexpr const char kClearOverlayLayerInput[] = R"json({"type":"object","properties":{
    "layer":{"type":"string","minLength":1,"maxLength":64},"all":{"type":"boolean"}},
  "additionalProperties":false})json";

constexpr const char kClearOverlayLayerOutput[] = R"json({"type":"object","properties":{
    "cleared":{"type":"integer","minimum":0},"layers":{"type":"integer","minimum":0},"generation":{"type":"string"}},
  "additionalProperties":false,"required":["cleared","layers","generation"]})json";

constexpr const char kOverlayLayersInput[] = R"json({"type":"object","properties":{},"additionalProperties":false})json";

constexpr const char kOverlayLayersOutput[] = R"json({"type":"object","properties":{
    "layers":{"type":"array","items":{"type":"object","properties":{
        "layer":{"type":"string"},"views":{"type":"string","enum":["2d","3d","both"]},
        "occlusion":{"type":"string","enum":["hide","fade","dash","always"]},
        "polylines":{"type":"integer","minimum":0},"lineVertices":{"type":"integer","minimum":0},
        "points":{"type":"integer","minimum":0},"meshes":{"type":"integer","minimum":0},
        "triangles":{"type":"integer","minimum":0},"texts":{"type":"integer","minimum":0},
        "dimensions":{"type":"integer","minimum":0},"legends":{"type":"integer","minimum":0},
        "panels":{"type":"integer","minimum":0}},
      "additionalProperties":false,
      "required":["layer","views","occlusion","polylines","lineVertices","points","meshes","triangles","texts",
                  "dimensions","legends","panels"]}},
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
