#include "Annotation/DrawList.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv {
namespace annotation {

namespace {

constexpr double kMinLength = 1.0e-12;
constexpr double kPlanarTolerance = 1.0e-9;
constexpr double kPi = 3.14159265358979323846;
constexpr size_t kMaxArcSegments = 4096;
constexpr size_t kArchitecturalArcSegments = 32;

Point3 Add (const Point3& left, const Point3& right)
{
    return { left.x + right.x, left.y + right.y, left.z + right.z };
}

Point3 Subtract (const Point3& left, const Point3& right)
{
    return { left.x - right.x, left.y - right.y, left.z - right.z };
}

Point3 Scale (const Point3& point, double scale)
{
    return { point.x * scale, point.y * scale, point.z * scale };
}

double Dot (const Point3& left, const Point3& right)
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

Point3 Cross (const Point3& left, const Point3& right)
{
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

bool Normalize (const Point3& point, Point3& normalized)
{
    const double lengthSquared = Dot (point, point);
    if (!std::isfinite (lengthSquared) || lengthSquared <= kMinLength * kMinLength)
        return false;

    normalized = Scale (point, 1.0 / std::sqrt (lengthSquared));
    return IsFinite (normalized);
}

size_t MinimumPointCount (PrimitiveKind kind)
{
    switch (kind) {
        case PrimitiveKind::Point:
        case PrimitiveKind::Label:
            return 1;
        case PrimitiveKind::Element:
            return 0;
        case PrimitiveKind::Polyline:
        case PrimitiveKind::Arrow:
        case PrimitiveKind::Dimension:
            return 2;
        case PrimitiveKind::Angle:
            return 3;
    }
    return 0;
}

} // namespace

ColourRgba RoleColour (SemanticRole role)
{
    switch (role) {
        case SemanticRole::None:
            return { 0x8B, 0x1E, 0x1E, 0xFF };
        case SemanticRole::Add:
            return { 0x18, 0x9C, 0x5C, 0xFF };
        case SemanticRole::Remove:
            return { 0xDA, 0x44, 0x44, 0xFF };
        case SemanticRole::Modify:
            return { 0xE8, 0x91, 0x1C, 0xFF };
        case SemanticRole::Context:
            return { 0x5C, 0x6C, 0x7E, 0xFF };
        case SemanticRole::Guide:
            return { 0x30, 0x7E, 0xD6, 0xFF };
    }
    return { 0x8B, 0x1E, 0x1E, 0xFF };
}

uint32_t PackRgba (ColourRgba colour)
{
    return (uint32_t (colour.red) << 24) | (uint32_t (colour.green) << 16) | (uint32_t (colour.blue) << 8) |
           uint32_t (colour.alpha);
}

bool BuildArchitecturalAngleGlyph (const ScreenPoint& center, const ScreenPoint& firstRayPoint,
                                   const ScreenPoint& secondRayPoint, double dpiScale, ArchitecturalAngleGlyph& glyph)
{
    if (!std::isfinite (center.x) || !std::isfinite (center.y) || !std::isfinite (firstRayPoint.x) ||
        !std::isfinite (firstRayPoint.y) || !std::isfinite (secondRayPoint.x) || !std::isfinite (secondRayPoint.y) ||
        !std::isfinite (dpiScale) || dpiScale <= 0.0)
        return false;

    double firstX = firstRayPoint.x - center.x;
    double firstY = firstRayPoint.y - center.y;
    double secondX = secondRayPoint.x - center.x;
    double secondY = secondRayPoint.y - center.y;
    const double firstLength = std::hypot (firstX, firstY);
    const double secondLength = std::hypot (secondX, secondY);
    if (firstLength <= kMinLength || secondLength <= kMinLength)
        return false;
    firstX /= firstLength;
    firstY /= firstLength;
    secondX /= secondLength;
    secondY /= secondLength;

    const double dot = std::clamp (firstX * secondX + firstY * secondY, -1.0, 1.0);
    const double cross = firstX * secondY - firstY * secondX;
    const double minorAngle = std::atan2 (std::fabs (cross), dot);
    if (minorAngle <= kMinLength)
        return false;
    double sweepSign = cross > 0.0 ? 1.0 : -1.0;
    if (cross == 0.0 && dot < 0.0) {
        const bool firstBeforeSecond = firstX < secondX || (firstX == secondX && firstY < secondY);
        sweepSign = firstBeforeSecond ? 1.0 : -1.0;
    }
    const double sweep = minorAngle * sweepSign;

    ArchitecturalAngleGlyph next;
    const double radius = 30.0 * dpiScale;
    const double arrowLength = 8.0 * dpiScale;
    const double arrowHalfWidth = 3.5 * dpiScale;
    next.arc.reserve (kArchitecturalArcSegments + 1);
    for (size_t index = 0; index <= kArchitecturalArcSegments; ++index) {
        const double angle = sweep * double (index) / double (kArchitecturalArcSegments);
        const double radialX = firstX * std::cos (angle) - firstY * std::sin (angle);
        const double radialY = firstX * std::sin (angle) + firstY * std::cos (angle);
        next.arc.push_back ({ center.x + radialX * radius, center.y + radialY * radius });
    }
    next.arc.front () = { center.x + firstX * radius, center.y + firstY * radius };
    next.arc.back () = { center.x + secondX * radius, center.y + secondY * radius };

    const auto makeArrow = [arrowLength, arrowHalfWidth] (const ScreenPoint& tip, double directionX,
                                                          double directionY) {
        const ScreenPoint base { tip.x - directionX * arrowLength, tip.y - directionY * arrowLength };
        const double normalX = -directionY;
        const double normalY = directionX;
        return ScreenTriangle { tip,
                                { base.x + normalX * arrowHalfWidth, base.y + normalY * arrowHalfWidth },
                                { base.x - normalX * arrowHalfWidth, base.y - normalY * arrowHalfWidth } };
    };
    const double firstTangentX = -firstY * sweepSign;
    const double firstTangentY = firstX * sweepSign;
    const double endTangentX = -secondY * sweepSign;
    const double endTangentY = secondX * sweepSign;
    next.arrowheads[0] = makeArrow (next.arc.front (), -firstTangentX, -firstTangentY);
    next.arrowheads[1] = makeArrow (next.arc.back (), endTangentX, endTangentY);

    const double halfAngle = sweep * 0.5;
    const double bisectorX = firstX * std::cos (halfAngle) - firstY * std::sin (halfAngle);
    const double bisectorY = firstX * std::sin (halfAngle) + firstY * std::cos (halfAngle);
    const double labelRadius = radius + 12.0 * dpiScale;
    next.labelAnchor = { center.x + bisectorX * labelRadius, center.y + bisectorY * labelRadius };
    next.degrees = std::fabs (sweep) * 180.0 / kPi;
    next.arcWidthPixels = std::max (1.0, dpiScale);
    next.fontSizePixels = 18.0 * dpiScale;
    glyph = std::move (next);
    return true;
}

bool DrawList::SelectNode (size_t nodeIndex)
{
    if (nodeIndex >= nodes.size ())
        return false;

    selectedNode_ = nodeIndex;
    selectedFrame_.reset ();
    return true;
}

bool DrawList::SelectFrame (size_t nodeIndex, size_t frameIndex)
{
    if (nodeIndex >= nodes.size () || frameIndex >= nodes[nodeIndex].frames.size ())
        return false;

    selectedNode_ = nodeIndex;
    selectedFrame_ = frameIndex;
    return true;
}

void DrawList::ClearSelection ()
{
    selectedNode_.reset ();
    selectedFrame_.reset ();
}

const Node* DrawList::SelectedNode () const
{
    if (!selectedNode_.has_value () || *selectedNode_ >= nodes.size ())
        return nullptr;
    return &nodes[*selectedNode_];
}

const Frame* DrawList::SelectedFrame () const
{
    const Node* node = SelectedNode ();
    if (node == nullptr || !selectedFrame_.has_value () || *selectedFrame_ >= node->frames.size ())
        return nullptr;
    return &node->frames[*selectedFrame_];
}

bool DrawList::GetBounds (Point3& minimum, Point3& maximum) const
{
    bool hasBounds = false;
    Point3 nextMinimum {};
    Point3 nextMaximum {};

    for (const Node& node : nodes) {
        for (const Frame& frame : node.frames) {
            for (const Primitive& primitive : frame.primitives) {
                if (!IsDrawable (primitive))
                    continue;
                for (const Point3& point : primitive.points) {
                    if (!hasBounds) {
                        nextMinimum = point;
                        nextMaximum = point;
                        hasBounds = true;
                        continue;
                    }
                    nextMinimum.x = std::min (nextMinimum.x, point.x);
                    nextMinimum.y = std::min (nextMinimum.y, point.y);
                    nextMinimum.z = std::min (nextMinimum.z, point.z);
                    nextMaximum.x = std::max (nextMaximum.x, point.x);
                    nextMaximum.y = std::max (nextMaximum.y, point.y);
                    nextMaximum.z = std::max (nextMaximum.z, point.z);
                }
            }
        }
    }

    if (!hasBounds)
        return false;
    minimum = nextMinimum;
    maximum = nextMaximum;
    return true;
}

bool GetBounds (const Frame& frame, Point3& minimum, Point3& maximum)
{
    bool hasBounds = false;
    Point3 nextMinimum {};
    Point3 nextMaximum {};
    for (const Primitive& primitive : frame.primitives) {
        if (!IsDrawable (primitive) || primitive.kind == PrimitiveKind::Element)
            continue;
        for (const Point3& point : primitive.points) {
            if (!hasBounds) {
                nextMinimum = nextMaximum = point;
                hasBounds = true;
            }
            else {
                nextMinimum.x = std::min (nextMinimum.x, point.x);
                nextMinimum.y = std::min (nextMinimum.y, point.y);
                nextMinimum.z = std::min (nextMinimum.z, point.z);
                nextMaximum.x = std::max (nextMaximum.x, point.x);
                nextMaximum.y = std::max (nextMaximum.y, point.y);
                nextMaximum.z = std::max (nextMaximum.z, point.z);
            }
        }
    }
    if (!hasBounds)
        return false;
    minimum = nextMinimum;
    maximum = nextMaximum;
    return true;
}

bool FitFrame (const Frame& frame, double width, double height, double margin, Transform2D& transform)
{
    Point3 minimum;
    Point3 maximum;
    if (!std::isfinite (width) || !std::isfinite (height) || !std::isfinite (margin) || width <= 2.0 * margin ||
        height <= 2.0 * margin || !GetBounds (frame, minimum, maximum))
        return false;

    const double extentX = std::max (maximum.x - minimum.x, kMinLength);
    const double extentY = std::max (maximum.y - minimum.y, kMinLength);
    const double scale = std::min ((width - 2.0 * margin) / extentX, (height - 2.0 * margin) / extentY);
    if (!std::isfinite (scale) || scale <= 0.0)
        return false;

    Transform2D next;
    next.scaleX = scale;
    next.scaleY = -scale;
    next.offX = width * 0.5 - (minimum.x + maximum.x) * 0.5 * scale;
    next.offY = height * 0.5 + (minimum.y + maximum.y) * 0.5 * scale;
    transform = next;
    return true;
}

bool IsFinite (const Point3& point)
{
    return std::isfinite (point.x) && std::isfinite (point.y) && std::isfinite (point.z);
}

bool IsDrawable (const Primitive& primitive)
{
    if (!std::isfinite (primitive.offset))
        return false;
    if (primitive.kind == PrimitiveKind::Element && primitive.guid.empty ())
        return false;
    if (primitive.points.size () < MinimumPointCount (primitive.kind))
        return false;
    return std::all_of (primitive.points.begin (), primitive.points.end (), IsFinite);
}

bool BuildArrowheadLegs (const Point3& tail, const Point3& tip, const Point3& planeNormal, double legLength,
                         double halfAngleRadians, std::vector<Point3>& outLegs)
{
    if (!IsFinite (tail) || !IsFinite (tip) || !IsFinite (planeNormal) || !std::isfinite (legLength) ||
        !std::isfinite (halfAngleRadians) || legLength <= kMinLength || halfAngleRadians <= 0.0 ||
        halfAngleRadians >= kPi)
        return false;

    Point3 direction;
    Point3 normal;
    if (!Normalize (Subtract (tail, tip), direction) || !Normalize (planeNormal, normal))
        return false;
    if (std::fabs (Dot (direction, normal)) > kPlanarTolerance)
        return false;

    Point3 perpendicular;
    if (!Normalize (Cross (normal, direction), perpendicular))
        return false;

    const double along = legLength * std::cos (halfAngleRadians);
    const double across = legLength * std::sin (halfAngleRadians);
    const Point3 left = Add (tip, Add (Scale (direction, along), Scale (perpendicular, across)));
    const Point3 right = Add (tip, Add (Scale (direction, along), Scale (perpendicular, -across)));
    if (!IsFinite (left) || !IsFinite (right))
        return false;

    const Point3 legs[] = { tip, left, tip, right };
    outLegs.insert (outLegs.end (), std::begin (legs), std::end (legs));
    return true;
}

bool SampleAngleArc (const Point3& center, const Point3& firstRayPoint, const Point3& secondRayPoint,
                     const Point3& planeNormal, double radius, size_t segmentCount, std::vector<Point3>& outPoints)
{
    if (!IsFinite (center) || !IsFinite (firstRayPoint) || !IsFinite (secondRayPoint) || !IsFinite (planeNormal) ||
        !std::isfinite (radius) || radius <= kMinLength || segmentCount == 0 || segmentCount > kMaxArcSegments)
        return false;

    Point3 firstDirection;
    Point3 secondDirection;
    Point3 normal;
    if (!Normalize (Subtract (firstRayPoint, center), firstDirection) ||
        !Normalize (Subtract (secondRayPoint, center), secondDirection) || !Normalize (planeNormal, normal))
        return false;
    if (std::fabs (Dot (firstDirection, normal)) > kPlanarTolerance ||
        std::fabs (Dot (secondDirection, normal)) > kPlanarTolerance)
        return false;

    const double sine = Dot (normal, Cross (firstDirection, secondDirection));
    const double cosine = std::clamp (Dot (firstDirection, secondDirection), -1.0, 1.0);
    double sweep = std::atan2 (sine, cosine);
    if (std::fabs (sweep) <= kMinLength)
        return false;
    if (std::fabs (sine) <= kMinLength && cosine < 0.0)
        sweep = kPi;

    std::vector<Point3> sampled;
    sampled.reserve (segmentCount + 1);
    for (size_t index = 0; index <= segmentCount; ++index) {
        const double angle = sweep * static_cast<double> (index) / static_cast<double> (segmentCount);
        const Point3 radial =
            Add (Scale (firstDirection, std::cos (angle)), Scale (Cross (normal, firstDirection), std::sin (angle)));
        const Point3 point = Add (center, Scale (radial, radius));
        if (!IsFinite (point))
            return false;
        sampled.push_back (point);
    }

    outPoints.insert (outPoints.end (), sampled.begin (), sampled.end ());
    return true;
}

} // namespace annotation
} // namespace geomsrv
