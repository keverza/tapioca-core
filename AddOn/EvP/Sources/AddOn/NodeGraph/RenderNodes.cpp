#include "NodeGraph/RenderNodes.hpp"

#include "NodeGraph/ArchicadNodes.hpp"
#include "NodeGraph/CaptureService.hpp"
#include "NodeGraph/Json.hpp"
#include "NodeGraph/NodeLifting.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace evp::nodegraph {
namespace {

constexpr const char kRenderSettings[] = "archviz.renderSettings";
constexpr const char kCapture[] = "archviz.capture";

// The capture's own limits, restated here so a bad number is CLAMPED at the node
// the user is typing in rather than refused by a schema minutes later, halfway
// through a batch. StartDiligentCapture's schema is the authority; these must
// match it, and the offline suite asserts they do.
constexpr int64_t kMinPixels = 16;
constexpr int64_t kMaxPixels = 8192;
constexpr double kMinLineWidth = 0.1;
constexpr double kMaxLineWidth = 32.0;

bool ReadBool (const Node& node, const char* id, bool fallback)
{
    const auto found = node.parameters.find (id);
    if (found == node.parameters.end () || found->second.Type () != ValueType::Bool)
        return fallback;
    return std::get<bool> (found->second.DataValue ());
}

double ReadDouble (const Node& node, const char* id, double fallback)
{
    const auto found = node.parameters.find (id);
    if (found == node.parameters.end ())
        return fallback;
    if (found->second.Type () == ValueType::Double)
        return std::get<double> (found->second.DataValue ());
    if (found->second.Type () == ValueType::Integer)
        return static_cast<double> (std::get<int64_t> (found->second.DataValue ()));
    return fallback;
}

std::string ReadString (const Node& node, const char* id, const char* fallback)
{
    const auto found = node.parameters.find (id);
    if (found == node.parameters.end () || found->second.Type () != ValueType::String)
        return fallback;
    const std::string text = std::get<std::string> (found->second.DataValue ());
    return text.empty () ? std::string (fallback) : text;
}

// One of a closed set, or the default. A client that sent a fourth spelling gets
// the default rather than a failed graph - the same reading the runtime takes of
// an unknown preview target.
std::string ReadChoice (const Node& node, const char* id, std::initializer_list<const char*> allowed,
                        const char* fallback)
{
    const std::string text = ReadString (node, id, fallback);
    for (const char* option : allowed) {
        if (text == option)
            return text;
    }
    return fallback;
}

int64_t ClampPixels (double value)
{
    if (!std::isfinite (value))
        return kMinPixels;
    return std::clamp (static_cast<int64_t> (std::llround (value)), kMinPixels, kMaxPixels);
}

int HexDigit (char character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

} // namespace

const char* const kRenderSettingsNodeType = kRenderSettings;
const char* const kCaptureNodeType = kCapture;
const char* const kRenderSettingsOutput = "settings";

uint32_t PackRgba (const std::string& hex, int alpha, uint32_t fallback)
{
    // "#RRGGBB", which is the only spelling the editor's colour widget produces
    // and validates. Anything else is the fallback rather than an error: a
    // hand-edited document with a typo in a contour colour should still render.
    if (hex.size () != 7 || hex[0] != '#')
        return fallback;
    uint32_t packed = 0;
    for (size_t i = 1; i < hex.size (); ++i) {
        const int digit = HexDigit (hex[i]);
        if (digit < 0)
            return fallback;
        packed = (packed << 4) | static_cast<uint32_t> (digit);
    }
    const uint32_t clamped = static_cast<uint32_t> (std::clamp (alpha, 0, 255));
    return (packed << 8) | clamped;
}

std::string EncodeRenderSettings (const RenderSettings& settings)
{
    json::JsonObject fields;
    fields["width"] = json::JsonValue::Integer (settings.width);
    fields["height"] = json::JsonValue::Integer (settings.height);
    fields["renderQuality"] = json::JsonValue::String (settings.renderQuality);
    fields["storySlices"] = json::JsonValue::Bool (settings.storySlices);
    fields["storySliceFill"] = json::JsonValue::Bool (settings.storySliceFill);
    fields["storySliceOccluded"] = json::JsonValue::String (settings.storySliceOccluded);
    fields["storySliceWidthPixels"] = json::JsonValue::Double (settings.storySliceWidthPixels);
    // ⚠️ INTEGER, AND IT DOES NOT FIT IN AN int32. 0xC8C8C84D is above 2^31, so
    // it is carried as int64 - the capture's schema says `integer` with no bound
    // for exactly this reason. Narrowing it anywhere would turn a light grey fill
    // into a negative number and, on the renderer's side, into nonsense.
    fields["storySliceRgba"] = json::JsonValue::Integer (static_cast<int64_t> (settings.storySliceRgba));
    fields["storySliceFillRgba"] = json::JsonValue::Integer (static_cast<int64_t> (settings.storySliceFillRgba));
    return json::Write (json::JsonValue::Object (std::move (fields)), 0);
}

bool DecodeRenderSettings (const std::string& encoded, RenderSettings& settings)
{
    const json::ParseResult parsed = json::Parse (encoded);
    if (!parsed.ok || parsed.value.Find ("renderQuality") == nullptr)
        return false;

    RenderSettings read;
    const auto integer = [&parsed] (const char* key, int64_t fallback) {
        const json::JsonValue* found = parsed.value.Find (key);
        int64_t value = fallback;
        if (found != nullptr && found->AsInteger (value))
            return value;
        return fallback;
    };
    const auto number = [&parsed] (const char* key, double fallback) {
        const json::JsonValue* found = parsed.value.Find (key);
        double value = fallback;
        if (found != nullptr && found->AsDouble (value))
            return value;
        return fallback;
    };
    const auto flag = [&parsed] (const char* key, bool fallback) {
        const json::JsonValue* found = parsed.value.Find (key);
        bool value = fallback;
        if (found != nullptr && found->AsBool (value))
            return value;
        return fallback;
    };
    const auto text = [&parsed] (const char* key, const char* fallback) {
        const json::JsonValue* found = parsed.value.Find (key);
        std::string value;
        if (found != nullptr && found->AsString (value) && !value.empty ())
            return value;
        return std::string (fallback);
    };

    read.width = std::clamp (integer ("width", 1920), kMinPixels, kMaxPixels);
    read.height = std::clamp (integer ("height", 1080), kMinPixels, kMaxPixels);
    read.renderQuality = text ("renderQuality", "realistic");
    if (read.renderQuality != "fast" && read.renderQuality != "realistic")
        read.renderQuality = "realistic";
    read.storySlices = flag ("storySlices", false);
    read.storySliceFill = flag ("storySliceFill", false);
    read.storySliceOccluded = text ("storySliceOccluded", "dashed");
    read.storySliceWidthPixels = std::clamp (number ("storySliceWidthPixels", 2.0), kMinLineWidth, kMaxLineWidth);
    read.storySliceRgba = static_cast<uint32_t> (integer ("storySliceRgba", 0x3C3C3CFF));
    read.storySliceFillRgba = static_cast<uint32_t> (integer ("storySliceFillRgba", 0xC8C8C84D));
    settings = read;
    return true;
}

// Defined at the end of this file. Declared up here because BOTH the tree body
// registered below and the dispatcher reach it first.
bool ExecuteCaptureNode (const Node& node, const ValueMap& inputs, const NodeExecutionContext& context,
                         ValueMap& outputs, std::string& error);

void RegisterRenderNodes (NodeRegistry& registry)
{
    std::string error;

    // WHAT A HEADLESS CAPTURE IS TOLD, and nothing else.
    //
    // ⚠️ Pure/Worker AND IT READS NOTHING. The node is a set of numbers the user
    // typed; evaluating it encodes its own parameters. That is what lets a
    // workflow carry its output size and quality with no project open, and it is
    // why the node cannot go stale.
    //
    // ⚠️ EVERY PARAMETER HERE IS ONE StartDiligentCapture ACTUALLY ACCEPTS.
    // Deliberately smaller than the viewer's control panel: sun, environment
    // map, exposure, render mode and debug view are GLOBAL viewer state with
    // their own commands, and a control on this node claiming to set them for
    // one capture would be a lie that outlives the run.
    NodeType settings;
    settings.id = kRenderSettings;
    settings.label = "Render Settings";
    settings.category = "Render";
    settings.description =
        "How a headless capture is rendered: image size, quality, and the per-storey section contours.";
    settings.executionDomain = ExecutionDomain::Worker;
    settings.effect = EffectKind::Pure;

    const auto number = [] (const char* id, const char* label, ValueType type, Value fallback, const char* section,
                            int order, double minimum, double maximum, const char* unit, const char* help) {
        ParameterSchema parameter { id, label, type, false, std::move (fallback) };
        ParameterUi ui;
        ui.widget = ParameterWidget::Number;
        ui.section = section;
        ui.order = order;
        ui.minimum = minimum;
        ui.maximum = maximum;
        ui.unit = unit;
        ui.help = help;
        parameter.ui = ui;
        return parameter;
    };

    settings.parameters.push_back (number ("width", "Width", ValueType::Integer, Value (static_cast<int64_t> (1920)),
                                           "Image", 1, static_cast<double> (kMinPixels),
                                           static_cast<double> (kMaxPixels), "px", "The captured image's width."));
    settings.parameters.push_back (number ("height", "Height", ValueType::Integer, Value (static_cast<int64_t> (1080)),
                                           "Image", 2, static_cast<double> (kMinPixels),
                                           static_cast<double> (kMaxPixels), "px", "The captured image's height."));
    {
        ParameterSchema quality { "renderQuality", "Quality", ValueType::String, false,
                                  Value (std::string ("realistic")) };
        ParameterUi ui;
        ui.widget = ParameterWidget::Select;
        ui.section = "Image";
        ui.order = 3;
        ui.help = "Realistic uses the full post-processing chain; Fast skips it.";
        // Literal options rather than an optionSource: these two are the
        // renderer's own vocabulary and do not depend on the open project, which
        // is the whole distinction optionSource exists to draw.
        ui.options.push_back ({ "Realistic", Value (std::string ("realistic")) });
        ui.options.push_back ({ "Fast", Value (std::string ("fast")) });
        quality.ui = ui;
        settings.parameters.push_back (std::move (quality));
    }

    const auto boolean = [] (const char* id, const char* label, bool fallback, int order, const char* help) {
        ParameterSchema parameter { id, label, ValueType::Bool, false, Value (fallback) };
        ParameterUi ui;
        ui.widget = ParameterWidget::Boolean;
        ui.section = "Story slices";
        ui.order = order;
        ui.help = help;
        parameter.ui = ui;
        return parameter;
    };
    settings.parameters.push_back (
        boolean ("storySlices", "Contours", false, 10, "Draw the per-storey section contour over the model."));
    settings.parameters.push_back (boolean ("storySliceFill", "Fill", false, 11, "Tint the area inside each contour."));
    {
        ParameterSchema occluded { "storySliceOccluded", "Behind geometry", ValueType::String, false,
                                   Value (std::string ("dashed")) };
        ParameterUi ui;
        ui.widget = ParameterWidget::Select;
        ui.section = "Story slices";
        ui.order = 12;
        ui.help = "How the parts of a contour buried inside the building are drawn.";
        ui.options.push_back ({ "Dashed", Value (std::string ("dashed")) });
        ui.options.push_back ({ "Hidden", Value (std::string ("hidden")) });
        ui.options.push_back ({ "Solid", Value (std::string ("solid")) });
        occluded.ui = ui;
        settings.parameters.push_back (std::move (occluded));
    }
    settings.parameters.push_back (number ("storySliceWidthPixels", "Line width", ValueType::Double, Value (2.0),
                                           "Story slices", 13, kMinLineWidth, kMaxLineWidth, "px",
                                           "Contour width, in pixels, held constant while zooming."));

    const auto colour = [] (const char* id, const char* label, const char* fallback, int order, const char* help) {
        ParameterSchema parameter { id, label, ValueType::String, false, Value (std::string (fallback)) };
        ParameterUi ui;
        ui.widget = ParameterWidget::Color;
        ui.section = "Story slices";
        ui.order = order;
        ui.help = help;
        parameter.ui = ui;
        return parameter;
    };
    settings.parameters.push_back (colour ("storySliceColor", "Line colour", "#3C3C3C", 14, "The contour's colour."));
    settings.parameters.push_back (colour ("storySliceFillColor", "Fill colour", "#C8C8C8", 15, "The tint's colour."));
    // ⚠️ A SEPARATE PARAMETER BECAUSE THE COLOUR WIDGET HAS NO ALPHA. It is a
    // six-digit hex string by contract (TextControl's HEX), while the renderer
    // takes packed RGBA - and the default fill is deliberately translucent
    // (0x4D), so dropping alpha would hide the building under its own tint.
    settings.parameters.push_back (number ("storySliceFillOpacity", "Fill opacity", ValueType::Double, Value (30.0),
                                           "Story slices", 16, 0.0, 100.0, "%",
                                           "How opaque the tint is. The contour line itself is always solid."));

    settings.outputs.push_back ({ kRenderSettingsOutput, "Settings", ValueType::String });
    if (!registry.Register (std::move (settings), error))
        throw std::logic_error (error);

    // RENDER ONE IMAGE PER CAMERA, FROM ONE MODEL EXTRACTION.
    //
    // ⚠️ HostUiWrite, AND THAT IS THE MOST IMPORTANT LINE IN THIS
    // REGISTRATION. It makes the node DEFERRED and PERMISSIONED: automatic
    // evaluation reports it as skipped and never runs it, and the only thing
    // that does is the editor's per-node commit button. A capture takes minutes
    // - the model is walked in full before the first frame - so a node that
    // fired because somebody nudged a slider would make the graph unusable. The
    // machinery already exists for Set Selection; this node needs no new
    // affordance, only the honest effect declaration.
    //
    // ⚠️ AND IT REALLY IS A HOST UI WRITE, not just an expensive read.
    // Choosing a Model View applies it with ACAPI_View_GoToView and LEAVES the
    // 3D window there, by explicit decision. Declaring the node Pure or
    // ReadModel would be a lie that the permission gate would then honour.
    NodeType capture;
    capture.id = kCapture;
    capture.label = "Capture Screenshot";
    capture.category = "Render";
    capture.description = "Renders one image per camera, headlessly, from a single model extraction. "
                          "Press its button to run - a capture takes minutes and never runs on its own.";
    // Not "Send to Archicad": this node sends nothing anywhere, it renders
    // images to disk. See NodeType::commitLabel.
    capture.commitLabel = "Capture";
    capture.executionDomain = ExecutionDomain::Worker;
    capture.effect = EffectKind::HostUiWrite;
    // ⚠️ A Project GENERATION, so the node goes stale when the model
    // moves. It renders the BUILDING; a graph that served yesterday's frames
    // after a wall was edited would be confidently wrong in the one way nobody
    // checks - the paths still resolve and the images still open.
    capture.generations = { GenerationDomain::Project };
    // Not required: this is a node you drop and wire afterwards, and an unwired
    // one is empty rather than broken.
    capture.inputs.push_back ({ "cameras", "Cameras", ValueType::List, false });
    // ⚠️ THE SUN COMES IN ON ITS OWN WIRE, index-parallel to the cameras,
    // because the camera node publishes it that way - one output positions the
    // frame and the other lights it. Optional: an unwired sun renders every
    // frame under the project's own lighting, which is what a graph built before
    // the sun existed does.
    capture.inputs.push_back ({ "sun", "Sun", ValueType::List, false });
    capture.inputs.push_back ({ "settings", "Settings", ValueType::String, false });
    {
        ParameterSchema view { "modelView", "Model View", ValueType::String, false, Value (std::string {}) };
        ParameterUi ui;
        ui.widget = ParameterWidget::Select;
        ui.section = "Scope";
        ui.help = "A saved 3D view whose visibility the capture should use. Empty uses the 3D window as it is.";
        // ⚠️ AN optionSource, NOT LITERAL OPTIONS, for the same reason the
        // layer picker is one: which views exist is THIS project's answer and
        // changes with the open document, so it can be neither a static catalog
        // entry nor something the browser enumerates.
        ui.optionSource = ParameterOptionSource::ModelView3D;
        view.ui = ui;
        capture.parameters.push_back (std::move (view));
    }
    capture.outputs.push_back ({ "paths", "Paths", ValueType::List });
    capture.outputs.push_back ({ "count", "Count", ValueType::Integer });
    // ⚠️ IT OPTS OUT OF LIFTING, AND THIS IS THE ONE DECISION THAT MAKES
    // THE BATCH A BATCH. Lifting walks a body over the tree, calling it once per
    // set of values - so a lifted capture would run ONCE PER CAMERA, and every
    // one of those runs is a full model extraction. That is precisely what
    // StartDiligentCaptureBatch was written to stop, and the graph would have
    // quietly reintroduced it.
    //
    // NodeType::treeBody's own warning names this shape exactly: a type opts out
    // only when the loop would destroy what it does. A Flatten run per item is
    // the identity function; a capture run per camera is eight extractions of one
    // building. It is also why the node reads its ports directly - with a tree
    // body every declared input is PRESENT as an empty tree, so an unwired
    // Settings port is an absent argument rather than zero iterations.
    capture.treeBody = [] (const Node& node, const data::TreeMap& inputs, const NodeExecutionContext& context,
                           data::TreeMap& outputs, std::string& error) {
        ValueMap flat;
        for (const auto& [portId, tree] : inputs)
            flat.emplace (portId, ProjectTreeToValue (tree));

        ValueMap produced;
        if (!ExecuteCaptureNode (node, flat, context, produced, error))
            return false;

        // Each output converts back at ITS OWN declared item type. `Any` would
        // be the tempting shortcut and the registry rejects it: `count` is an
        // Integer port, and a tree of Any handed to it is "invalid output".
        for (auto& [portId, value] : produced) {
            const data::ItemType itemType = portId == "count" ? data::ItemType::Integer : data::ItemType::String;
            data::TreeValue tree;
            std::string conversion;
            if (!TreeFromValue (value, itemType, tree, conversion)) {
                error = conversion;
                return false;
            }
            outputs.emplace (portId, std::move (tree));
        }
        return true;
    };
    if (!registry.Register (std::move (capture), error))
        throw std::logic_error (error);
}

bool IsRenderNodeType (const std::string& nodeTypeId)
{
    return nodeTypeId == kRenderSettings || nodeTypeId == kCapture;
}

namespace {

// The cameras and their suns, recombined into what the renderer takes.
//
// ⚠️ THE SUN LIST IS INDEX-MATCHED AND A SHORT ONE DOES NOT SHIFT
// ANYTHING. A sun list shorter than the cameras - an older graph, a wire from
// somewhere else - leaves the remaining frames on the project's own lighting
// rather than reusing the last sun, which would light three viewpoints with one
// afternoon and look deliberate.
std::vector<ViewCamera> CamerasWithSun (const ValueMap& inputs)
{
    const auto cameras = inputs.find ("cameras");
    if (cameras == inputs.end ())
        return {};
    std::vector<ViewCamera> list = CamerasFromValue (cameras->second);

    const auto sun = inputs.find ("sun");
    if (sun == inputs.end ())
        return list;

    // The sun output is a list of SetDiligentSun-shaped objects; the camera rows
    // already carry their own sun, so this only overrides where a wire says so.
    std::vector<std::string> encoded;
    if (sun->second.Type () == ValueType::String)
        encoded.push_back (std::get<std::string> (sun->second.DataValue ()));
    else if (sun->second.Type () == ValueType::List) {
        for (const Value& item : sun->second.Items ()) {
            if (item.Type () == ValueType::String)
                encoded.push_back (std::get<std::string> (item.DataValue ()));
        }
    }

    for (size_t i = 0; i < list.size () && i < encoded.size (); ++i) {
        const json::ParseResult parsed = json::Parse (encoded[i]);
        if (!parsed.ok)
            continue;
        bool enabled = false;
        const json::JsonValue* on = parsed.value.Find ("enabled");
        if (on != nullptr)
            on->AsBool (enabled);
        list[i].hasSun = enabled;
        const auto angle = [&parsed] (const char* key) {
            const json::JsonValue* found = parsed.value.Find (key);
            double value = 0.0;
            if (found != nullptr)
                found->AsDouble (value);
            return value;
        };
        list[i].sunAzimuthDegrees = angle ("azimuthDegrees");
        list[i].sunAltitudeDegrees = angle ("altitudeDegrees");
    }
    return list;
}

} // namespace

bool ExecuteRenderNode (const Node& node, const ValueMap& inputs, const NodeExecutionContext& context,
                        ValueMap& outputs, std::string& error)
{
    if (node.nodeType == kCapture)
        return ExecuteCaptureNode (node, inputs, context, outputs, error);
    if (node.nodeType != kRenderSettings) {
        error = "unknown render node type: " + node.nodeType;
        return false;
    }

    RenderSettings settings;
    // CLAMPED IN THE BODY, not merely hinted at in the UI. NodeType.hpp is
    // explicit that a parameter's range metadata is a display hint the evaluator
    // never reads, so a node that wants a bound has to enforce it here - and this
    // one wants it, because the alternative is a batch that dies on a schema
    // rejection after the model has already been extracted.
    settings.width = ClampPixels (ReadDouble (node, "width", 1920.0));
    settings.height = ClampPixels (ReadDouble (node, "height", 1080.0));
    settings.renderQuality = ReadChoice (node, "renderQuality", { "fast", "realistic" }, "realistic");
    settings.storySlices = ReadBool (node, "storySlices", false);
    settings.storySliceFill = ReadBool (node, "storySliceFill", false);
    settings.storySliceOccluded = ReadChoice (node, "storySliceOccluded", { "hidden", "dashed", "solid" }, "dashed");
    const double width = ReadDouble (node, "storySliceWidthPixels", 2.0);
    settings.storySliceWidthPixels =
        std::isfinite (width) ? std::clamp (width, kMinLineWidth, kMaxLineWidth) : kMinLineWidth;

    // The line is opaque, always: that is the renderer's own default (0x...FF)
    // and there is no control for it, because a half-transparent contour over the
    // geometry it is describing reads as a rendering fault rather than a choice.
    settings.storySliceRgba = PackRgba (ReadString (node, "storySliceColor", "#3C3C3C"), 255, 0x3C3C3CFFu);
    const double opacity = ReadDouble (node, "storySliceFillOpacity", 30.0);
    // ⚠️ `/ 100 * 255`, NOT `* 2.55`. The literal 2.55 is not representable, so
    // 50 * 2.55 lands at 127.49999999999999 and rounds DOWN - a half-opaque fill
    // that is one step off, and 100% that is not quite 255. This form is exact at
    // both ends and reproduces the renderer's own 30% default as 0x4D.
    const int alpha = std::isfinite (opacity)
                          ? static_cast<int> (std::llround (std::clamp (opacity, 0.0, 100.0) / 100.0 * 255.0))
                          : 0x4D;
    settings.storySliceFillRgba = PackRgba (ReadString (node, "storySliceFillColor", "#C8C8C8"), alpha, 0xC8C8C84Du);

    outputs.emplace (kRenderSettingsOutput, Value (EncodeRenderSettings (settings)));
    return true;
}

bool ExecuteCaptureNode (const Node& node, const ValueMap& inputs, const NodeExecutionContext& context,
                         ValueMap& outputs, std::string& error)
{
    const std::vector<ViewCamera> cameras = CamerasWithSun (inputs);
    // An unwired node is EMPTY, not an error - the seconds between dropping a
    // node and wiring it are not a broken graph. This is the same reading every
    // container takes of an unwired input.
    if (cameras.empty ()) {
        outputs.emplace ("paths", Argument::FromItems ({}));
        outputs.emplace ("count", Value (static_cast<int64_t> (0)));
        return true;
    }

    ICaptureService* service = ActiveCaptureService ();
    if (service == nullptr) {
        // Ordinary rather than exceptional: the offline suite and any headless
        // run have no renderer installed. The message says which half is
        // missing, because "capture failed" would send the reader to the cameras.
        error = "no renderer is available to capture with";
        return false;
    }

    CaptureBatchRequest request;
    request.cameras = cameras;
    const auto settings = inputs.find ("settings");
    if (settings != inputs.end () && settings->second.Type () == ValueType::String)
        request.settings = std::get<std::string> (settings->second.DataValue ());
    const auto view = node.parameters.find ("modelView");
    if (view != node.parameters.end () && view->second.Type () == ValueType::String)
        request.modelViewGuid = std::get<std::string> (view->second.DataValue ());

    std::vector<std::string> paths;
    // ⚠️ THE EVALUATOR'S CANCELLATION IS THREADED ALL THE WAY DOWN. A batch
    // runs for minutes; a Stop that only took effect when it finished would not
    // be a Stop, and this node holds a worker thread for the whole time.
    const std::function<bool ()> cancelled = [&context] () { return context.cancellation.IsCancelled (); };
    if (!service->Capture (request, cancelled, paths, error))
        return false;

    std::vector<Value> items;
    items.reserve (paths.size ());
    for (const std::string& path : paths)
        items.emplace_back (path);
    outputs.emplace ("paths", Argument::FromItems (std::move (items)));
    outputs.emplace ("count", Value (static_cast<int64_t> (paths.size ())));
    return true;
}

} // namespace evp::nodegraph
