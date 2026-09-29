// ArchViz/OverlayAnnotationContent -- see the header.

#include "ArchViz/OverlayAnnotationContent.hpp"

#include "Annotation/DimensionGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace geomsrv {
namespace archviz {
namespace overlayannotations {

namespace layers = overlaylayers;
using annotation::Point3;
using annotation::PrimitiveKind;

namespace {

constexpr double kPi = 3.14159265358979323846;

void Push (std::vector<double>& out, const Point3& p)
{
    out.insert (out.end (), { p.x, p.y, p.z });
}

double Distance (const Point3& a, const Point3& b)
{
    return std::sqrt ((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y) + (b.z - a.z) * (b.z - a.z));
}

// The plane an arrowhead is drawn in, which must contain the arrow
// (BuildArrowheadLegs refuses one it pierces): the primitive's own; else the plan for
// a level arrow, so the head reads from above; else the upright plane through it.
Point3 PlaneFor (const annotation::Primitive& primitive, const Point3& direction)
{
    if (primitive.planeNormal.has_value ())
        return *primitive.planeNormal;
    const double length = std::sqrt (direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    if (length <= 0.0 || std::fabs (direction.z) < 1e-9 * length)
        return Point3 { 0.0, 0.0, 1.0 };
    // cross (direction, z): horizontal, perpendicular to the arrow.
    const double nx = direction.y, ny = -direction.x;
    const double n = std::sqrt (nx * nx + ny * ny);
    return n > 1e-12 * length ? Point3 { nx / n, ny / n, 0.0 } : Point3 { 1.0, 0.0, 0.0 };
}

layers::Text Label (const std::string& text, const Point3& at, uint32_t rgba, float dx, float dy)
{
    layers::Text label;
    label.text = text;
    label.at[0] = at.x;
    label.at[1] = at.y;
    label.at[2] = at.z;
    label.offsetPixels[0] = dx;
    label.offsetPixels[1] = dy;
    label.sizePixels = 12.0f;
    label.rgba = rgba;
    label.haloRgba = 0x000000B0u;
    label.align = dx != 0.0f ? layers::Align::Left : layers::Align::Center;
    label.behind = layers::Behind::Fade; // the viewer's FadeWhenOccluded
    return label;
}

} // namespace

Built BuildLayer (const annotation::Frame& frame)
{
    Built out;
    out.layer.name = kLayerName;
    out.layer.views = layers::Views::Both;
    out.layer.occluded = false;
    for (const annotation::Primitive& primitive : frame.primitives) {
        ++out.primitives;
        if (!annotation::IsDrawable (primitive) || primitive.kind == PrimitiveKind::Element)
            continue;
        const uint32_t rgba = annotation::PackRgba (annotation::RoleColour (primitive.role));
        const std::vector<Point3>& points = primitive.points;
        switch (primitive.kind) {
            case PrimitiveKind::Point: {
                layers::PointSet set;
                Push (set.points, points[0]);
                set.rgba = rgba;
                set.sizePixels = 8.0f;
                set.sizeMetres = 0.2f;
                out.layer.points.push_back (std::move (set));
                if (!primitive.text.empty ())
                    out.layer.texts.push_back (Label (primitive.text, points[0], rgba, 7.0f, 7.0f));
                break;
            }
            case PrimitiveKind::Label:
                out.layer.texts.push_back (Label (primitive.text, points[0], rgba, 0.0f, 0.0f));
                break;
            case PrimitiveKind::Polyline:
            case PrimitiveKind::Arrow: {
                layers::Polyline polyline;
                for (const Point3& p : points)
                    Push (polyline.points, p);
                polyline.closed = primitive.kind == PrimitiveKind::Polyline && primitive.closed && points.size () > 2;
                polyline.rgba = rgba;
                polyline.widthPixels = primitive.role == annotation::SemanticRole::Context ? 1.0f : 2.0f;
                out.layer.polylines.push_back (std::move (polyline));
                if (primitive.kind == PrimitiveKind::Arrow) {
                    const Point3& tail = points[points.size () - 2];
                    const Point3& tip = points.back ();
                    const double length = Distance (tail, tip);
                    const Point3 direction = { tip.x - tail.x, tip.y - tail.y, tip.z - tail.z };
                    std::vector<Point3> legs;
                    const double leg = (std::min) ((std::max) (0.12 * length, 0.05), 0.6);
                    if (annotation::BuildArrowheadLegs (tail, tip, PlaneFor (primitive, direction), leg, 0.4636476,
                                                        legs) &&
                        legs.size () == 4) {
                        layers::Polyline head;
                        Push (head.points, legs[1]);
                        Push (head.points, legs[0]);
                        Push (head.points, legs[3]);
                        head.rgba = rgba;
                        head.widthPixels = 2.0f;
                        out.layer.polylines.push_back (std::move (head));
                    }
                    if (!primitive.text.empty ()) {
                        const Point3& first = points.front ();
                        out.layer.texts.push_back (
                            Label (primitive.text,
                                   { (first.x + tip.x) * 0.5, (first.y + tip.y) * 0.5, (first.z + tip.z) * 0.5 }, rgba,
                                   0.0f, -10.0f));
                    }
                }
                break;
            }
            case PrimitiveKind::Dimension: {
                layers::Dimension dimension;
                dimension.from[0] = points[0].x;
                dimension.from[1] = points[0].y;
                dimension.from[2] = points[0].z;
                dimension.to[0] = points[1].x;
                dimension.to[1] = points[1].y;
                dimension.to[2] = points[1].z;
                // The viewer's default offset when the primitive names none.
                dimension.offsetMetres =
                    primitive.offset != 0.0 ? primitive.offset : annotation::DimensionStyle {}.dimensionOffset;
                if (primitive.preferredOffsetDirection.has_value ()) {
                    dimension.direction[0] = primitive.preferredOffsetDirection->x;
                    dimension.direction[1] = primitive.preferredOffsetDirection->y;
                    dimension.direction[2] = primitive.preferredOffsetDirection->z;
                }
                if (primitive.planeNormal.has_value ()) {
                    dimension.normal[0] = primitive.planeNormal->x;
                    dimension.normal[1] = primitive.planeNormal->y;
                    dimension.normal[2] = primitive.planeNormal->z;
                }
                dimension.text = primitive.text;
                dimension.rgba = rgba;
                dimension.textSizePixels = 12.0f;
                dimension.terminator = layers::Terminator::Arrow;
                dimension.behind = primitive.alwaysVisible ? layers::Behind::Show : layers::Behind::Fade;
                out.layer.dimensions.push_back (std::move (dimension));
                break;
            }
            case PrimitiveKind::Angle: {
                annotation::AngularDimensionInput input;
                input.vertex = points[0];
                input.firstRayPoint = points[1];
                input.secondRayPoint = points[2];
                if (primitive.planeNormal.has_value ())
                    input.planes.explicitNormal = primitive.planeNormal;
                input.planes.declaredNormal = Point3 { 0.0, 0.0, 1.0 };
                const std::optional<annotation::ResolvedAngularDimensionGeometry> resolved =
                    annotation::ResolveAngularDimensionGeometry (input);
                if (!resolved.has_value ())
                    continue;
                const double radius =
                    (std::max) (0.3 * (std::min) (Distance (points[0], points[1]), Distance (points[0], points[2])),
                                0.05);
                std::vector<Point3> arc;
                if (!annotation::SampleAngleArc (points[0], points[1], points[2], resolved->planeNormal, radius, 32,
                                                 arc) ||
                    arc.size () < 2)
                    continue;
                layers::Polyline polyline;
                for (const Point3& p : arc)
                    Push (polyline.points, p);
                polyline.rgba = rgba;
                polyline.widthPixels = 1.5f;
                out.layer.polylines.push_back (std::move (polyline));
                char degrees[32] = {};
                std::snprintf (degrees, sizeof (degrees), "%.1f\xC2\xB0", resolved->angleRadians * 180.0 / kPi);
                const Point3& middle = arc[arc.size () / 2];
                out.layer.texts.push_back (
                    Label (primitive.text.empty () ? degrees : primitive.text, middle, rgba, 0.0f, 0.0f));
                break;
            }
            case PrimitiveKind::Element:
                continue;
        }
        ++out.drawn;
    }
    return out;
}

} // namespace overlayannotations
} // namespace archviz
} // namespace geomsrv
