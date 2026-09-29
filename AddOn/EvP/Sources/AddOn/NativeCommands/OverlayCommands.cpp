// NativeCommands/OverlayCommands -- the overlays in Archicad's own views, as the
// product exposes them. See the header.
//
//   Tapioca.Overlay3D {action}   the 3D overlay: composed at the 3D window's Present
//   Tapioca.Overlay2D {action}   the 2D overlay: the floor plan, composed at its Present
//   Tapioca.OverlayStorySlices {action, ...}  slices on them: the massing slabs' floors,
//                                             or the whole model per storey (below)
//   Tapioca.OverlayAnnotations {action}       the Watch trace's annotations on them
//
//   action  "on"      want it; it starts at once if its view is in front, otherwise
//                     when that view comes forward
//           "off"     stop wanting it; its renderer stops
//           "toggle"  what the menu item does
//           "state"   (the default) report, change nothing
//
// ⚠️ A REFUSED START IS AN ANSWER, NOT A TRANSPORT FAILURE. The response says
// `refused`, the refusal's `code` and whether it may clear by itself; the verb fails
// only when it is asked for something it does not know.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/OverlayCommands.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/CommandRegistration.hpp"
#include "NativeCommands/CommandUtils.hpp" // ReadReal/ReadReals: a JSON whole number is a number too
#include "NativeCommands/OverlayLayerReading.hpp"

#include "ArchViz/InjectedOverlayRuntime.hpp"
#include "ArchViz/OverlayAnnotations.hpp"
#include "ArchViz/OverlayController.hpp"
#include "ArchViz/PlanOverlayRuntime.hpp"
#include "ArchViz/StorySliceOverlay.hpp"

#include <string>
#include <vector>

namespace geomsrv {

namespace {

namespace control = archviz::overlaycontrol;

GS::UniString Utf8 (const std::string& text)
{
    return GS::UniString (text.c_str (), CC_UTF8);
}

GS::UniString Text (uint64_t value)
{
    return GS::UniString (std::to_string (value).c_str (), CC_UTF8);
}

// The action, applied. False only for an action this verb does not know -- which the
// schema's enum refuses for a bus caller before this runs.
bool Apply (control::Overlay which, const GS::ObjectState& params, std::string& action, control::Outcome& outcome)
{
    action = "state";
    if (params.Contains ("action")) {
        GS::UniString text;
        params.Get ("action", text);
        action = text.ToCStr (0, MaxUSize, CC_UTF8).Get ();
    }
    if (action == "on")
        outcome = control::SetWanted (which, true, "api");
    else if (action == "off")
        outcome = control::SetWanted (which, false, "api");
    else if (action == "toggle")
        outcome = control::Toggle (which, "api");
    else if (action == "state")
        outcome = control::Describe (which);
    else
        return false;
    return true;
}

void AddOutcome (GS::ObjectState& os, const std::string& action, const control::Outcome& outcome)
{
    os.Add ("action", Utf8 (action));
    os.Add ("wanted", outcome.wanted);
    os.Add ("running", outcome.running);
    os.Add ("refused", !outcome.ok);
    os.Add ("code", Utf8 (outcome.code));
    os.Add ("message", Utf8 (outcome.message));
    os.Add ("retryable", outcome.retryable);
    os.Add ("view", Utf8 (control::ViewKindName (control::CurrentView ())));
}

class Overlay3DCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "Overlay3D";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        std::string action;
        control::Outcome outcome;
        if (!Apply (control::Overlay::ThreeD, params, action, outcome))
            return NativeCommandResult::Failure (
                EVP_FAIL (GS::UniString ("unknown action '") + Utf8 (action) + "'; expected on, off, toggle or state",
                          "driving the 3D overlay"));
        namespace runtime = archviz::overlayruntime;
        const runtime::Health health = runtime::GetHealth ();
        GS::ObjectState os;
        AddOutcome (os, action, outcome);
        // Whether it is drawing, and if not, where it stopped: `camera` is Locked
        // once the model's camera is found, `host` Ready once the building's own
        // geometry can hide the overlay. `OverlayRuntime` has the whole record.
        os.Add ("camera", Utf8 (runtime::CameraStateName (health.camera)));
        os.Add ("host", Utf8 (runtime::HostStateName (health.host)));
        os.Add ("presentInjections", Text (health.presentInjections));
        return os;
    }
};

class Overlay2DCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "Overlay2D";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        std::string action;
        control::Outcome outcome;
        if (!Apply (control::Overlay::TwoD, params, action, outcome))
            return NativeCommandResult::Failure (
                EVP_FAIL (GS::UniString ("unknown action '") + Utf8 (action) + "'; expected on, off, toggle or state",
                          "driving the 2D overlay"));
        const archviz::planruntime::Status status = archviz::planruntime::GetStatus ();
        GS::ObjectState os;
        AddOutcome (os, action, outcome);
        // What it draws and how it is keeping up: every canvas Present past the
        // first is drawn once prepared, and `drawnWithLastRead` counts the frames
        // whose read was refused (our own redraws) and drew with the one before.
        os.Add ("storey", (GS::Int32) status.storey);
        os.Add ("rings", (GS::Int32) status.rings);
        os.Add ("segments", (GS::Int32) status.segments);
        os.Add ("canvasPresents", Text (status.canvasPresents));
        os.Add ("drawn", Text (status.drawn));
        os.Add ("readsFresh", Text (status.readsFresh));
        os.Add ("drawnWithLastRead", Text (status.drawnWithLastRead));
        os.Add ("lastError", Utf8 (status.lastError));
        return os;
    }
};

// ---- the storey slices and the Watch annotations -----------------------------------

namespace slices = archviz::storysliceoverlay;
using Cut = archviz::slabslices::Cut;
namespace overlays = archviz::overlaylayers;

// One reading of a colour and of an occlusion for every overlay verb.
using overlayreading::OcclusionOf;
using overlayreading::ReadColour;
using overlayreading::ReadFont;
using overlayreading::StringOf;

float FloatOf (const GS::ObjectState& item, const char* key, float fallback)
{
    double value = 0.0;
    return ReadReal (item, key, value) ? float (value) : fallback;
}

bool ReadSliceControls (const GS::ObjectState& params, slices::Controls& controls, std::string& error)
{
    GS::Array<GS::Int32> storeys;
    if (params.Get ("storeys", storeys))
        for (const GS::Int32 storey : storeys)
            controls.storeys.push_back (int (storey));
    if (params.Contains ("views")) {
        const std::string which = StringOf (params, "views");
        controls.views = which == "2d"     ? overlays::Views::TwoD
                         : which == "both" ? overlays::Views::Both
                                           : overlays::Views::ThreeD;
    }
    GS::ObjectState outline;
    if (params.Get ("outline", outline)) {
        if (!ReadColour (outline, "color", controls.outlineRgba, error))
            return false;
        controls.outlineWidthPixels = FloatOf (outline, "widthPixels", controls.outlineWidthPixels);
        controls.outlineDashPixels = FloatOf (outline, "dashPixels", controls.outlineDashPixels);
        controls.outlineBehind = OcclusionOf (outline, controls.outlineBehind);
    }
    GS::ObjectState fill;
    if (params.Get ("fill", fill)) {
        if (!ReadColour (fill, "color", controls.fillRgba, error))
            return false;
        controls.fillBehind = OcclusionOf (fill, controls.fillBehind);
    }
    GS::ObjectState label;
    if (params.Get ("label", label)) {
        if (label.Contains ("show"))
            label.Get ("show", controls.label);
        controls.labelSizePixels = FloatOf (label, "sizePixels", controls.labelSizePixels);
        controls.labelHaloPixels = FloatOf (label, "haloPixels", controls.labelHaloPixels);
        if (!ReadColour (label, "color", controls.labelRgba, error) ||
            !ReadColour (label, "halo", controls.labelHaloRgba, error))
            return false;
        if (label.Contains ("decimals")) {
            GS::Int32 decimals = 1;
            label.Get ("decimals", decimals);
            controls.decimals = uint32_t (decimals < 0 ? 0 : decimals);
        }
        if (label.Contains ("name"))
            label.Get ("name", controls.labelName);
        if (label.Contains ("onSlice"))
            label.Get ("onSlice", controls.labelOnSlice);
        if (!ReadFont (label, "font", controls.labelFont, error))
            return false;
        ReadReal (label, "sizeMetres", controls.labelSizeMetres);
    }
    ReadReal (params, "liftMetres", controls.liftMetres);
    return true;
}

// The Watch annotations' style; the schema has checked the ranges.
bool ReadAnnotationStyle (const GS::ObjectState& params, archviz::overlayannotations::Style& style, std::string& error)
{
    style.lineWidthPixels = FloatOf (params, "lineWidthPixels", style.lineWidthPixels);
    style.contextWidthPixels = FloatOf (params, "contextWidthPixels", style.contextWidthPixels);
    style.pointSizePixels = FloatOf (params, "pointSizePixels", style.pointSizePixels);
    style.textSizePixels = FloatOf (params, "textSizePixels", style.textSizePixels);
    style.haloPixels = FloatOf (params, "haloPixels", style.haloPixels);
    style.occlusion = OcclusionOf (params, style.occlusion);
    if (!ReadColour (params, "halo", style.haloRgba, error) || !ReadFont (params, "font", style.font, error))
        return false;
    GS::ObjectState colors;
    if (params.Get ("colors", colors)) {
        // By annotation::SemanticRole: None, Add, Remove, Modify, Context, Guide.
        const char* const roles[6] = { "none", "add", "remove", "modify", "context", "guide" };
        for (size_t i = 0; i < 6; ++i)
            if (!ReadColour (colors, roles[i], style.roleRgba[i], error))
                return false;
    }
    return true;
}

// Where the slices come from and where the slabs are cut; the schema has checked the
// enums and the ranges.
void ReadSliceRequest (const GS::ObjectState& params, slices::Request& request)
{
    const std::string source = params.Contains ("source") ? StringOf (params, "source") : std::string ("selection");
    request.source = source == "model"      ? slices::Source::Model
                     : source == "elements" ? slices::Source::Elements
                                            : slices::Source::Selection;
    GS::Array<GS::ObjectState> elements;
    if (params.Get ("elements", elements)) {
        for (const GS::ObjectState& element : elements) {
            GS::ObjectState id;
            if (element.Get ("elementId", id))
                request.elements.push_back (StringOf (id, "guid"));
        }
    }
    const std::string cut = params.Contains ("cut") ? StringOf (params, "cut") : std::string ("storeys");
    request.rule.cut = cut == "storeyLevels" ? Cut::StoreyLevels
                       : cut == "step"       ? Cut::Step
                       : cut == "levels"     ? Cut::Levels
                                             : Cut::Storeys;
    ReadReal (params, "stepMetres", request.rule.stepMetres);
    ReadReals (params, "levels", request.rule.levels);
    ReadReal (params, "offsetMetres", request.rule.offsetMetres);
    ReadReal (params, "minTopMetres", request.rule.minTopMetres);
}

GS::ObjectState ElementIdOf (const std::string& guid)
{
    GS::ObjectState id;
    id.Add ("guid", Utf8 (guid));
    return id;
}

GS::ObjectState SlabRecord (const archviz::slabslices::Summary& slab)
{
    GS::ObjectState os;
    os.Add ("elementId", ElementIdOf (slab.guid));
    os.Add ("id", Utf8 (slab.id));
    os.Add ("bottom", slab.bottom);
    os.Add ("top", slab.top);
    os.Add ("footprintM2", slab.footprintM2);
    os.Add ("sliceAreaM2", slab.sliceAreaM2);
    os.Add ("areaM2", slab.areaM2);
    GS::Array<GS::ObjectState> floors;
    for (const archviz::slabslices::Floor& floor : slab.floors) {
        GS::ObjectState record;
        record.Add ("base", floor.base);
        record.Add ("height", floor.height);
        floors.Push (record);
    }
    os.Add ("floors", floors);
    os.Add ("slopedEdges", (GS::Int32) slab.slopedEdges);
    os.Add ("problem", Utf8 (slab.problem));
    return os;
}

class OverlayStorySlicesCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "OverlayStorySlices";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        const std::string action = params.Contains ("action") ? StringOf (params, "action") : std::string ("state");
        slices::State state;
        if (action == "state") {
            state = slices::Describe ();
        }
        else if (action == "off") {
            state = slices::Apply (false, slices::Request {}, slices::Controls {}, false);
        }
        else if (action == "on" || action == "refresh") {
            slices::Controls controls;
            slices::Request request;
            std::string error;
            if (!ReadSliceControls (params, controls, error))
                return NativeCommandResult::Failure (EVP_FAIL (Utf8 (error), "showing the storey slices"));
            ReadSliceRequest (params, request);
            if (request.source == slices::Source::Elements && request.elements.empty ())
                return NativeCommandResult::Failure (
                    EVP_FAIL ("source 'elements' needs the slabs in `elements`", "showing the storey slices"));
            if (request.rule.cut == Cut::Levels && request.rule.levels.empty ())
                return NativeCommandResult::Failure (
                    EVP_FAIL ("cut 'levels' needs the heights in `levels`", "showing the storey slices"));
            state = slices::Apply (true, request, controls, action == "refresh");
        }
        else {
            return NativeCommandResult::Failure (
                EVP_FAIL (GS::UniString ("unknown action '") + Utf8 (action) + "'; expected on, off, refresh or state",
                          "showing the storey slices"));
        }
        GS::ObjectState os;
        os.Add ("action", Utf8 (action));
        os.Add ("enabled", state.enabled);
        os.Add ("source", Utf8 (slices::SourceName (state.source)));
        os.Add ("cut", Utf8 (archviz::slabslices::CutName (state.cut)));
        os.Add ("waiting", state.waiting);
        os.Add ("slices", (GS::Int32) state.slices);
        os.Add ("areaM2", state.areaM2);
        os.Add ("storeys", (GS::Int32) state.storeys);
        os.Add ("snapshot", Text (state.snapshot));
        os.Add ("cuts", (GS::Int32) state.cuts);
        GS::Array<GS::ObjectState> slabs;
        for (const archviz::slabslices::Summary& slab : state.slabs)
            slabs.Push (SlabRecord (slab));
        os.Add ("slabs", slabs);
        GS::Array<GS::ObjectState> skipped;
        for (const archviz::slabsource::Skip& skip : state.skipped) {
            GS::ObjectState record;
            record.Add ("elementId", ElementIdOf (skip.guid));
            record.Add ("reason", Utf8 (skip.reason));
            skipped.Push (record);
        }
        os.Add ("skipped", skipped);
        os.Add ("message", Utf8 (state.message));
        return os;
    }
};

class OverlayAnnotationsCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "OverlayAnnotations";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        const std::string action = params.Contains ("action") ? StringOf (params, "action") : std::string ("state");
        GS::ObjectState styleParams;
        if (params.Get ("style", styleParams)) {
            // A style replaces the one before it; what it leaves out is the default.
            archviz::overlayannotations::Style style;
            std::string error;
            if (!ReadAnnotationStyle (styleParams, style, error))
                return NativeCommandResult::Failure (EVP_FAIL (Utf8 (error), "styling the Watch annotations"));
            archviz::overlayannotations::SetStyle (style);
        }
        archviz::overlayannotations::State state;
        if (action == "on")
            state = archviz::overlayannotations::Apply (true);
        else if (action == "off")
            state = archviz::overlayannotations::Apply (false);
        else if (action == "state")
            state = archviz::overlayannotations::Describe ();
        else
            return NativeCommandResult::Failure (
                EVP_FAIL (GS::UniString ("unknown action '") + Utf8 (action) + "'; expected on, off or state",
                          "showing the Watch annotations"));
        GS::ObjectState os;
        os.Add ("action", Utf8 (action));
        os.Add ("enabled", state.enabled);
        os.Add ("haveFrame", state.haveFrame);
        os.Add ("primitives", (GS::Int32) state.primitives);
        os.Add ("drawn", (GS::Int32) state.drawn);
        os.Add ("frame", Utf8 (state.frame));
        return os;
    }
};

// clang-format off
constexpr const char kOverlayActionInput[] = R"json({"type":"object","properties":{
    "action":{"type":"string","enum":["on","off","toggle","state"]}},
  "additionalProperties":false})json";

constexpr const char kOverlay3DOutput[] = R"json({"type":"object","properties":{
    "action":{"type":"string"},"wanted":{"type":"boolean"},"running":{"type":"boolean"},"refused":{"type":"boolean"},
    "code":{"type":"string"},"message":{"type":"string"},"retryable":{"type":"boolean"},"view":{"type":"string"},
    "camera":{"type":"string"},"host":{"type":"string"},"presentInjections":{"type":"string"}},
  "additionalProperties":false,
  "required":["action","wanted","running","refused","code","message","retryable","view","camera","host",
              "presentInjections"]})json";

constexpr const char kOverlay2DOutput[] = R"json({"type":"object","properties":{
    "action":{"type":"string"},"wanted":{"type":"boolean"},"running":{"type":"boolean"},"refused":{"type":"boolean"},
    "code":{"type":"string"},"message":{"type":"string"},"retryable":{"type":"boolean"},"view":{"type":"string"},
    "storey":{"type":"integer"},"rings":{"type":"integer","minimum":0},"segments":{"type":"integer","minimum":0},
    "canvasPresents":{"type":"string"},"drawn":{"type":"string"},"readsFresh":{"type":"string"},
    "drawnWithLastRead":{"type":"string"},"lastError":{"type":"string"}},
  "additionalProperties":false,
  "required":["action","wanted","running","refused","code","message","retryable","view","storey","rings","segments",
              "canvasPresents","drawn","readsFresh","drawnWithLastRead","lastError"]})json";

constexpr const char kOverlayStorySlicesInput[] = R"json({"type":"object","properties":{
    "action":{"type":"string","enum":["on","off","refresh","state"]},
    "source":{"type":"string","enum":["selection","elements","model"]},
    "elements":{"$ref":"#Elements"},
    "cut":{"type":"string","enum":["storeys","storeyLevels","step","levels"]},
    "stepMetres":{"type":"number","minimum":0.1,"maximum":1000},
    "levels":{"type":"array","minItems":1,"maxItems":1000,"items":{"type":"number"}},
    "offsetMetres":{"type":"number","minimum":-1000,"maximum":1000},
    "minTopMetres":{"type":"number","minimum":0,"maximum":1000},
    "storeys":{"type":"array","maxItems":1000,"items":{"type":"integer"}},
    "views":{"type":"string","enum":["2d","3d","both"]},)json"
    R"json("outline":{"type":"object","properties":{
        "color":{"$ref":"#Color"},
        "widthPixels":{"type":"number","exclusiveMinimum":0,"maximum":16},
        "dashPixels":{"type":"number","minimum":0,"maximum":512},
        "occlusion":{"type":"string","enum":["hide","fade","dash","always"]}},
      "additionalProperties":false},
    "fill":{"type":"object","properties":{
        "color":{"$ref":"#Color"},
        "occlusion":{"type":"string","enum":["hide","fade","always"]}},
      "additionalProperties":false},
    "label":{"type":"object","properties":{
        "show":{"type":"boolean"},
        "sizePixels":{"type":"number","minimum":4,"maximum":64},
        "color":{"$ref":"#Color"},
        "halo":{"$ref":"#Color"},
        "haloPixels":{"type":"number","minimum":0,"maximum":8,"description":"Fixed; absent grows with the text."},
        "decimals":{"type":"integer","minimum":0,"maximum":6},
        "name":{"type":"boolean"},
        "onSlice":{"type":"boolean"},
        "font":{"type":"string","minLength":1,"maxLength":260,"description":"An installed family as Windows lists it, or a .ttf, .otf or .ttc path."},
        "sizeMetres":{"type":"number","minimum":0,"maximum":100}},
      "additionalProperties":false},
    "liftMetres":{"type":"number","minimum":-100,"maximum":100}},
  "additionalProperties":false})json";

constexpr const char kOverlayStorySlicesOutput[] = R"json({"type":"object","properties":{
    "action":{"type":"string"},"enabled":{"type":"boolean"},
    "source":{"type":"string","enum":["selection","elements","model"]},
    "cut":{"type":"string","enum":["storeys","storeyLevels","step","levels"]},
    "waiting":{"type":"boolean"},"slices":{"type":"integer","minimum":0},"areaM2":{"type":"number"},
    "storeys":{"type":"integer","minimum":0},"snapshot":{"type":"string"},"cuts":{"type":"integer","minimum":0},)json"
    R"json("slabs":{"type":"array","items":{"type":"object","properties":{
        "elementId":{"$ref":"#ElementId"},"id":{"type":"string"},"bottom":{"type":"number"},"top":{"type":"number"},
        "footprintM2":{"type":"number"},"sliceAreaM2":{"type":"number"},"areaM2":{"type":"number"},
        "floors":{"type":"array","items":{"type":"object","properties":{
            "base":{"type":"number"},"height":{"type":"number"}},
          "additionalProperties":false,"required":["base","height"]}},
        "slopedEdges":{"type":"integer","minimum":0},"problem":{"type":"string"}},
      "additionalProperties":false,
      "required":["elementId","id","bottom","top","footprintM2","sliceAreaM2","areaM2","floors","slopedEdges",
                  "problem"]}},
    "skipped":{"type":"array","items":{"type":"object","properties":{
        "elementId":{"$ref":"#ElementId"},"reason":{"type":"string"}},
      "additionalProperties":false,"required":["elementId","reason"]}},
    "message":{"type":"string"}},
  "additionalProperties":false,
  "required":["action","enabled","source","cut","waiting","slices","areaM2","storeys","snapshot","cuts","slabs",
              "skipped","message"]})json";

constexpr const char kOverlayAnnotationsInput[] = R"json({"type":"object","properties":{
    "action":{"type":"string","enum":["on","off","state"]},
    "style":{"type":"object","description":"Replaces the previous style; what it leaves out is the default.",
      "properties":{
        "lineWidthPixels":{"type":"number","minimum":0.25,"maximum":16},
        "contextWidthPixels":{"type":"number","minimum":0.25,"maximum":16},
        "pointSizePixels":{"type":"number","minimum":1,"maximum":64},
        "textSizePixels":{"type":"number","minimum":4,"maximum":64},
        "halo":{"$ref":"#Color"},
        "haloPixels":{"type":"number","minimum":0,"maximum":8,"description":"Fixed; absent grows with the text."},
        "occlusion":{"type":"string","enum":["hide","fade","dash","always"]},
        "font":{"type":"string","minLength":1,"maxLength":260,"description":"An installed family as Windows lists it, or a .ttf, .otf or .ttc path."},
        "colors":{"type":"object","properties":{
            "none":{"$ref":"#Color"},"add":{"$ref":"#Color"},"remove":{"$ref":"#Color"},
            "modify":{"$ref":"#Color"},"context":{"$ref":"#Color"},"guide":{"$ref":"#Color"}},
          "additionalProperties":false}},
      "additionalProperties":false}},
  "additionalProperties":false})json";

constexpr const char kOverlayAnnotationsOutput[] = R"json({"type":"object","properties":{
    "action":{"type":"string"},"enabled":{"type":"boolean"},"haveFrame":{"type":"boolean"},
    "primitives":{"type":"integer","minimum":0},"drawn":{"type":"integer","minimum":0},"frame":{"type":"string"}},
  "additionalProperties":false,"required":["action","enabled","haveFrame","primitives","drawn","frame"]})json";

const NativeCommandRegistration kOverlayCommandRegistrations[] = {
    { "Overlay3D", &MakeRegisteredNativeCommand<Overlay3DCommand>, false, kOverlayActionInput, kOverlay3DOutput },
    { "Overlay2D", &MakeRegisteredNativeCommand<Overlay2DCommand>, false, kOverlayActionInput, kOverlay2DOutput },
    { "OverlayStorySlices", &MakeRegisteredNativeCommand<OverlayStorySlicesCommand>, false, kOverlayStorySlicesInput,
      kOverlayStorySlicesOutput },
    { "OverlayAnnotations", &MakeRegisteredNativeCommand<OverlayAnnotationsCommand>, false, kOverlayAnnotationsInput,
      kOverlayAnnotationsOutput },
};
// clang-format on

} // namespace

NativeCommandRegistrations GetOverlayCommandRegistrations ()
{
    return MakeRegistrationView (kOverlayCommandRegistrations);
}

} // namespace geomsrv
