#include "NodeGraph/BuiltinNodes.hpp"

#include "Geometry/GeometryEngine.hpp"
#include "NodeGraph/ValueText.hpp"

#include <algorithm>
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

double Number (const Value& value)
{
    return std::get<double> (value.DataValue ());
}

// A number wherever it came from. A slider's bound is authored as a Double and
// its decimals as an Integer, and both reach ExecuteBuiltinNode as parameters -
// so the one place that reads them must accept either rather than throwing a
// bad_variant_access at the user.
double AnyNumber (const Value& value, double fallback)
{
    if (value.Type () == ValueType::Double)
        return std::get<double> (value.DataValue ());
    if (value.Type () == ValueType::Integer)
        return static_cast<double> (std::get<int64_t> (value.DataValue ()));
    return fallback;
}

double ParameterNumber (const Node& node, const char* id, double fallback)
{
    const auto found = node.parameters.find (id);
    return found == node.parameters.end () ? fallback : AnyNumber (found->second, fallback);
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

double ScalarFrom (const ValueMap& inputs, const Node& node, const char* id, double fallback)
{
    const auto wired = inputs.find (id);
    if (wired != inputs.end () && wired->second.Type () == ValueType::Double)
        return std::get<double> (wired->second.DataValue ());
    const auto stored = node.parameters.find (id);
    if (stored != node.parameters.end () && stored->second.Type () == ValueType::Double)
        return std::get<double> (stored->second.DataValue ());
    return fallback;
}

Point3 ComponentsFrom (const ValueMap& inputs, const Node& node, double defaultZ)
{
    return { ScalarFrom (inputs, node, "x", 0.0), ScalarFrom (inputs, node, "y", 0.0),
             ScalarFrom (inputs, node, "z", defaultZ) };
}

geomsrv::engine::Vector3 ToEngineVector (const Point3& value)
{
    return { value.x, value.y, value.z };
}

Point3 FromEngineVector (const geomsrv::engine::Vector3& value)
{
    return { value.x, value.y, value.z };
}

geomsrv::engine::Polygon ToEnginePolygon (const Polygon& value)
{
    geomsrv::engine::Polygon result;
    result.points.reserve (value.points.size ());
    for (const Point3& point : value.points)
        result.points.push_back (ToEngineVector (point));
    return result;
}

Value PolygonListValue (const std::vector<geomsrv::engine::Polygon>& polygons)
{
    Value::List values;
    values.reserve (polygons.size ());
    for (const geomsrv::engine::Polygon& source : polygons) {
        Polygon polygon;
        polygon.points.reserve (source.points.size ());
        for (const geomsrv::engine::Vector3& point : source.points)
            polygon.points.push_back (FromEngineVector (point));
        values.emplace_back (std::move (polygon));
    }
    return Value (std::move (values));
}

// The stored parameter, verbatim, for the nodes whose whole job is to hold one.
Value StoredOr (const Node& node, const char* id, Value fallback)
{
    const auto found = node.parameters.find (id);
    if (found == node.parameters.end () || found->second.Type () == ValueType::Absent)
        return fallback;
    return found->second;
}

ParameterUi NumberUi (const char* section, int order, const char* help, int decimals,
                      const char* decimalsFrom = nullptr)
{
    ParameterUi ui;
    ui.widget = ParameterWidget::Number;
    ui.section = section;
    ui.order = order;
    ui.help = help;
    ui.decimals = decimals;
    // A sibling that governs the precision, when one does. The slider's range
    // fields follow the SAME decimals setting the value does: a slider showing
    // two decimals whose minimum box shows three is telling the user its range
    // is finer than its value can express, and the step it nudges by has to
    // agree with both or the arrows land on numbers the field then rounds away.
    if (decimalsFrom != nullptr)
        ui.decimalsParameter = decimalsFrom;
    return ui;
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
    number.parameters.push_back ({ "value", "Value", ValueType::Double, true });
    if (!registry.Register (std::move (number), error))
        throw std::logic_error (error);

    for (const auto& [id, label, description] : { std::tuple { "add", "Add", "Adds two numbers." },
                                                  std::tuple { "multiply", "Multiply", "Multiplies two numbers." } }) {
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
    // Geometry. Coordinates are separate scalar inputs so each component can be
    // wired or referenced independently; the node assembles the Point3 output.
    // ------------------------------------------------------------------
    NodeType point = PureNode ("point", "Point", "A position in space.");
    point.category = "Geometry";
    for (const auto& [id, label] : { std::pair { "x", "X" }, std::pair { "y", "Y" }, std::pair { "z", "Z" } }) {
        point.inputs.push_back ({ id, label, ValueType::Double, false, false });
        ParameterSchema coordinate { id, label, ValueType::Double, false, Value (0.0) };
        coordinate.ui = NumberUi ("Value", static_cast<int> (point.parameters.size ()), "A point coordinate.", 2);
        coordinate.ui->unit = "mm";
        point.parameters.push_back (std::move (coordinate));
    }
    point.outputs.push_back (Port ("point", ValueType::Point3));
    if (!registry.Register (std::move (point), error))
        throw std::logic_error (error);

    // A direction and a length, not a position - which is why it carries no
    // unit and offers no pick-in-model affordance.
    NodeType vector = PureNode ("vector", "Vector", "A direction and a length.");
    vector.category = "Geometry";
    for (const auto& [id, label, defaultValue] :
         { std::tuple { "x", "X", 0.0 }, std::tuple { "y", "Y", 0.0 }, std::tuple { "z", "Z", 1.0 } }) {
        vector.inputs.push_back ({ id, label, ValueType::Double, false, false });
        ParameterSchema component { id, label, ValueType::Double, false, Value (defaultValue) };
        component.ui = NumberUi ("Value", static_cast<int> (vector.parameters.size ()), "A vector component.", 3);
        vector.parameters.push_back (std::move (component));
    }
    vector.outputs.push_back (Port ("vector", ValueType::Point3));
    vector.outputs.push_back (Port ("length", ValueType::Double));
    if (!registry.Register (std::move (vector), error))
        throw std::logic_error (error);

    for (const auto& [id, label, description] :
         { std::tuple { "geom.vectorAdd", "Vector Add", "Adds two vectors." },
           std::tuple { "geom.vectorCross", "Vector Cross", "Returns the cross product of two vectors." } }) {
        NodeType operation = PureNode (id, label, description);
        operation.category = "Geometry";
        operation.inputs = { Port ("left", ValueType::Point3), Port ("right", ValueType::Point3) };
        operation.outputs.push_back (Port ("vector", ValueType::Point3));
        if (!registry.Register (std::move (operation), error))
            throw std::logic_error (error);
    }

    NodeType vectorDot = PureNode ("geom.vectorDot", "Vector Dot", "Returns the dot product of two vectors.");
    vectorDot.category = "Geometry";
    vectorDot.inputs = { Port ("left", ValueType::Point3), Port ("right", ValueType::Point3) };
    vectorDot.outputs.push_back (Port ("value", ValueType::Double));
    if (!registry.Register (std::move (vectorDot), error))
        throw std::logic_error (error);

    NodeType vectorUnit = PureNode ("geom.vectorUnit", "Unit Vector", "Normalizes a vector to unit length.");
    vectorUnit.category = "Geometry";
    vectorUnit.inputs.push_back (Port ("vector", ValueType::Point3));
    vectorUnit.outputs.push_back (Port ("vector", ValueType::Point3));
    vectorUnit.bypassMappings.push_back ({ "vector", "vector" });
    if (!registry.Register (std::move (vectorUnit), error))
        throw std::logic_error (error);

    NodeType polygon = PureNode ("geom.polygon", "Polygon", "Builds a planar polygon from connected points.");
    polygon.category = "Geometry";
    polygon.inputs.push_back (Port ("points", ValueType::Point3, true));
    polygon.outputs.push_back (Port ("polygon", ValueType::Polygon));
    if (!registry.Register (std::move (polygon), error))
        throw std::logic_error (error);

    for (const auto& [id, label, description] :
         { std::tuple { "geom.polygonUnion", "Polygon Union", "Unites two coplanar polygons." },
           std::tuple { "geom.polygonDifference", "Polygon Difference", "Subtracts the clip polygon." },
           std::tuple { "geom.polygonIntersection", "Polygon Intersection", "Intersects two coplanar polygons." } }) {
        NodeType operation = PureNode (id, label, description);
        operation.category = "Geometry";
        operation.inputs = { Port ("subject", ValueType::Polygon), Port ("clip", ValueType::Polygon) };
        operation.outputs.push_back (Port ("polygons", ValueType::List));
        if (!registry.Register (std::move (operation), error))
            throw std::logic_error (error);
    }

    NodeType polygonOffset =
        PureNode ("geom.polygonOffset", "Polygon Offset", "Offsets a planar polygon with mitered joins.");
    polygonOffset.category = "Geometry";
    polygonOffset.inputs = { Port ("polygon", ValueType::Polygon), Port ("distance", ValueType::Double) };
    polygonOffset.outputs.push_back (Port ("polygons", ValueType::List));
    if (!registry.Register (std::move (polygonOffset), error))
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
        outputs.emplace ("value", node.parameters.at ("value"));
    else if (node.nodeType == "numberSlider") {
        const double minimum = ParameterNumber (node, "minimum", 0.0);
        const double maximum = ParameterNumber (node, "maximum", 100.0);
        const int decimals = static_cast<int> (ParameterNumber (node, "decimals", 2.0));
        outputs.emplace ("value",
                         Value (ClampAndRound (ParameterNumber (node, "value", 0.0), minimum, maximum, decimals)));
    }
    else if (node.nodeType == "booleanToggle")
        outputs.emplace ("value", StoredOr (node, "value", Value (false)));
    else if (node.nodeType == "point")
        outputs.emplace ("point", Value (ComponentsFrom (inputs, node, 0.0)));
    else if (node.nodeType == "vector") {
        const Point3 vector = ComponentsFrom (inputs, node, 1.0);
        outputs.emplace ("vector", Value (vector));
        outputs.emplace ("length", Value (std::sqrt (vector.x * vector.x + vector.y * vector.y + vector.z * vector.z)));
    }
    else if (node.nodeType == "geom.vectorAdd" || node.nodeType == "geom.vectorCross") {
        const auto left = ToEngineVector (std::get<Point3> (inputs.at ("left").DataValue ()));
        const auto right = ToEngineVector (std::get<Point3> (inputs.at ("right").DataValue ()));
        const geomsrv::engine::Vector3 result = node.nodeType == "geom.vectorAdd"
                                                    ? geomsrv::engine::Add (left, right)
                                                    : geomsrv::engine::Cross (left, right);
        outputs.emplace ("vector", Value (FromEngineVector (result)));
    }
    else if (node.nodeType == "geom.vectorDot") {
        const auto left = ToEngineVector (std::get<Point3> (inputs.at ("left").DataValue ()));
        const auto right = ToEngineVector (std::get<Point3> (inputs.at ("right").DataValue ()));
        outputs.emplace ("value", Value (geomsrv::engine::Dot (left, right)));
    }
    else if (node.nodeType == "geom.vectorUnit") {
        geomsrv::engine::Vector3 result;
        if (!geomsrv::engine::Unit (ToEngineVector (std::get<Point3> (inputs.at ("vector").DataValue ())), result,
                                    error))
            return false;
        outputs.emplace ("vector", Value (FromEngineVector (result)));
    }
    else if (node.nodeType == "geom.polygon") {
        Polygon polygon;
        for (const Value& item : std::get<Value::List> (inputs.at ("points").DataValue ()))
            polygon.points.push_back (std::get<Point3> (item.DataValue ()));
        if (polygon.points.size () < 3) {
            error = "polygon requires at least three points";
            return false;
        }
        outputs.emplace ("polygon", Value (std::move (polygon)));
    }
    else if (node.nodeType == "geom.polygonUnion" || node.nodeType == "geom.polygonDifference" ||
             node.nodeType == "geom.polygonIntersection") {
        geomsrv::engine::PolygonOperation operation = geomsrv::engine::PolygonOperation::Union;
        if (node.nodeType == "geom.polygonDifference")
            operation = geomsrv::engine::PolygonOperation::Difference;
        else if (node.nodeType == "geom.polygonIntersection")
            operation = geomsrv::engine::PolygonOperation::Intersection;
        std::vector<geomsrv::engine::Polygon> result;
        if (!geomsrv::engine::BooleanPolygons (ToEnginePolygon (std::get<Polygon> (inputs.at ("subject").DataValue ())),
                                               ToEnginePolygon (std::get<Polygon> (inputs.at ("clip").DataValue ())),
                                               operation, result, error))
            return false;
        outputs.emplace ("polygons", PolygonListValue (result));
    }
    else if (node.nodeType == "geom.polygonOffset") {
        std::vector<geomsrv::engine::Polygon> result;
        if (!geomsrv::engine::OffsetPolygon (ToEnginePolygon (std::get<Polygon> (inputs.at ("polygon").DataValue ())),
                                             Number (inputs.at ("distance")), result, error))
            return false;
        outputs.emplace ("polygons", PolygonListValue (result));
    }
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
        Value::List scaled;
        const double factor = Number (inputs.at ("factor"));
        for (const Value& item : std::get<Value::List> (inputs.at ("list").DataValue ()))
            scaled.emplace_back (Number (item) * factor);
        outputs.emplace ("value", Value (std::move (scaled)));
    }
    else if (node.nodeType == "panel") {
        const Value& value = inputs.at ("value");
        const std::vector<std::string> lines = FormatValueLines (value);
        Value::List lineValues;
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
        outputs.emplace ("lines", Value (std::move (lineValues)));
        outputs.emplace ("count", Value (static_cast<int64_t> (value.Type () == ValueType::List
                                                                   ? std::get<Value::List> (value.DataValue ()).size ()
                                                                   : 1)));
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
