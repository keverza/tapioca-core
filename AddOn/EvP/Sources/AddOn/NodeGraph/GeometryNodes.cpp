#include "NodeGraph/GeometryNodes.hpp"

#include "Geometry/Curves.hpp"
#include "Geometry/Primitives.hpp"
#include "Geometry/Transforms.hpp"
#include "NodeGraph/NodeInputs.hpp"
#include "NodeGraph/ParameterDescriptors.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace evp::nodegraph {
namespace {

PortSchema Port (const char* id, ValueType valueType, bool multiple = false)
{
    return { id, id, valueType, true, multiple };
}

// Named PureNode to match the other families: every type in this file is Pure
// and runs on a worker, and a second spelling of the same idea is how two files
// end up disagreeing about what the default category is.
NodeType PureNode (const char* id, const char* label, const char* description)
{
    return { id, label, "Geometry", description };
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

Argument PolygonListValue (const std::vector<geomsrv::engine::Polygon>& polygons)
{
    std::vector<Value> values;
    values.reserve (polygons.size ());
    for (const geomsrv::engine::Polygon& source : polygons) {
        Polygon polygon;
        polygon.points.reserve (source.points.size ());
        for (const geomsrv::engine::Vector3& point : source.points)
            polygon.points.push_back (FromEngineVector (point));
        values.emplace_back (std::move (polygon));
    }
    return Argument::FromItems (std::move (values));
}

// Every point in a value, as engine vectors, whatever shape the value is.
//
// Used by the curve nodes, which accept a list of points, a polyline or a
// polygon interchangeably - a curve is a curve, and making the user convert
// between three spellings of "a run of points" is busywork the runtime can do.
void CollectPoints (const Value& value, std::vector<geomsrv::engine::Vector3>& out)
{
    switch (value.Type ()) {
        case ValueType::Point3:
            out.push_back (ToEngineVector (std::get<Point3> (value.DataValue ())));
            break;
        case ValueType::Polyline:
            for (const Point3& point : std::get<Polyline> (value.DataValue ()).points)
                out.push_back (ToEngineVector (point));
            break;
        case ValueType::Polygon:
            for (const Point3& point : std::get<Polygon> (value.DataValue ()).points)
                out.push_back (ToEngineVector (point));
            break;
        default:
            break;
    }
}

// The List-typed branch a port hands the body, or a scalar argument - unwraps
// the branch and recurses per item, which is guaranteed a scalar Value.
void CollectPoints (const Argument& value, std::vector<geomsrv::engine::Vector3>& out)
{
    if (value.Type () == ValueType::List) {
        for (const Value& item : value.Items ())
            CollectPoints (item, out);
        return;
    }
    CollectPoints (value.AsValue (), out);
}

std::vector<geomsrv::engine::Vector3> PointsFrom (const ValueMap& inputs, const Node& node, const char* id)
{
    std::vector<geomsrv::engine::Vector3> points;
    const auto wired = inputs.find (id);
    if (wired != inputs.end ())
        CollectPoints (wired->second, points);
    if (points.empty ()) {
        const auto stored = node.parameters.find (id);
        if (stored != node.parameters.end ())
            CollectPoints (stored->second, points);
    }
    return points;
}

// The three coordinates of a point or a vector, each wired or typed in on its
// own. Separate scalars rather than one Point3 parameter, because that is what
// lets a single coordinate be driven from upstream while the other two stay as
// the user set them.
Point3 ComponentsFrom (const ValueMap& inputs, const Node& node, double defaultZ)
{
    return { ScalarFrom (inputs, node, "x", 0.0), ScalarFrom (inputs, node, "y", 0.0),
             ScalarFrom (inputs, node, "z", defaultZ) };
}

// A Double, and only a Double. The polygon operations declare their operands as
// Double ports, so anything else reaching here is a graph the edit rules already
// refused.
double Number (const Argument& value)
{
    return std::get<double> (value.DataValue ());
}

Argument PointListValue (const std::vector<geomsrv::engine::Vector3>& points)
{
    std::vector<Value> values;
    values.reserve (points.size ());
    for (const geomsrv::engine::Vector3& point : points)
        values.emplace_back (FromEngineVector (point));
    return Argument::FromItems (std::move (values));
}

Value PolylineValueFrom (const std::vector<geomsrv::engine::Vector3>& points)
{
    Polyline polyline;
    polyline.points.reserve (points.size ());
    for (const geomsrv::engine::Vector3& point : points)
        polyline.points.push_back (FromEngineVector (point));
    return Value (std::move (polyline));
}

// ⚠️ ONE TRANSFORM APPLIER FOR EVERY GEOMETRY TYPE, AND FOR THE ARRAYS. Move,
// Rotate, Scale, Mirror and both Array nodes differ only in the matrix; writing
// each against points, polylines, polygons, meshes and lists separately would be
// thirty implementations of one idea, and the ones nobody exercised with a mesh
// would be the broken ones.
//
// ⚠️ A MESH TAKES THREE DIFFERENT TREATMENTS AT ONCE: positions transform,
// normals take the inverse transpose, and a mirroring transform must have its
// triangle winding REVERSED. Skipping the last leaves a reflected solid
// inside-out - correct-looking until something culls back faces, then half of it
// disappears.
Value TransformValue (const Value& value, const geomsrv::engine::Transform& transform)
{
    switch (value.Type ()) {
        case ValueType::Point3:
            return Value (FromEngineVector (
                geomsrv::engine::ApplyToPoint (transform, ToEngineVector (std::get<Point3> (value.DataValue ())))));

        case ValueType::Polyline: {
            Polyline moved;
            for (const Point3& point : std::get<Polyline> (value.DataValue ()).points)
                moved.points.push_back (
                    FromEngineVector (geomsrv::engine::ApplyToPoint (transform, ToEngineVector (point))));
            return Value (std::move (moved));
        }

        case ValueType::Polygon: {
            Polygon moved;
            for (const Point3& point : std::get<Polygon> (value.DataValue ()).points)
                moved.points.push_back (
                    FromEngineVector (geomsrv::engine::ApplyToPoint (transform, ToEngineVector (point))));
            return Value (std::move (moved));
        }

        case ValueType::Mesh: {
            const auto& source = std::get<Value::ImmutableMesh> (value.DataValue ());
            if (source == nullptr)
                return value;
            auto moved = std::make_shared<geomsrv::Mesh> (*source);
            for (std::size_t index = 0; index + 2 < moved->vertices.size (); index += 3) {
                const geomsrv::engine::Vector3 point = geomsrv::engine::ApplyToPoint (
                    transform, { moved->vertices[index], moved->vertices[index + 1], moved->vertices[index + 2] });
                moved->vertices[index] = point.x;
                moved->vertices[index + 1] = point.y;
                moved->vertices[index + 2] = point.z;
            }
            for (std::size_t index = 0; index + 2 < moved->normals.size (); index += 3) {
                geomsrv::engine::Vector3 normal = geomsrv::engine::ApplyToNormal (
                    transform, { moved->normals[index], moved->normals[index + 1], moved->normals[index + 2] });
                std::string ignored;
                geomsrv::engine::Vector3 unit;
                if (geomsrv::engine::Unit (normal, unit, ignored))
                    normal = unit;
                moved->normals[index] = static_cast<float> (normal.x);
                moved->normals[index + 1] = static_cast<float> (normal.y);
                moved->normals[index + 2] = static_cast<float> (normal.z);
            }
            if (geomsrv::engine::FlipsOrientation (transform)) {
                for (std::size_t index = 0; index + 2 < moved->triangles.size (); index += 3)
                    std::swap (moved->triangles[index + 1], moved->triangles[index + 2]);
            }
            moved->bounds = geomsrv::Aabb {};
            for (std::size_t index = 0; index + 2 < moved->vertices.size (); index += 3)
                moved->bounds.Expand (moved->vertices[index], moved->vertices[index + 1], moved->vertices[index + 2]);
            return Value (std::static_pointer_cast<const geomsrv::Mesh> (moved));
        }

        case ValueType::List:
            // Unreachable: a Value can no longer carry a List branch itself -
            // see TransformArgument, which handles the branch before an item
            // ever reaches here.
            return value;

        default:
            // A number, a string, an element reference: carried through
            // unchanged rather than dropped, so a list of mixed content survives
            // a Move with only its geometry affected.
            return value;
    }
}

// The branch a List-typed or wildcard port hands the body, transformed item by
// item; a scalar argument is transformed directly.
Argument TransformArgument (const Argument& value, const geomsrv::engine::Transform& transform)
{
    if (value.Type () != ValueType::List)
        return TransformValue (value.AsValue (), transform);
    std::vector<Value> moved;
    moved.reserve (value.Items ().size ());
    for (const Value& item : value.Items ())
        moved.push_back (TransformValue (item, transform));
    return Argument::FromItems (std::move (moved));
}

// A transform node's answer: always a list, because an Absent output cannot be
// wired on and the input may itself have been many things.
Argument TransformedList (const Argument& value, const geomsrv::engine::Transform& transform)
{
    Argument moved = TransformArgument (value, transform);
    if (moved.Type () == ValueType::List)
        return moved;
    return Argument::FromItems ({ moved.AsValue () });
}

} // namespace

void RegisterGeometryNodes (NodeRegistry& registry)
{
    std::string error;

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

    // ------------------------------------------------------------------
    // Solids.
    //
    // The first shapes in the catalog, and the first outputs with VOLUME. Points
    // and polylines say where and how far; nothing said "this much space", and a
    // graph whose only visible result is a wire diagram cannot be judged against
    // a building. They are also what makes the Preview node worth having - a
    // mesh is the case its flat shading, its normals and its ceilings exist for.
    //
    // Centred rather than corner-anchored, both of them: a sphere has no corner,
    // and a catalog where the point you give it means the middle of one shape and
    // the corner of another is one people get wrong once each.
    // ------------------------------------------------------------------
    NodeType box = PureNode ("geom.box", "Box", "A rectangular solid, centred on a point.");
    box.category = "Geometry";
    box.inputs = { Port ("centre", ValueType::Point3), Port ("width", ValueType::Double),
                   Port ("depth", ValueType::Double), Port ("height", ValueType::Double) };
    box.outputs.push_back (Port ("mesh", ValueType::Mesh));
    ParameterSchema boxCentre { "centre", "Centre", ValueType::Point3, false, Value (Point3 { 0.0, 0.0, 0.0 }) };
    boxCentre.ui = PointUi ("Box", 0, "Where the middle of the box sits.");
    box.parameters.push_back (std::move (boxCentre));
    ParameterSchema boxWidth { "width", "Width", ValueType::Double, false, Value (1.0) };
    boxWidth.ui = LengthUi ("Box", 1, "Size along X.");
    box.parameters.push_back (std::move (boxWidth));
    ParameterSchema boxDepth { "depth", "Depth", ValueType::Double, false, Value (1.0) };
    boxDepth.ui = LengthUi ("Box", 2, "Size along Y.");
    box.parameters.push_back (std::move (boxDepth));
    ParameterSchema boxHeight { "height", "Height", ValueType::Double, false, Value (1.0) };
    boxHeight.ui = LengthUi ("Box", 3, "Size along Z.");
    box.parameters.push_back (std::move (boxHeight));
    if (!registry.Register (std::move (box), error))
        throw std::logic_error (error);

    NodeType sphere = PureNode ("geom.sphere", "Sphere", "A sphere, centred on a point.");
    sphere.category = "Geometry";
    sphere.inputs = { Port ("centre", ValueType::Point3), Port ("radius", ValueType::Double),
                      Port ("segments", ValueType::Integer) };
    sphere.outputs.push_back (Port ("mesh", ValueType::Mesh));
    ParameterSchema sphereCentre { "centre", "Centre", ValueType::Point3, false, Value (Point3 { 0.0, 0.0, 0.0 }) };
    sphereCentre.ui = PointUi ("Sphere", 0, "Where the middle of the sphere sits.");
    sphere.parameters.push_back (std::move (sphereCentre));
    ParameterSchema sphereRadius { "radius", "Radius", ValueType::Double, false, Value (0.5) };
    sphereRadius.ui = LengthUi ("Sphere", 1, "Distance from the centre to the surface.");
    sphere.parameters.push_back (std::move (sphereRadius));
    // ONE smoothness number, not rings and bands: what a user wants is
    // "smoother", and two numbers is two ways to make an ellipsoid by accident.
    ParameterSchema sphereSegments { "segments", "Segments", ValueType::Integer, false,
                                     Value (static_cast<int64_t> (24)) };
    ParameterUi segmentsUi;
    segmentsUi.widget = ParameterWidget::Slider;
    segmentsUi.section = "Sphere";
    segmentsUi.order = 2;
    segmentsUi.help = "How smooth the surface is. Higher costs more triangles.";
    segmentsUi.minimum = static_cast<double> (geomsrv::engine::kMinSphereSegments);
    segmentsUi.maximum = static_cast<double> (geomsrv::engine::kMaxSphereSegments);
    segmentsUi.step = 1.0;
    segmentsUi.decimals = 0;
    sphereSegments.ui = std::move (segmentsUi);
    sphere.parameters.push_back (std::move (sphereSegments));
    if (!registry.Register (std::move (sphere), error))
        throw std::logic_error (error);

    // ------------------------------------------------------------------
    // Curves. There is no curve TYPE: an arc is tessellated into a polyline
    // where it is made, so everything downstream sees one kind of thing. See
    // Geometry/Curves.hpp for what that buys and what it costs.
    // ------------------------------------------------------------------
    NodeType line = PureNode ("geom.line", "Line", "A straight line between two points.");
    line.category = "Geometry";
    line.inputs = { Port ("start", ValueType::Point3), Port ("end", ValueType::Point3) };
    line.outputs.push_back (Port ("curve", ValueType::Polyline));
    line.outputs.push_back (Port ("length", ValueType::Double));
    ParameterSchema lineStart { "start", "Start", ValueType::Point3, false, Value (Point3 { 0.0, 0.0, 0.0 }) };
    lineStart.ui = PointUi ("Line", 0, "Where the line begins.");
    line.parameters.push_back (std::move (lineStart));
    ParameterSchema lineEnd { "end", "End", ValueType::Point3, false, Value (Point3 { 1.0, 0.0, 0.0 }) };
    lineEnd.ui = PointUi ("Line", 1, "Where the line ends.");
    line.parameters.push_back (std::move (lineEnd));
    if (!registry.Register (std::move (line), error))
        throw std::logic_error (error);

    NodeType polyline = PureNode ("geom.polyline", "Polyline", "A run of points joined into a curve.");
    polyline.category = "Geometry";
    // Absent, which the edit rules read as "any type", so a list of points, a
    // polyline or a polygon all wire in - they are three spellings of one thing.
    polyline.inputs.push_back ({ "points", "Points", ValueType::Absent, true, false });
    polyline.outputs.push_back (Port ("curve", ValueType::Polyline));
    polyline.outputs.push_back (Port ("length", ValueType::Double));
    if (!registry.Register (std::move (polyline), error))
        throw std::logic_error (error);

    NodeType arc = PureNode ("geom.arc", "Arc", "An arc, drawn as a polyline.");
    arc.category = "Geometry";
    arc.inputs = { Port ("centre", ValueType::Point3), Port ("normal", ValueType::Point3),
                   Port ("radius", ValueType::Double), Port ("start", ValueType::Double),
                   Port ("sweep", ValueType::Double),  Port ("segments", ValueType::Integer) };
    arc.outputs.push_back (Port ("curve", ValueType::Polyline));
    ParameterSchema arcCentre { "centre", "Centre", ValueType::Point3, false, Value (Point3 { 0.0, 0.0, 0.0 }) };
    arcCentre.ui = PointUi ("Arc", 0, "The centre of the circle the arc lies on.");
    arc.parameters.push_back (std::move (arcCentre));
    ParameterSchema arcNormal { "normal", "Normal", ValueType::Point3, false, Value (Point3 { 0.0, 0.0, 1.0 }) };
    arcNormal.ui = VectorUi ("Arc", 1, "Which way the arc's plane faces.");
    arc.parameters.push_back (std::move (arcNormal));
    ParameterSchema arcRadius { "radius", "Radius", ValueType::Double, false, Value (1.0) };
    arcRadius.ui = LengthUi ("Arc", 2, "The arc's radius.");
    arc.parameters.push_back (std::move (arcRadius));
    ParameterSchema arcStart { "start", "Start angle", ValueType::Double, false, Value (0.0) };
    arcStart.ui = AngleUi ("Arc", 3, "Where the sweep begins, measured in the arc's plane.");
    arc.parameters.push_back (std::move (arcStart));
    ParameterSchema arcSweep { "sweep", "Sweep", ValueType::Double, false, Value (360.0) };
    arcSweep.ui = AngleUi ("Arc", 4, "How far round it goes. 360 is a full circle.");
    arc.parameters.push_back (std::move (arcSweep));
    ParameterSchema arcSegments { "segments", "Segments", ValueType::Integer, false,
                                  Value (static_cast<int64_t> (32)) };
    arcSegments.ui =
        CountUi ("Arc", 5, "How many straight pieces the arc is drawn with.", geomsrv::engine::kMinArcSegments, 256);
    arc.parameters.push_back (std::move (arcSegments));
    if (!registry.Register (std::move (arc), error))
        throw std::logic_error (error);

    // ⚠️ A PLANE IS AN ORIGIN AND THREE AXES, NOT A VALUE TYPE. Adding one would
    // have to travel through the value variant, the serializer, the bridge schema
    // and every consumer, and half of that is worse than none; the nodes that
    // need a plane - Mirror, Arc, Array - take an origin and a normal, which is
    // what a plane IS. This node is what makes those two agree with each other.
    NodeType plane = PureNode ("geom.plane", "Plane", "An origin and a direction, with axes to match.");
    plane.category = "Geometry";
    plane.inputs = { Port ("origin", ValueType::Point3), Port ("normal", ValueType::Point3) };
    plane.outputs.push_back (Port ("origin", ValueType::Point3));
    plane.outputs.push_back (Port ("normal", ValueType::Point3));
    plane.outputs.push_back (Port ("xAxis", ValueType::Point3));
    plane.outputs.push_back (Port ("yAxis", ValueType::Point3));
    ParameterSchema planeOrigin { "origin", "Origin", ValueType::Point3, false, Value (Point3 { 0.0, 0.0, 0.0 }) };
    planeOrigin.ui = PointUi ("Plane", 0, "Where the plane sits.");
    plane.parameters.push_back (std::move (planeOrigin));
    ParameterSchema planeNormal { "normal", "Normal", ValueType::Point3, false, Value (Point3 { 0.0, 0.0, 1.0 }) };
    planeNormal.ui = VectorUi ("Plane", 1, "Which way the plane faces. It is unitised for you.");
    plane.parameters.push_back (std::move (planeNormal));
    if (!registry.Register (std::move (plane), error))
        throw std::logic_error (error);

    // ------------------------------------------------------------------
    // Making surfaces and solids from curves.
    // ------------------------------------------------------------------
    NodeType extrude = PureNode ("geom.extrude", "Extrude", "Sweeps a closed outline along a vector, capped.");
    extrude.category = "Geometry";
    extrude.inputs.push_back ({ "outline", "Outline", ValueType::Absent, true, false });
    extrude.inputs.push_back ({ "direction", "Direction", ValueType::Point3, false, false });
    extrude.outputs.push_back (Port ("mesh", ValueType::Mesh));
    ParameterSchema extrudeDirection { "direction", "Direction", ValueType::Point3, false,
                                       Value (Point3 { 0.0, 0.0, 3.0 }) };
    extrudeDirection.ui = VectorUi ("Extrude", 0, "How far and which way the outline is swept.");
    extrudeDirection.ui->unit = "m";
    extrude.parameters.push_back (std::move (extrudeDirection));
    if (!registry.Register (std::move (extrude), error))
        throw std::logic_error (error);

    NodeType loft = PureNode ("geom.loft", "Loft", "A surface between two curves with matching point counts.");
    loft.category = "Geometry";
    loft.inputs.push_back ({ "from", "From", ValueType::Absent, true, false });
    loft.inputs.push_back ({ "to", "To", ValueType::Absent, true, false });
    loft.inputs.push_back ({ "closed", "Closed", ValueType::Bool, false, false });
    loft.outputs.push_back (Port ("mesh", ValueType::Mesh));
    ParameterSchema loftClosed { "closed", "Closed", ValueType::Bool, false, Value (false) };
    loftClosed.ui = BooleanUi ("Loft", 0, "Join the last span back to the first, for closed profiles.");
    loft.parameters.push_back (std::move (loftClosed));
    if (!registry.Register (std::move (loft), error))
        throw std::logic_error (error);

    // ------------------------------------------------------------------
    // Sampling curves.
    // ------------------------------------------------------------------
    NodeType divide = PureNode ("geom.divideCurve", "Divide Curve", "Points spaced evenly along a curve.");
    divide.category = "Geometry";
    divide.inputs.push_back ({ "curve", "Curve", ValueType::Absent, true, false });
    divide.inputs.push_back ({ "count", "Count", ValueType::Integer, false, false });
    divide.inputs.push_back ({ "includeEnds", "Include ends", ValueType::Bool, false, false });
    divide.outputs.push_back (Port ("points", ValueType::List));
    divide.outputs.push_back (Port ("count", ValueType::Integer));
    ParameterSchema divideCount { "count", "Count", ValueType::Integer, false, Value (static_cast<int64_t> (8)) };
    divideCount.ui = CountUi ("Divide", 0, "How many segments the curve is cut into.", 1, 1000);
    divide.parameters.push_back (std::move (divideCount));
    ParameterSchema divideEnds { "includeEnds", "Include ends", ValueType::Bool, false, Value (true) };
    divideEnds.ui = BooleanUi ("Divide", 1, "Include the curve's own two ends. N segments is N+1 points.");
    divide.parameters.push_back (std::move (divideEnds));
    if (!registry.Register (std::move (divide), error))
        throw std::logic_error (error);

    NodeType pointOn = PureNode ("geom.pointOnCurve", "Point on Curve", "One point along a curve, by length.");
    pointOn.category = "Geometry";
    pointOn.inputs.push_back ({ "curve", "Curve", ValueType::Absent, true, false });
    pointOn.inputs.push_back ({ "t", "T", ValueType::Double, false, false });
    pointOn.outputs.push_back (Port ("point", ValueType::Point3));
    pointOn.outputs.push_back (Port ("tangent", ValueType::Point3));
    ParameterSchema pointT { "t", "T", ValueType::Double, false, Value (0.5) };
    ParameterUi pointTUi;
    pointTUi.widget = ParameterWidget::Slider;
    pointTUi.section = "Point";
    pointTUi.order = 0;
    pointTUi.help = "How far along, from 0 at the start to 1 at the end. Measured by LENGTH, not by point index.";
    pointTUi.minimum = 0.0;
    pointTUi.maximum = 1.0;
    pointTUi.step = 0.01;
    pointTUi.decimals = 3;
    pointT.ui = std::move (pointTUi);
    pointOn.parameters.push_back (std::move (pointT));
    if (!registry.Register (std::move (pointOn), error))
        throw std::logic_error (error);

    // ------------------------------------------------------------------
    // Transforms and arrays. All six share one matrix applier; see
    // TransformValue for why that is one function and not thirty.
    // ------------------------------------------------------------------
    NodeType move = PureNode ("geom.move", "Move", "Moves geometry by a vector.");
    move.category = "Transform";
    move.inputs.push_back ({ "geometry", "Geometry", ValueType::Absent, true, false });
    move.inputs.push_back ({ "by", "By", ValueType::Point3, false, false });
    move.outputs.push_back (Port ("geometry", ValueType::List));
    ParameterSchema moveBy { "by", "By", ValueType::Point3, false, Value (Point3 { 1.0, 0.0, 0.0 }) };
    moveBy.ui = VectorUi ("Move", 0, "How far, and which way.");
    moveBy.ui->unit = "m";
    move.parameters.push_back (std::move (moveBy));
    if (!registry.Register (std::move (move), error))
        throw std::logic_error (error);

    NodeType rotate = PureNode ("geom.rotate", "Rotate", "Rotates geometry about an axis through a point.");
    rotate.category = "Transform";
    rotate.inputs.push_back ({ "geometry", "Geometry", ValueType::Absent, true, false });
    rotate.inputs.push_back ({ "origin", "Origin", ValueType::Point3, false, false });
    rotate.inputs.push_back ({ "axis", "Axis", ValueType::Point3, false, false });
    rotate.inputs.push_back ({ "angle", "Angle", ValueType::Double, false, false });
    rotate.outputs.push_back (Port ("geometry", ValueType::List));
    ParameterSchema rotateOrigin { "origin", "Origin", ValueType::Point3, false, Value (Point3 { 0.0, 0.0, 0.0 }) };
    rotateOrigin.ui =
        PointUi ("Rotate", 0, "The point the axis passes through. NOT the world origin unless you say so.");
    rotate.parameters.push_back (std::move (rotateOrigin));
    ParameterSchema rotateAxis { "axis", "Axis", ValueType::Point3, false, Value (Point3 { 0.0, 0.0, 1.0 }) };
    rotateAxis.ui = VectorUi ("Rotate", 1, "The axis to turn about.");
    rotate.parameters.push_back (std::move (rotateAxis));
    ParameterSchema rotateAngle { "angle", "Angle", ValueType::Double, false, Value (90.0) };
    rotateAngle.ui = AngleUi ("Rotate", 2, "How far to turn.");
    rotate.parameters.push_back (std::move (rotateAngle));
    if (!registry.Register (std::move (rotate), error))
        throw std::logic_error (error);

    NodeType scaleGeometry = PureNode ("geom.scale", "Scale", "Scales geometry about a point.");
    scaleGeometry.category = "Transform";
    scaleGeometry.inputs.push_back ({ "geometry", "Geometry", ValueType::Absent, true, false });
    scaleGeometry.inputs.push_back ({ "origin", "Origin", ValueType::Point3, false, false });
    scaleGeometry.inputs.push_back ({ "factor", "Factor", ValueType::Point3, false, false });
    scaleGeometry.outputs.push_back (Port ("geometry", ValueType::List));
    ParameterSchema scaleOrigin { "origin", "Origin", ValueType::Point3, false, Value (Point3 { 0.0, 0.0, 0.0 }) };
    scaleOrigin.ui = PointUi ("Scale", 0, "The point that stays put.");
    scaleGeometry.parameters.push_back (std::move (scaleOrigin));
    // Per axis rather than one number, because a non-uniform scale is the case
    // this node is actually wanted for - and it is also the case that makes
    // normals need the inverse transpose. See TransformValue.
    ParameterSchema scaleFactor { "factor", "Factor", ValueType::Point3, false, Value (Point3 { 2.0, 2.0, 2.0 }) };
    scaleFactor.ui = VectorUi ("Scale", 1, "How much bigger, per axis. 1 leaves an axis alone.");
    scaleGeometry.parameters.push_back (std::move (scaleFactor));
    if (!registry.Register (std::move (scaleGeometry), error))
        throw std::logic_error (error);

    NodeType mirror = PureNode ("geom.mirror", "Mirror", "Reflects geometry in a plane.");
    mirror.category = "Transform";
    mirror.inputs.push_back ({ "geometry", "Geometry", ValueType::Absent, true, false });
    mirror.inputs.push_back ({ "origin", "Origin", ValueType::Point3, false, false });
    mirror.inputs.push_back ({ "normal", "Normal", ValueType::Point3, false, false });
    mirror.outputs.push_back (Port ("geometry", ValueType::List));
    ParameterSchema mirrorOrigin { "origin", "Origin", ValueType::Point3, false, Value (Point3 { 0.0, 0.0, 0.0 }) };
    mirrorOrigin.ui = PointUi ("Mirror", 0, "A point on the mirror plane.");
    mirror.parameters.push_back (std::move (mirrorOrigin));
    ParameterSchema mirrorNormal { "normal", "Normal", ValueType::Point3, false, Value (Point3 { 1.0, 0.0, 0.0 }) };
    mirrorNormal.ui = VectorUi ("Mirror", 1, "Which way the mirror plane faces.");
    mirror.parameters.push_back (std::move (mirrorNormal));
    if (!registry.Register (std::move (mirror), error))
        throw std::logic_error (error);

    NodeType arrayLinear = PureNode ("geom.arrayLinear", "Linear Array", "Repeats geometry along a vector.");
    arrayLinear.category = "Transform";
    arrayLinear.inputs.push_back ({ "geometry", "Geometry", ValueType::Absent, true, false });
    arrayLinear.inputs.push_back ({ "step", "Step", ValueType::Point3, false, false });
    arrayLinear.inputs.push_back ({ "count", "Count", ValueType::Integer, false, false });
    arrayLinear.outputs.push_back (Port ("geometry", ValueType::List));
    arrayLinear.outputs.push_back (Port ("count", ValueType::Integer));
    ParameterSchema arrayStep { "step", "Step", ValueType::Point3, false, Value (Point3 { 1.0, 0.0, 0.0 }) };
    arrayStep.ui = VectorUi ("Array", 0, "The gap between one copy and the next.");
    arrayStep.ui->unit = "m";
    arrayLinear.parameters.push_back (std::move (arrayStep));
    // The ORIGINAL counts as one. A count of 5 gives five things, not six.
    ParameterSchema arrayCount { "count", "Count", ValueType::Integer, false, Value (static_cast<int64_t> (5)) };
    arrayCount.ui = CountUi ("Array", 1, "How many copies in total, counting the original.", 1, 10000);
    arrayLinear.parameters.push_back (std::move (arrayCount));
    if (!registry.Register (std::move (arrayLinear), error))
        throw std::logic_error (error);

    NodeType arrayGrid = PureNode ("geom.arrayGrid", "Grid Array", "Repeats geometry across two directions.");
    arrayGrid.category = "Transform";
    arrayGrid.inputs.push_back ({ "geometry", "Geometry", ValueType::Absent, true, false });
    arrayGrid.inputs.push_back ({ "stepX", "Step X", ValueType::Point3, false, false });
    arrayGrid.inputs.push_back ({ "stepY", "Step Y", ValueType::Point3, false, false });
    arrayGrid.inputs.push_back ({ "countX", "Count X", ValueType::Integer, false, false });
    arrayGrid.inputs.push_back ({ "countY", "Count Y", ValueType::Integer, false, false });
    arrayGrid.outputs.push_back (Port ("geometry", ValueType::List));
    arrayGrid.outputs.push_back (Port ("count", ValueType::Integer));
    ParameterSchema gridStepX { "stepX", "Step X", ValueType::Point3, false, Value (Point3 { 1.0, 0.0, 0.0 }) };
    gridStepX.ui = VectorUi ("Grid", 0, "The gap along the first direction.");
    gridStepX.ui->unit = "m";
    arrayGrid.parameters.push_back (std::move (gridStepX));
    ParameterSchema gridStepY { "stepY", "Step Y", ValueType::Point3, false, Value (Point3 { 0.0, 1.0, 0.0 }) };
    gridStepY.ui = VectorUi ("Grid", 1, "The gap along the second direction.");
    gridStepY.ui->unit = "m";
    arrayGrid.parameters.push_back (std::move (gridStepY));
    ParameterSchema gridCountX { "countX", "Count X", ValueType::Integer, false, Value (static_cast<int64_t> (3)) };
    gridCountX.ui = CountUi ("Grid", 2, "How many along the first direction.", 1, 1000);
    arrayGrid.parameters.push_back (std::move (gridCountX));
    ParameterSchema gridCountY { "countY", "Count Y", ValueType::Integer, false, Value (static_cast<int64_t> (3)) };
    gridCountY.ui = CountUi ("Grid", 3, "How many along the second direction.", 1, 1000);
    arrayGrid.parameters.push_back (std::move (gridCountY));
    if (!registry.Register (std::move (arrayGrid), error))
        throw std::logic_error (error);
}

bool IsGeometryNodeType (const std::string& nodeTypeId)
{
    // ⚠️ PREFIX-MATCHED, and that is a rule the ids have to keep. Every type this
    // family registers is named "geom." plus what it makes, so adding one needs
    // no change here - and a type that broke the convention would be silently
    // routed to the builtin executor, which would report it as unknown.
    // `point` and `vector` are the two exceptions, and they are NAMED rather
    // than renamed: their ids are written into every graph already saved with
    // them, and a rename would be a silent load failure for a cosmetic gain.
    return nodeTypeId.rfind ("geom.", 0) == 0 || nodeTypeId == "point" || nodeTypeId == "vector";
}

bool ExecuteGeometryNode (const Node& node, const ValueMap& inputs, const NodeExecutionContext& context,
                          ValueMap& outputs, std::string& error)
{
    // Pure nodes read nothing outside their inputs; that is what makes them pure.
    (void) context;

    if (false) {
    }
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
        for (const Value& item : inputs.at ("points").Items ())
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
    else if (node.nodeType == "geom.line") {
        const geomsrv::engine::Vector3 start = ToEngineVector (Point3From (inputs, node, "start"));
        const geomsrv::engine::Vector3 end = ToEngineVector (Point3From (inputs, node, "end"));
        const std::vector<geomsrv::engine::Vector3> points { start, end };
        std::vector<double> cumulative;
        outputs.emplace ("length", Value (geomsrv::engine::PolylineLength (points, cumulative)));
        outputs.emplace ("curve", PolylineValueFrom (points));
    }
    else if (node.nodeType == "geom.polyline") {
        const std::vector<geomsrv::engine::Vector3> points = PointsFrom (inputs, node, "points");
        if (points.size () < 2) {
            error = "a polyline needs at least two points";
            return false;
        }
        std::vector<double> cumulative;
        outputs.emplace ("length", Value (geomsrv::engine::PolylineLength (points, cumulative)));
        outputs.emplace ("curve", PolylineValueFrom (points));
    }
    else if (node.nodeType == "geom.arc") {
        std::vector<geomsrv::engine::Vector3> points;
        if (!geomsrv::engine::MakeArc (
                ToEngineVector (Point3From (inputs, node, "centre")),
                ToEngineVector (Point3From (inputs, node, "normal")), ScalarFrom (inputs, node, "radius", 1.0),
                Radians (ScalarFrom (inputs, node, "start", 0.0)), Radians (ScalarFrom (inputs, node, "sweep", 360.0)),
                static_cast<int> (ScalarFrom (inputs, node, "segments", 32.0)), points, error)) {
            return false;
        }
        outputs.emplace ("curve", PolylineValueFrom (points));
    }
    else if (node.nodeType == "geom.plane") {
        const geomsrv::engine::Vector3 origin = ToEngineVector (Point3From (inputs, node, "origin"));
        geomsrv::engine::Vector3 normal;
        if (!geomsrv::engine::Unit (ToEngineVector (Point3From (inputs, node, "normal")), normal, error)) {
            error = "a plane needs a normal with a direction";
            return false;
        }
        // The same seed rule the arc uses: crossing with a fixed axis gives a
        // zero-length result for exactly the commonest case in a Z-up model.
        const geomsrv::engine::Vector3 seed = std::fabs (normal.z) < 0.9 ? geomsrv::engine::Vector3 { 0.0, 0.0, 1.0 }
                                                                         : geomsrv::engine::Vector3 { 1.0, 0.0, 0.0 };
        geomsrv::engine::Vector3 xAxis;
        geomsrv::engine::Vector3 yAxis;
        if (!geomsrv::engine::Unit (geomsrv::engine::Cross (normal, seed), xAxis, error) ||
            !geomsrv::engine::Unit (geomsrv::engine::Cross (normal, xAxis), yAxis, error)) {
            return false;
        }
        outputs.emplace ("origin", Value (FromEngineVector (origin)));
        outputs.emplace ("normal", Value (FromEngineVector (normal)));
        outputs.emplace ("xAxis", Value (FromEngineVector (xAxis)));
        outputs.emplace ("yAxis", Value (FromEngineVector (yAxis)));
    }
    else if (node.nodeType == "geom.extrude") {
        geomsrv::Mesh mesh;
        if (!geomsrv::engine::MakeExtrusion (PointsFrom (inputs, node, "outline"),
                                             ToEngineVector (Point3From (inputs, node, "direction")), mesh, error)) {
            return false;
        }
        outputs.emplace ("mesh", Value (std::make_shared<const geomsrv::Mesh> (std::move (mesh))));
    }
    else if (node.nodeType == "geom.loft") {
        geomsrv::Mesh mesh;
        if (!geomsrv::engine::MakeLoft (PointsFrom (inputs, node, "from"), PointsFrom (inputs, node, "to"),
                                        BoolFrom (inputs, node, "closed", false), mesh, error)) {
            return false;
        }
        outputs.emplace ("mesh", Value (std::make_shared<const geomsrv::Mesh> (std::move (mesh))));
    }
    else if (node.nodeType == "geom.divideCurve") {
        std::vector<geomsrv::engine::Vector3> points;
        if (!geomsrv::engine::DividePolyline (PointsFrom (inputs, node, "curve"),
                                              static_cast<int> (ScalarFrom (inputs, node, "count", 8.0)),
                                              BoolFrom (inputs, node, "includeEnds", true), points, error)) {
            return false;
        }
        outputs.emplace ("count", Value (static_cast<int64_t> (points.size ())));
        outputs.emplace ("points", PointListValue (points));
    }
    else if (node.nodeType == "geom.pointOnCurve") {
        geomsrv::engine::Vector3 point;
        geomsrv::engine::Vector3 tangent;
        if (!geomsrv::engine::PointOnPolyline (PointsFrom (inputs, node, "curve"), ScalarFrom (inputs, node, "t", 0.5),
                                               point, tangent, error)) {
            return false;
        }
        outputs.emplace ("point", Value (FromEngineVector (point)));
        outputs.emplace ("tangent", Value (FromEngineVector (tangent)));
    }
    else if (node.nodeType == "geom.move" || node.nodeType == "geom.rotate" || node.nodeType == "geom.scale" ||
             node.nodeType == "geom.mirror") {
        // One matrix, then one applier. See TransformValue: the four differ only
        // in how the matrix is built.
        geomsrv::engine::Transform transform;
        bool built = false;
        if (node.nodeType == "geom.move") {
            transform = geomsrv::engine::Translation (ToEngineVector (Point3From (inputs, node, "by")));
            built = true;
        }
        else if (node.nodeType == "geom.rotate") {
            built = geomsrv::engine::Rotation (ToEngineVector (Point3From (inputs, node, "origin")),
                                               ToEngineVector (Point3From (inputs, node, "axis")),
                                               Radians (ScalarFrom (inputs, node, "angle", 90.0)), transform, error);
        }
        else if (node.nodeType == "geom.scale") {
            built = geomsrv::engine::Scaling (ToEngineVector (Point3From (inputs, node, "origin")),
                                              ToEngineVector (Point3From (inputs, node, "factor")), transform, error);
        }
        else {
            built = geomsrv::engine::Mirroring (ToEngineVector (Point3From (inputs, node, "origin")),
                                                ToEngineVector (Point3From (inputs, node, "normal")), transform, error);
        }
        if (!built)
            return false;
        const auto geometry = inputs.find ("geometry");
        outputs.emplace ("geometry",
                         TransformedList (geometry == inputs.end () ? Argument {} : geometry->second, transform));
    }
    else if (node.nodeType == "geom.arrayLinear" || node.nodeType == "geom.arrayGrid") {
        const bool grid = node.nodeType == "geom.arrayGrid";
        const int64_t countX =
            static_cast<int64_t> (ScalarFrom (inputs, node, grid ? "countX" : "count", grid ? 3.0 : 5.0));
        const int64_t countY = grid ? static_cast<int64_t> (ScalarFrom (inputs, node, "countY", 3.0)) : 1;
        if (countX < 1 || countY < 1 || countX * countY > 100000) {
            error = "an array makes between 1 and 100000 copies";
            return false;
        }
        const geomsrv::engine::Vector3 stepX = ToEngineVector (Point3From (inputs, node, grid ? "stepX" : "step"));
        const geomsrv::engine::Vector3 stepY =
            grid ? ToEngineVector (Point3From (inputs, node, "stepY")) : geomsrv::engine::Vector3 { 0, 0, 0 };
        const auto geometry = inputs.find ("geometry");
        const Argument source = geometry == inputs.end () ? Argument {} : geometry->second;

        // Flat, never nested: a source that is itself a branch contributes its
        // whole (transformed) branch at each array position rather than a list
        // of lists, which a tree cannot hold (§7.3).
        std::vector<Value> copies;
        copies.reserve (static_cast<size_t> (countX * countY));
        for (int64_t y = 0; y < countY; ++y) {
            for (int64_t x = 0; x < countX; ++x) {
                // ⚠️ THE ORIGINAL IS COPY ZERO. A count of five means five
                // things, not five plus the one you started with - the other
                // reading puts every array one bay too long, which reads as a
                // modelling decision rather than as an off-by-one.
                const geomsrv::engine::Vector3 offset {
                    stepX.x * static_cast<double> (x) + stepY.x * static_cast<double> (y),
                    stepX.y * static_cast<double> (x) + stepY.y * static_cast<double> (y),
                    stepX.z * static_cast<double> (x) + stepY.z * static_cast<double> (y)
                };
                const Argument moved = TransformArgument (source, geomsrv::engine::Translation (offset));
                if (moved.Type () == ValueType::List) {
                    for (const Value& item : moved.Items ())
                        copies.push_back (item);
                }
                else {
                    copies.push_back (moved.AsValue ());
                }
            }
        }
        outputs.emplace ("count", Value (static_cast<int64_t> (copies.size ())));
        outputs.emplace ("geometry", Argument::FromItems (std::move (copies)));
    }
    else if (node.nodeType == "geom.box" || node.nodeType == "geom.sphere") {
        // ⚠️ THE BUILDER'S REFUSALS BECOME NODE FAILURES, NOT EMPTY MESHES. A
        // box of zero height and a box that failed to build look the same in a
        // viewport; only one of them says why, and the evaluator already has a
        // place to put that - the node goes Error and its downstream goes
        // Blocked, with the reason on the node.
        const geomsrv::engine::Vector3 at = ToEngineVector (Point3From (inputs, node, "centre"));
        geomsrv::Mesh mesh;
        const bool built = node.nodeType == "geom.box"
                               ? geomsrv::engine::MakeBox (at, ScalarFrom (inputs, node, "width", 1.0),
                                                           ScalarFrom (inputs, node, "depth", 1.0),
                                                           ScalarFrom (inputs, node, "height", 1.0), mesh, error)
                               : geomsrv::engine::MakeSphere (
                                     at, ScalarFrom (inputs, node, "radius", 0.5),
                                     static_cast<int> (ScalarFrom (inputs, node, "segments", 24.0)), mesh, error);
        if (!built)
            return false;
        outputs.emplace ("mesh", Value (std::make_shared<const geomsrv::Mesh> (std::move (mesh))));
    }
    else {
        error = "unknown geometry node type: " + node.nodeType;
        return false;
    }
    return true;
}

} // namespace evp::nodegraph
