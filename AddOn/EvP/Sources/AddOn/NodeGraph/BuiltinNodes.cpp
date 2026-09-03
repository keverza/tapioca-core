#include "NodeGraph/BuiltinNodes.hpp"

#include "NodeGraph/NodeInputs.hpp"
#include "NodeGraph/ParameterDescriptors.hpp"

#include "Geometry/GeometryEngine.hpp"
#include "Geometry/Curves.hpp"
#include "Geometry/Primitives.hpp"
#include "Geometry/Transforms.hpp"
#include "NodeGraph/PreviewProjection.hpp"
#include "NodeGraph/ValueText.hpp"

#include <algorithm>
#include <vector>
#include <memory>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace evp::nodegraph {
namespace {

PortSchema Port (const char* id, ValueType valueType, bool multiple = false)
{
    return { id, id, valueType, true, multiple };
}

NodeType PureNode (const char* id, const char* label, const char* description)
{
    return { id, label, "Core", description };
}

double Number (const Argument& value)
{
    return std::get<double> (value.DataValue ());
}

// The slider's own contract, applied by the NODE and not by the control.
//
// WARNING: A RANGE A CLIENT HONOURS IS NOT A RANGE. The editor clamps so the
// user cannot drag past the end; this clamps so a graph loaded from a file, a
// pasted value or a second client cannot produce an out-of-range answer either.
// Rounding is here for the same reason: 'decimals' is what the number IS, not
// how it is spelled on screen, so two decimals means the downstream node sees
// two decimals.
double ClampAndRound (double value, double minimum, double maximum, int decimals)
{
    if (minimum > maximum)
        std::swap (minimum, maximum);
    const double clamped = std::min (std::max (value, minimum), maximum);
    if (decimals < 0)
        return clamped;
    const double scale = std::pow (10.0, static_cast<double> (std::min (decimals, 15)));
    return std::round (clamped * scale) / scale;
}

// One Archicad attribute picker.
//
// WARNING: Pure/Worker, AND THAT IS THE POINT - the same reasoning the selection
// set is built on (see ArchicadNodes.cpp). The node's output IS its stored
// parameter, so evaluating it reads nothing from the host: it needs no project,
// runs offline, stays out of MainThreadGate, and does not go dirty every time
// somebody edits an attribute they did not pick. What DOES need Archicad is
// listing the choices, and that is a separate native verb the client calls when
// it draws the list - never an evaluation, and never the browser enumerating a
// model domain itself.
NodeType AttributePicker (const char* id, const char* label, const char* description, ValueType valueType,
                          ParameterOptionSource source, Value defaultValue)
{
    NodeType picker;
    picker.id = id;
    picker.label = label;
    picker.category = "Archicad";
    picker.description = description;
    picker.executionDomain = ExecutionDomain::Worker;
    picker.effect = EffectKind::Pure;

    ParameterUi ui;
    ui.widget = ParameterWidget::Select;
    ui.section = "Value";
    ui.help = description;
    ui.optionSource = source;

    ParameterSchema parameter { "value", label, valueType, true, std::move (defaultValue) };
    parameter.ui = std::move (ui);
    picker.parameters.push_back (std::move (parameter));
    picker.outputs.push_back ({ "value", label, valueType });
    return picker;
}

} // namespace

NodeRegistry MakeBuiltinNodeRegistry ()
{
    NodeRegistry registry;
    std::string error;

    NodeType number = PureNode ("number", "Number", "Provides a numeric constant.");
    number.outputs.push_back (Port ("value", ValueType::Double));
    // A DEFAULT, so a Number node placed and left alone is zero rather than a
    // node that throws. It is the same rule the rest of the catalog follows -
    // the mini-UI holds the value and the port is for taking that over - and it
    // is what lets the "every node evaluates from its own defaults" test cover
    // this type rather than carve out an exception for it.
    number.parameters.push_back ({ "value", "Value", ValueType::Double, true, Value (0.0) });
    if (!registry.Register (std::move (number), error))
        throw std::logic_error (error);

    for (const auto& [id, label, description] :
         { std::tuple { "add", "Add", "Adds two numbers." },
           std::tuple { "subtract", "Subtract", "Subtracts the right number from the left." },
           std::tuple { "multiply", "Multiply", "Multiplies two numbers." },
           std::tuple { "divide", "Divide", "Divides the left number by the right." } }) {
        NodeType arithmetic = PureNode (id, label, description);
        arithmetic.inputs = { Port ("left", ValueType::Double), Port ("right", ValueType::Double) };
        arithmetic.outputs.push_back (Port ("value", ValueType::Double));
        // Stage F3. BOTH inputs are type-compatible with the output, so the
        // mapping is a DECISION the type makes rather than something anyone
        // could derive: bypassing an Add passes the LEFT operand through. The
        // value of declaring it is that it then means the same thing on every
        // Add in every graph, which an inferred mapping would not.
        arithmetic.bypassMappings.push_back ({ "left", "value" });
        if (!registry.Register (std::move (arithmetic), error))
            throw std::logic_error (error);
    }

    NodeType list = PureNode ("makeList", "Make List", "Collects connected numbers in connection order.");
    list.inputs.push_back (Port ("items", ValueType::Double, true));
    list.outputs.push_back (Port ("value", ValueType::List));
    if (!registry.Register (std::move (list), error))
        throw std::logic_error (error);

    NodeType scale = PureNode ("scaleList", "Scale List", "Multiplies every number in a list.");
    scale.inputs = { Port ("list", ValueType::List), Port ("factor", ValueType::Double) };
    scale.outputs.push_back (Port ("value", ValueType::List));
    scale.bypassMappings.push_back ({ "list", "value" });
    if (!registry.Register (std::move (scale), error))
        throw std::logic_error (error);

    // The Grasshopper-panel equivalent: wire anything into it and read what came
    // out. Its input is declared Absent, which the edit rules read as "any type",
    // so one node inspects every value the runtime has rather than there being a
    // panel per type.
    NodeType panel = PureNode ("panel", "Panel", "Shows whatever is wired into it as readable text.");
    panel.category = "Inspect";
    panel.display = NodeDisplay::Text;
    panel.inputs.push_back ({ "value", "Value", ValueType::Absent, true, false });
    panel.outputs.push_back ({ "text", "Text", ValueType::String });
    panel.outputs.push_back ({ "lines", "Lines", ValueType::List });
    panel.outputs.push_back ({ "count", "Count", ValueType::Integer });
    panel.outputs.push_back ({ "summary", "Summary", ValueType::String });
    if (!registry.Register (std::move (panel), error))
        throw std::logic_error (error);

    // The viewport's end of the graph.
    //
    // ⚠️ PURE, AND THAT IS THE POINT. Showing geometry in the model is plainly a
    // side effect, so the tempting shape is EffectKind::HostUiWrite - and that
    // would be wrong here for a reason worth stating: HostUiWrite nodes are
    // deferred to a second phase and REFUSED unless the request allowed side
    // effects, which only a deliberate Run does. A preview that appears only when
    // you press Run is not a preview. So the node stays a pass-through, and the
    // runtime PROJECTS the results into the preview store afterwards - see
    // NodeGraph/PreviewProjection.hpp for the whole argument.
    //
    // Its input is declared Absent, which the edit rules read as "any type", so
    // one node previews a point, a curve, a mesh and a list of all three.
    NodeType preview =
        PureNode (kPreviewNodeType, "Preview", "Shows whatever is wired into it in the Tapioca 3D viewport.");
    preview.category = "Inspect";
    preview.display = NodeDisplay::Preview;
    preview.inputs.push_back ({ "geometry", "Geometry", ValueType::Absent, true, false });
    // ⚠️ NO OUTPUTS. A Preview is a TERMINAL: it is the end of a branch, not a
    // stage in one. It had a pass-through output only because the projection read
    // its result, and outputs are the only thing the evaluator caches - which put
    // two ports on the node that nobody would ever wire and made "List of 1"
    // appear beside a node whose whole job is to show you a shape. The projection
    // now walks the EDGE INTO the node and reads the upstream node's output
    // instead, which is where the geometry actually is.
    ParameterUi previewEnabledUi;
    previewEnabledUi.widget = ParameterWidget::Boolean;
    previewEnabledUi.section = "Preview";
    previewEnabledUi.order = 0;
    previewEnabledUi.help = "Show this geometry in the viewport. Off leaves the graph running and draws nothing.";
    ParameterSchema previewEnabled { kPreviewEnabledParameter, "Show", ValueType::Bool, false, Value (true) };
    previewEnabled.ui = std::move (previewEnabledUi);
    preview.parameters.push_back (std::move (previewEnabled));
    // ⚠️ ONE CONTROL FOR BOTH HALVES, NOT TWO. The node has its own viewport and
    // the model has an overlay, and a switch for each would let them disagree -
    // "showing nothing" would then have two causes that look identical. The
    // runtime reads this for the overlay; the editor reads the same parameter for
    // the node viewport, dispatching on the widget rather than on the node id.
    ParameterUi previewTargetUi;
    previewTargetUi.widget = ParameterWidget::PreviewTarget;
    previewTargetUi.section = "Preview";
    previewTargetUi.order = 1;
    previewTargetUi.help = "Where this geometry is drawn.";
    previewTargetUi.options = { { "Node", Value (std::string ("node")) },
                                { "Archicad", Value (std::string ("archicad")) },
                                { "Both", Value (std::string ("both")) } };
    ParameterSchema previewTarget { kPreviewTargetParameter, "Draw in", ValueType::String, false,
                                    Value (std::string ("both")) };
    previewTarget.ui = std::move (previewTargetUi);
    preview.parameters.push_back (std::move (previewTarget));
    ParameterUi previewColorUi;
    previewColorUi.widget = ParameterWidget::Color;
    previewColorUi.section = "Preview";
    previewColorUi.order = 2;
    previewColorUi.help = "The colour this node's geometry is drawn in.";
    ParameterSchema previewColor { kPreviewColorParameter, "Colour", ValueType::String, false,
                                   Value (std::string ("#4CA64C")) };
    previewColor.ui = std::move (previewColorUi);
    preview.parameters.push_back (std::move (previewColor));
    ParameterUi previewXRayUi;
    previewXRayUi.widget = ParameterWidget::Boolean;
    previewXRayUi.section = "Preview";
    previewXRayUi.order = 3;
    previewXRayUi.help = "Draw through the model, so geometry inside a wall stays visible.";
    ParameterSchema previewXRay { kPreviewXRayParameter, "X-ray", ValueType::Bool, false, Value (false) };
    previewXRay.ui = std::move (previewXRayUi);
    preview.parameters.push_back (std::move (previewXRay));
    if (!registry.Register (std::move (preview), error))
        throw std::logic_error (error);

    NodeType watch = PureNode ("watch", "Watch", "Reports a list without changing it.");
    watch.display = NodeDisplay::Preview;
    watch.inputs.push_back (Port ("value", ValueType::List));
    watch.outputs.push_back (Port ("value", ValueType::List));
    watch.bypassMappings.push_back ({ "value", "value" });
    if (!registry.Register (std::move (watch), error))
        throw std::logic_error (error);

    // Stage F4's hold-capable type. Holding is a CONTRACT a type opts into
    // rather than a badge every node can wear, so the catalog needs at least one
    // node that means it - without one, ExecutionMode::Holding is unreachable
    // and the release path is untestable.
    //
    // Typed List rather than "any" because an Absent OUTPUT fails the
    // publish-time type check, which reads Absent as a type and not as a
    // wildcard. Damming a bare number therefore wants either a second dam type
    // or a wildcard rule for outputs, and both are catalog decisions outside
    // Stage F.
    // ------------------------------------------------------------------
    // The input library: the values a graph STARTS from.
    //
    // Every one of these is a parameter with a UI descriptor and nothing else -
    // no node-specific execution rule beyond the type's own contract, and no
    // client that has to know their names. That is the test UI-1 exists to
    // pass: adding another input node here must need no change in the editor.
    // ------------------------------------------------------------------

    // The slider's range and precision are ORDINARY PARAMETERS, not fixed
    // catalog metadata, because the user is the one who decides them - and the
    // descriptor says which parameters they are, so the control still needs no
    // branch on nodeType.
    NodeType slider = PureNode ("numberSlider", "Number Slider", "A number you drag, within a range you set.");
    slider.category = "Input";
    ParameterUi sliderUi;
    sliderUi.widget = ParameterWidget::Slider;
    sliderUi.section = "Value";
    sliderUi.order = 0;
    sliderUi.help = "Drag to change the value. The range and the decimal places are set below.";
    sliderUi.minimumParameter = "minimum";
    sliderUi.maximumParameter = "maximum";
    sliderUi.stepParameter = "step";
    sliderUi.decimalsParameter = "decimals";
    sliderUi.minimum = 0.0;
    sliderUi.maximum = 100.0;
    sliderUi.decimals = 2;
    ParameterSchema sliderValue { "value", "Value", ValueType::Double, true, Value (0.0) };
    sliderValue.ui = std::move (sliderUi);
    slider.parameters.push_back (std::move (sliderValue));
    ParameterSchema sliderMin { "minimum", "Minimum", ValueType::Double, false, Value (0.0) };
    sliderMin.ui = NumberUi ("Range", 1, "The lowest value the slider can reach.", 3, "decimals");
    slider.parameters.push_back (std::move (sliderMin));
    ParameterSchema sliderMax { "maximum", "Maximum", ValueType::Double, false, Value (100.0) };
    sliderMax.ui = NumberUi ("Range", 2, "The highest value the slider can reach.", 3, "decimals");
    slider.parameters.push_back (std::move (sliderMax));
    ParameterSchema sliderStep { "step", "Step", ValueType::Double, false, Value (0.1) };
    ParameterUi stepUi = NumberUi ("Range", 3, "How far one nudge of the slider moves the value.", 3, "decimals");
    stepUi.minimum = 0.0;
    sliderStep.ui = std::move (stepUi);
    slider.parameters.push_back (std::move (sliderStep));
    // Integer, and bounded at registration by the widget rules: decimal places
    // are a count, and a count with a fractional part is a defect waiting to be
    // rendered.
    ParameterSchema sliderDecimals { "decimals", "Decimals", ValueType::Integer, false,
                                     Value (static_cast<int64_t> (2)) };
    ParameterUi decimalsUi = NumberUi ("Range", 4, "How many decimal places the value is rounded to.", 0);
    decimalsUi.minimum = 0.0;
    decimalsUi.maximum = 15.0;
    decimalsUi.step = 1.0;
    sliderDecimals.ui = std::move (decimalsUi);
    slider.parameters.push_back (std::move (sliderDecimals));
    slider.outputs.push_back (Port ("value", ValueType::Double));
    if (!registry.Register (std::move (slider), error))
        throw std::logic_error (error);

    NodeType toggle = PureNode ("booleanToggle", "Boolean Toggle", "True or false, as a switch.");
    toggle.category = "Input";
    ParameterUi toggleUi;
    toggleUi.widget = ParameterWidget::Boolean;
    toggleUi.section = "Value";
    toggleUi.help = "Switch the value between true and false.";
    ParameterSchema toggleValue { "value", "Value", ValueType::Bool, true, Value (false) };
    toggleValue.ui = std::move (toggleUi);
    toggle.parameters.push_back (std::move (toggleValue));
    toggle.outputs.push_back (Port ("value", ValueType::Bool));
    if (!registry.Register (std::move (toggle), error))
        throw std::logic_error (error);

    // ------------------------------------------------------------------
    // Archicad attribute pickers. Names, never indices - the same policy the
    // command palette's pickers follow, and for the same reason: an index is
    // not stable across projects and means nothing to the person reading the
    // graph. A Pen is the one exception; a pen IS its number.
    // ------------------------------------------------------------------
    struct PickerSpec {
        const char* id;
        const char* label;
        const char* description;
        ValueType valueType;
        ParameterOptionSource source;
    };
    const PickerSpec pickers[] = {
        { "attribute.layer", "Layer", "A layer from the open project.", ValueType::String,
          ParameterOptionSource::Layer },
        { "attribute.pen", "Pen", "A pen from the project's pen table.", ValueType::Integer,
          ParameterOptionSource::Pen },
        { "attribute.fill", "Fill", "A fill from the open project.", ValueType::String, ParameterOptionSource::Fill },
        { "attribute.lineType", "Line Type", "A line type from the open project.", ValueType::String,
          ParameterOptionSource::LineType },
        { "attribute.surface", "Surface", "A surface from the open project.", ValueType::String,
          ParameterOptionSource::Surface },
        { "attribute.buildingMaterial", "Building Material", "A building material from the open project.",
          ValueType::String, ParameterOptionSource::BuildingMaterial },
        { "attribute.composite", "Composite", "A composite structure - the wall, slab and roof types.",
          ValueType::String, ParameterOptionSource::Composite },
        { "attribute.profile", "Profile", "A complex profile from the open project.", ValueType::String,
          ParameterOptionSource::Profile },
    };
    for (const PickerSpec& spec : pickers) {
        Value defaultValue =
            spec.valueType == ValueType::Integer ? Value (static_cast<int64_t> (1)) : Value (std::string {});
        NodeType picker = AttributePicker (spec.id, spec.label, spec.description, spec.valueType, spec.source,
                                           std::move (defaultValue));
        if (!registry.Register (std::move (picker), error))
            throw std::logic_error (error);
    }

    // ------------------------------------------------------------------
    // Numbers and flow.
    // ------------------------------------------------------------------
    NodeType remap = PureNode ("math.remap", "Remap", "Moves a number from one range into another.");
    remap.category = "Math";
    remap.inputs = { Port ("value", ValueType::Double),     Port ("sourceMin", ValueType::Double),
                     Port ("sourceMax", ValueType::Double), Port ("targetMin", ValueType::Double),
                     Port ("targetMax", ValueType::Double), Port ("clamp", ValueType::Bool) };
    remap.outputs.push_back (Port ("value", ValueType::Double));
    const std::pair<const char*, double> remapFields[] = {
        { "value", 0.5 }, { "sourceMin", 0.0 }, { "sourceMax", 1.0 }, { "targetMin", 0.0 }, { "targetMax", 100.0 },
    };
    int remapOrder = 0;
    for (const auto& [id, defaultValue] : remapFields) {
        ParameterSchema field { id, id, ValueType::Double, false, Value (defaultValue) };
        field.ui = NumberUi ("Remap", remapOrder++, "A remap bound.", 3);
        remap.parameters.push_back (std::move (field));
    }
    ParameterSchema remapClamp { "clamp", "Clamp", ValueType::Bool, false, Value (true) };
    remapClamp.ui = BooleanUi ("Remap", remapOrder, "Keep the result inside the target range.");
    remap.parameters.push_back (std::move (remapClamp));
    if (!registry.Register (std::move (remap), error))
        throw std::logic_error (error);

    NodeType random = PureNode ("math.random", "Random", "A repeatable run of random numbers.");
    random.category = "Math";
    random.inputs = { Port ("seed", ValueType::Integer), Port ("count", ValueType::Integer),
                      Port ("minimum", ValueType::Double), Port ("maximum", ValueType::Double) };
    random.outputs.push_back (Port ("values", ValueType::List));
    // ⚠️ SEEDED, AND THEREFORE REPEATABLE. A node that returned different numbers
    // on every evaluation would make the whole graph unstable: the solution runs
    // continuously now, so an unseeded Random would jitter the model on every
    // keystroke anywhere upstream. The seed is what makes "random" a value rather
    // than an event.
    ParameterSchema randomSeed { "seed", "Seed", ValueType::Integer, false, Value (static_cast<int64_t> (1)) };
    randomSeed.ui =
        CountUi ("Random", 0, "Change this for a different set. The same seed always gives the same set.", 0, 1000000);
    random.parameters.push_back (std::move (randomSeed));
    ParameterSchema randomCount { "count", "Count", ValueType::Integer, false, Value (static_cast<int64_t> (10)) };
    randomCount.ui = CountUi ("Random", 1, "How many numbers.", 1, 100000);
    random.parameters.push_back (std::move (randomCount));
    ParameterSchema randomMin { "minimum", "Minimum", ValueType::Double, false, Value (0.0) };
    randomMin.ui = NumberUi ("Random", 2, "The lowest number it may produce.", 3);
    random.parameters.push_back (std::move (randomMin));
    ParameterSchema randomMax { "maximum", "Maximum", ValueType::Double, false, Value (1.0) };
    randomMax.ui = NumberUi ("Random", 3, "The highest number it may produce.", 3);
    random.parameters.push_back (std::move (randomMax));
    if (!registry.Register (std::move (random), error))
        throw std::logic_error (error);

    // ⚠️ THE INTERMEDIATE STEP THE TYPE RULE REQUIRES, AND THE REASON IT EXISTS.
    //
    // An Integer reaches a Double port on its own, because nothing is lost. A
    // Double reaching an Integer port is refused, and this node is how a graph
    // says what it wants instead. It is not busywork: 2.5 becomes 2 or 3
    // depending on an answer only the author has, and a silent cast would put
    // that answer in the runtime where nobody can see it. Here it is a node on
    // the canvas with its choice written on it.
    NodeType toInteger =
        PureNode ("math.toInteger", "To Integer", "Turns a number into a whole number, the way you choose.");
    toInteger.category = "Math";
    toInteger.inputs.push_back (Port ("value", ValueType::Double));
    toInteger.outputs.push_back (Port ("value", ValueType::Integer));
    ParameterSchema rounding { "mode", "Mode", ValueType::String, false, Value (std::string ("nearest")) };
    ParameterUi roundingUi;
    roundingUi.widget = ParameterWidget::Select;
    roundingUi.section = "To Integer";
    roundingUi.order = 0;
    roundingUi.help = "Which whole number to take.";
    roundingUi.options = { { "Nearest", Value (std::string ("nearest")) },
                           { "Down (floor)", Value (std::string ("floor")) },
                           { "Up (ceiling)", Value (std::string ("ceiling")) },
                           { "Toward zero (truncate)", Value (std::string ("truncate")) } };
    rounding.ui = std::move (roundingUi);
    toInteger.parameters.push_back (std::move (rounding));
    if (!registry.Register (std::move (toInteger), error))
        throw std::logic_error (error);

    NodeType conditional = PureNode ("flow.if", "If", "Passes one of two inputs through, by a condition.");
    conditional.category = "Flow";
    conditional.inputs.push_back ({ "condition", "Condition", ValueType::Bool, false, false });
    conditional.inputs.push_back ({ "ifTrue", "If true", ValueType::Absent, false, false });
    conditional.inputs.push_back ({ "ifFalse", "If false", ValueType::Absent, false, false });
    conditional.outputs.push_back (Port ("value", ValueType::List));
    conditional.outputs.push_back (Port ("taken", ValueType::Bool));
    ParameterSchema conditionParameter { "condition", "Condition", ValueType::Bool, false, Value (true) };
    conditionParameter.ui = BooleanUi ("If", 0, "Which branch is passed through.");
    conditional.parameters.push_back (std::move (conditionParameter));
    if (!registry.Register (std::move (conditional), error))
        throw std::logic_error (error);

    NodeType dam = PureNode ("dataDam", "Data Dam", "Holds its input until you release it.");
    dam.category = "Flow";
    dam.inputs.push_back (Port ("value", ValueType::List));
    dam.outputs.push_back (Port ("value", ValueType::List));
    dam.holdCapable = true;
    dam.bypassMappings.push_back ({ "value", "value" });
    if (!registry.Register (std::move (dam), error))
        throw std::logic_error (error);
    return registry;
}

bool ExecuteBuiltinNode (const Node& node, const ValueMap& inputs, const NodeExecutionContext& context,
                         ValueMap& outputs, std::string& error)
{
    // Pure nodes read nothing outside their inputs; that is what makes them pure.
    (void) context;

    if (node.nodeType == "number")
        outputs.emplace ("value", StoredOr (node, "value", Value (0.0)));
    else if (node.nodeType == "numberSlider") {
        const double minimum = ParameterNumber (node, "minimum", 0.0);
        const double maximum = ParameterNumber (node, "maximum", 100.0);
        const int decimals = static_cast<int> (ParameterNumber (node, "decimals", 2.0));
        outputs.emplace ("value",
                         Value (ClampAndRound (ParameterNumber (node, "value", 0.0), minimum, maximum, decimals)));
    }
    else if (node.nodeType == "booleanToggle")
        outputs.emplace ("value", StoredOr (node, "value", Value (false)));
    else if (node.nodeType.rfind ("attribute.", 0) == 0)
        // Every picker answers with what the user picked. The list it was picked
        // FROM comes from Archicad; the pick itself is the graph's own state.
        outputs.emplace ("value", StoredOr (node, "value", Value (std::string {})));
    else if (node.nodeType == "add")
        outputs.emplace ("value", Value (Number (inputs.at ("left")) + Number (inputs.at ("right"))));
    else if (node.nodeType == "multiply")
        outputs.emplace ("value", Value (Number (inputs.at ("left")) * Number (inputs.at ("right"))));
    else if (node.nodeType == "makeList")
        outputs.emplace ("value", inputs.at ("items"));
    else if (node.nodeType == "scaleList") {
        std::vector<Value> scaled;
        const double factor = Number (inputs.at ("factor"));
        for (const Value& item : inputs.at ("list").Items ())
            scaled.emplace_back (Number (item) * factor);
        outputs.emplace ("value", Argument::FromItems (std::move (scaled)));
    }
    else if (node.nodeType == "subtract")
        outputs.emplace ("value",
                         Value (ScalarFrom (inputs, node, "left", 0.0) - ScalarFrom (inputs, node, "right", 0.0)));
    else if (node.nodeType == "divide") {
        const double right = ScalarFrom (inputs, node, "right", 1.0);
        // Refused, not infinity. An infinity propagates silently through every
        // downstream node and surfaces as geometry somewhere off in space; the
        // node that produced it is then the last place anyone looks.
        if (right == 0.0) {
            error = "cannot divide by zero";
            return false;
        }
        outputs.emplace ("value", Value (ScalarFrom (inputs, node, "left", 0.0) / right));
    }
    else if (node.nodeType == "math.remap") {
        const double sourceMin = ScalarFrom (inputs, node, "sourceMin", 0.0);
        const double sourceMax = ScalarFrom (inputs, node, "sourceMax", 1.0);
        if (sourceMin == sourceMax) {
            // A source range of zero width has no answer: every input maps to
            // the whole target range at once.
            error = "the source range is empty - its minimum and maximum are the same";
            return false;
        }
        const double targetMin = ScalarFrom (inputs, node, "targetMin", 0.0);
        const double targetMax = ScalarFrom (inputs, node, "targetMax", 100.0);
        const double value = ScalarFrom (inputs, node, "value", 0.0);
        double fraction = (value - sourceMin) / (sourceMax - sourceMin);
        if (BoolFrom (inputs, node, "clamp", true))
            fraction = std::min (std::max (fraction, 0.0), 1.0);
        outputs.emplace ("value", Value (targetMin + fraction * (targetMax - targetMin)));
    }
    else if (node.nodeType == "math.random") {
        const int64_t count = static_cast<int64_t> (ScalarFrom (inputs, node, "count", 10.0));
        if (count < 1 || count > 100000) {
            error = "Random produces between 1 and 100000 numbers";
            return false;
        }
        const double minimum = ScalarFrom (inputs, node, "minimum", 0.0);
        const double maximum = ScalarFrom (inputs, node, "maximum", 1.0);
        // ⚠️ SEEDED AND SELF-CONTAINED - no global generator, no clock. The
        // solution runs continuously now, so a Random that answered differently
        // each evaluation would jitter the model on every keystroke anywhere
        // upstream, and nothing on screen would say why. splitmix64 is used
        // rather than <random> because its sequence is fixed by the standard's
        // arithmetic rather than by an implementation's engine, so the same seed
        // gives the same numbers on every machine and every build.
        uint64_t state = static_cast<uint64_t> (ScalarFrom (inputs, node, "seed", 1.0));
        std::vector<Value> values;
        values.reserve (static_cast<size_t> (count));
        for (int64_t index = 0; index < count; ++index) {
            state += 0x9E3779B97F4A7C15ull;
            uint64_t z = state;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            z = z ^ (z >> 31);
            // 53 bits, which is every bit a double can hold exactly.
            const double unit = static_cast<double> (z >> 11) / 9007199254740992.0;
            values.emplace_back (minimum + unit * (maximum - minimum));
        }
        outputs.emplace ("values", Argument::FromItems (std::move (values)));
    }
    else if (node.nodeType == "math.toInteger") {
        const double value = ScalarFrom (inputs, node, "value", 0.0);
        const auto chosen = node.parameters.find ("mode");
        const std::string* mode =
            chosen == node.parameters.end () ? nullptr : std::get_if<std::string> (&chosen->second.DataValue ());
        const std::string name = mode == nullptr ? "nearest" : *mode;
        // std::llround rather than a cast for "nearest", because a cast toward
        // zero is what "truncate" means and having two of the four modes do the
        // same thing would make the choice a lie.
        double whole = std::llround (value);
        if (name == "floor")
            whole = std::floor (value);
        else if (name == "ceiling")
            whole = std::ceil (value);
        else if (name == "truncate")
            whole = std::trunc (value);
        outputs.emplace ("value", Value (static_cast<int64_t> (whole)));
    }
    else if (node.nodeType == "flow.if") {
        const bool condition = BoolFrom (inputs, node, "condition", true);
        const auto branch = inputs.find (condition ? "ifTrue" : "ifFalse");
        Argument taken = branch == inputs.end () ? Argument {} : branch->second;
        outputs.emplace ("value", taken.Type () == ValueType::List ? std::move (taken)
                                                                   : Argument::FromItems ({ taken.AsValue () }));
        outputs.emplace ("taken", Value (condition));
    }
    else if (node.nodeType == kPreviewNodeType) {
        // Nothing. The node produces no value; what makes the geometry appear is
        // NodeGraph/PreviewProjection following the edge into this node after the
        // run. Executing it is still what marks it reached, which is how a
        // preview downstream of a disabled branch stops drawing.
    }
    else if (node.nodeType == "panel") {
        const Argument& value = inputs.at ("value");
        const std::vector<std::string> lines = FormatValueLines (value);
        std::vector<Value> lineValues;
        lineValues.reserve (lines.size ());
        std::string joined;
        for (size_t i = 0; i < lines.size (); ++i) {
            lineValues.emplace_back (lines[i]);
            if (i != 0)
                joined += "\n";
            joined += lines[i];
        }
        // Both shapes, because a client should not have to split a string to
        // render a list, nor join a list to show one line.
        outputs.emplace ("text", Value (joined));
        outputs.emplace ("lines", Argument::FromItems (std::move (lineValues)));
        outputs.emplace ("count",
                         Value (static_cast<int64_t> (value.Type () == ValueType::List ? value.Items ().size () : 1)));
        outputs.emplace ("summary", Value (DescribeValue (value)));
    }
    else if (node.nodeType == "watch" || node.nodeType == "dataDam")
        // A dam computes nothing; what makes it a dam is that Holding stages
        // this result instead of publishing it. See Evaluator::PublishNode.
        outputs.emplace ("value", inputs.at ("value"));
    else {
        error = "unknown built-in node type: " + node.nodeType;
        return false;
    }
    error.clear ();
    return true;
}

} // namespace evp::nodegraph
