#include "ArchViz/TraceAnnotationLayer.hpp"

#include "ArchViz/MatrixMath.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>
#include <numeric>

namespace geomsrv::archviz {
namespace {

using annotation::Frame;
using annotation::Primitive;
using annotation::PrimitiveKind;
using annotation::SemanticRole;

constexpr float kArrowLength = 10.0f;
constexpr float kArrowWidth = 5.0f;
constexpr float kDimensionOffset = 14.0f;
constexpr float kBaseFontPixels = 18.0f;

bool PointAt (const Primitive& primitive, std::size_t index, double out[3])
{
    if (index >= primitive.points.size ())
        return false;
    out[0] = primitive.points[index].x;
    out[1] = primitive.points[index].y;
    out[2] = primitive.points[index].z;
    return std::isfinite (out[0]) && std::isfinite (out[1]) && std::isfinite (out[2]);
}

bool Project (const double point[3], const float viewProj[16], uint32_t width, uint32_t height, ScreenPoint& screen,
              float* depth = nullptr)
{
    const float input[4] = { float (point[0]), float (point[1]), float (point[2]), 1.0f };
    float clip[4];
    TransformPoint (clip, input, viewProj);
    if (!std::isfinite (clip[0]) || !std::isfinite (clip[1]) || !std::isfinite (clip[2]) || !std::isfinite (clip[3]) ||
        clip[3] <= 1.0e-6f || clip[2] < 0.0f || clip[2] > clip[3])
        return false;
    const float ndcX = clip[0] / clip[3];
    const float ndcY = clip[1] / clip[3];
    screen = { (ndcX * 0.5f + 0.5f) * float (width), (0.5f - ndcY * 0.5f) * float (height) };
    if (depth != nullptr)
        *depth = clip[2] / clip[3];
    return std::isfinite (screen.x) && std::isfinite (screen.y);
}

void AddLine (ProjectedDrawList& out, const ScreenPoint& from, const ScreenPoint& to, uint32_t color,
              float width = 2.0f, bool collisionObstacle = true)
{
    out.lines.push_back ({ from, to, color, width, collisionObstacle });
}

void AddArrowhead (ProjectedDrawList& out, const ScreenPoint& tail, const ScreenPoint& tip, uint32_t color,
                   float dpiScale)
{
    const float dx = tip.x - tail.x;
    const float dy = tip.y - tail.y;
    const float length = std::sqrt (dx * dx + dy * dy);
    if (length <= 1.0e-3f)
        return;
    const float ux = dx / length;
    const float uy = dy / length;
    const ScreenPoint base { tip.x - ux * kArrowLength * dpiScale, tip.y - uy * kArrowLength * dpiScale };
    out.triangles.push_back ({ { tip,
                                 { base.x - uy * kArrowWidth * dpiScale, base.y + ux * kArrowWidth * dpiScale },
                                 { base.x + uy * kArrowWidth * dpiScale, base.y - ux * kArrowWidth * dpiScale } },
                               color });
}

bool ProjectPoint (const Primitive& primitive, std::size_t index, const float viewProj[16], uint32_t width,
                   uint32_t height, ScreenPoint& screen, float* depth = nullptr)
{
    double point[3];
    return PointAt (primitive, index, point) && Project (point, viewProj, width, height, screen, depth);
}

bool ModelAnnotationScale (const Primitive& primitive, const float viewProj[16], uint32_t width, uint32_t height,
                           float textHeightMetres, float hideBelowPixels, float capAbovePixels, float& fontPixels,
                           float& furnitureScale)
{
    if (primitive.points.empty ())
        return false;
    double anchor[3] = {};
    for (const annotation::Point3& point : primitive.points) {
        anchor[0] += point.x;
        anchor[1] += point.y;
        anchor[2] += point.z;
    }
    const double pointCount = double (primitive.points.size ());
    anchor[0] /= pointCount;
    anchor[1] /= pointCount;
    anchor[2] /= pointCount;

    ScreenPoint projectedAnchor;
    if (!Project (anchor, viewProj, width, height, projectedAnchor))
        return false;
    constexpr double modelStep = 0.01;
    float horizontalSquared = 0.0f;
    float crossProduct = 0.0f;
    float verticalSquared = 0.0f;
    for (int axis = 0; axis < 3; ++axis) {
        double displaced[3] = { anchor[0], anchor[1], anchor[2] };
        displaced[axis] += modelStep;
        ScreenPoint projectedDisplaced;
        if (Project (displaced, viewProj, width, height, projectedDisplaced)) {
            const float horizontal = (projectedDisplaced.x - projectedAnchor.x) / float (modelStep);
            const float vertical = (projectedDisplaced.y - projectedAnchor.y) / float (modelStep);
            horizontalSquared += horizontal * horizontal;
            crossProduct += horizontal * vertical;
            verticalSquared += vertical * vertical;
        }
    }
    const float trace = horizontalSquared + verticalSquared;
    const float discriminant = std::hypot (horizontalSquared - verticalSquared, 2.0f * crossProduct);
    // The largest singular value of the local projection Jacobian is the projected
    // size of one model metre without making the result depend on camera roll.
    const float pixelsPerMetre = std::sqrt (std::max (0.0f, 0.5f * (trace + discriminant)));
    const float projectedPixels = textHeightMetres * pixelsPerMetre;
    if (!std::isfinite (projectedPixels) || projectedPixels < hideBelowPixels)
        return false;
    fontPixels = std::min (projectedPixels, capAbovePixels);
    furnitureScale = fontPixels / kBaseFontPixels;
    return fontPixels > 0.0f;
}

void FadeLabelWhenOccluded (ScreenLabel& label, float depth)
{
    label.depthAnchor = label.anchor;
    label.anchorDepth = depth;
    label.fadeWhenOccluded = true;
}

void AddText (ProjectedDrawList& out, const Primitive& primitive, const ScreenPoint& anchor, uint32_t color,
              const std::string& fallback = {}, float fontSize = 0.0f, bool centered = false,
              bool backgroundPanel = true, uint32_t haloRgba = 0, float haloWidthPixels = 0.0f)
{
    const std::string text = primitive.text.empty () ? fallback : primitive.text;
    if (!text.empty ())
        out.labels.push_back ({ anchor, text, color, fontSize, centered, haloRgba, haloWidthPixels, backgroundPanel });
}

std::size_t Utf8CodepointCount (std::string_view text)
{
    return std::count_if (text.begin (), text.end (),
                          [] (char value) { return (static_cast<unsigned char> (value) & 0xC0u) != 0x80u; });
}

ScreenTextExtent MeasureText (std::string_view text, float fontSize, const ScreenTextMeasure& measureText)
{
    ScreenTextExtent extent;
    if (measureText && measureText (text, fontSize, extent) && std::isfinite (extent.width) &&
        std::isfinite (extent.height) && extent.width >= 0.0f && extent.height > 0.0f)
        return extent;
    return { std::max (fontSize * 0.56f * float (Utf8CodepointCount (text)), fontSize * 0.5f), fontSize };
}

float EdgeClearance (const ScreenPoint& first, const ScreenPoint& second, const ScreenPoint& labelCenter,
                     const ScreenTextExtent& label, uint32_t width, uint32_t height)
{
    return std::min ({ first.x, second.x, labelCenter.x - label.width * 0.5f, float (width) - first.x,
                       float (width) - second.x, float (width) - labelCenter.x - label.width * 0.5f, first.y, second.y,
                       labelCenter.y - label.height * 0.5f, float (height) - first.y, float (height) - second.y,
                       float (height) - labelCenter.y - label.height * 0.5f });
}

float LabelOverflow (const ScreenPoint& center, const ScreenTextExtent& extent, uint32_t width, uint32_t height,
                     float inset)
{
    const float left = center.x - extent.width * 0.5f - inset;
    const float right = center.x + extent.width * 0.5f + inset;
    const float top = center.y - extent.height * 0.5f - inset;
    const float bottom = center.y + extent.height * 0.5f + inset;
    return std::max (0.0f, inset - left) + std::max (0.0f, right - (float (width) - inset)) +
           std::max (0.0f, inset - top) + std::max (0.0f, bottom - (float (height) - inset));
}

float ReadableRotation (float radians)
{
    constexpr float halfPi = 1.57079632679489661923f;
    constexpr float pi = 3.14159265358979323846f;
    while (radians > pi)
        radians -= 2.0f * pi;
    while (radians <= -pi)
        radians += 2.0f * pi;
    if (radians > halfPi)
        radians -= pi;
    else if (radians < -halfPi)
        radians += pi;
    return radians;
}

void AddLineTrimmedAgainstLabel (ProjectedDrawList& out, const ScreenPoint& from, const ScreenPoint& to,
                                 const ScreenPoint& labelCenter, const ScreenTextExtent& label, float padding,
                                 uint32_t color, float lineWidth)
{
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float length = std::hypot (dx, dy);
    if (length <= 1.0e-3f)
        return;
    const float ux = dx / length;
    const float uy = dy / length;
    const float centerDistance = (labelCenter.x - from.x) * ux + (labelCenter.y - from.y) * uy;
    const float halfGap =
        std::fabs (ux) * (label.width * 0.5f + padding) + std::fabs (uy) * (label.height * 0.5f + padding);
    const float before = std::clamp (centerDistance - halfGap, 0.0f, length);
    const float after = std::clamp (centerDistance + halfGap, 0.0f, length);
    if (before > 1.0e-3f)
        AddLine (out, from, { from.x + ux * before, from.y + uy * before }, color, lineWidth);
    if (after < length - 1.0e-3f)
        AddLine (out, { from.x + ux * after, from.y + uy * after }, to, color, lineWidth);
}

struct LabelBox {
    ScreenPoint center;
    float halfWidth;
    float halfHeight;
    float rotationRadians;
};

float ProjectionRadius (const LabelBox& box, const ScreenPoint& axis)
{
    const float cosine = std::cos (box.rotationRadians);
    const float sine = std::sin (box.rotationRadians);
    const ScreenPoint along { cosine, sine };
    const ScreenPoint across { -sine, cosine };
    return box.halfWidth * std::fabs (along.x * axis.x + along.y * axis.y) +
           box.halfHeight * std::fabs (across.x * axis.x + across.y * axis.y);
}

bool Overlaps (const LabelBox& left, const LabelBox& right, float gap)
{
    const ScreenPoint delta { right.center.x - left.center.x, right.center.y - left.center.y };
    const float leftCosine = std::cos (left.rotationRadians);
    const float leftSine = std::sin (left.rotationRadians);
    const float rightCosine = std::cos (right.rotationRadians);
    const float rightSine = std::sin (right.rotationRadians);
    const ScreenPoint axes[] = {
        { leftCosine, leftSine },
        { -leftSine, leftCosine },
        { rightCosine, rightSine },
        { -rightSine, rightCosine },
    };
    return std::all_of (std::begin (axes), std::end (axes), [&] (const ScreenPoint& axis) {
        const float distance = std::fabs (delta.x * axis.x + delta.y * axis.y);
        return distance < ProjectionRadius (left, axis) + ProjectionRadius (right, axis) + gap;
    });
}

bool Intersects (const ScreenLine& line, const LabelBox& box, float gap)
{
    const float cosine = std::cos (box.rotationRadians);
    const float sine = std::sin (box.rotationRadians);
    const auto local = [&] (const ScreenPoint& point) {
        const float x = point.x - box.center.x;
        const float y = point.y - box.center.y;
        return ScreenPoint { x * cosine + y * sine, -x * sine + y * cosine };
    };
    const ScreenPoint from = local (line.from);
    const ScreenPoint to = local (line.to);
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float halfWidth = box.halfWidth + gap;
    const float halfHeight = box.halfHeight + gap;
    float minimum = 0.0f;
    float maximum = 1.0f;
    const auto clip = [&] (float start, float delta, float extent) {
        if (std::fabs (delta) <= 1.0e-6f)
            return std::fabs (start) <= extent;
        float first = (-extent - start) / delta;
        float second = (extent - start) / delta;
        if (first > second)
            std::swap (first, second);
        minimum = std::max (minimum, first);
        maximum = std::min (maximum, second);
        return minimum <= maximum;
    };
    return clip (from.x, dx, halfWidth) && clip (from.y, dy, halfHeight);
}

float PointSegmentDistanceSquared (const ScreenPoint& point, const ScreenPoint& first, const ScreenPoint& second)
{
    const float dx = second.x - first.x;
    const float dy = second.y - first.y;
    const float lengthSquared = dx * dx + dy * dy;
    const float parameter =
        lengthSquared > 1.0e-6f
            ? std::clamp (((point.x - first.x) * dx + (point.y - first.y) * dy) / lengthSquared, 0.0f, 1.0f)
            : 0.0f;
    const float nearestX = first.x + parameter * dx;
    const float nearestY = first.y + parameter * dy;
    const float distanceX = point.x - nearestX;
    const float distanceY = point.y - nearestY;
    return distanceX * distanceX + distanceY * distanceY;
}

void ResolveLabelOverlaps (ProjectedDrawList& out, uint32_t width, uint32_t height, float dpiScale,
                           const ScreenTextMeasure& measureText, AnnotationPlacementHistory* placementHistory)
{
    std::vector<LabelBox> occupied;
    occupied.reserve (out.labels.size ());
    const float inset = 4.0f * dpiScale;
    for (ScreenLabel& label : out.labels) {
        const float fontSize = label.fontSize > 0.0f ? label.fontSize : 18.0f * dpiScale;
        const float labelScale = fontSize / kBaseFontPixels;
        const float gap = 5.0f * labelScale;
        const ScreenTextExtent extent = MeasureText (label.text, fontSize, measureText);
        const ScreenPoint original = label.anchor;
        const ScreenPoint originalCenter { label.centered ? original.x : original.x + extent.width * 0.5f,
                                           label.centered ? original.y + extent.height * 0.5f
                                                          : original.y - extent.height * 0.5f };
        const float cosine = std::cos (label.rotationRadians);
        const float sine = std::sin (label.rotationRadians);
        const ScreenPoint along { cosine, sine };
        const ScreenPoint across { -sine, cosine };
        const float stepAlong = extent.width + 7.0f * labelScale;
        const float stepAcross = extent.height + 7.0f * labelScale;
        const ScreenPoint offsets[] = {
            { 0.0f, 0.0f },
            { across.x * stepAcross, across.y * stepAcross },
            { -across.x * stepAcross, -across.y * stepAcross },
            { along.x * stepAlong, along.y * stepAlong },
            { -along.x * stepAlong, -along.y * stepAlong },
            { across.x * stepAcross + along.x * stepAlong, across.y * stepAcross + along.y * stepAlong },
            { across.x * stepAcross - along.x * stepAlong, across.y * stepAcross - along.y * stepAlong },
            { -across.x * stepAcross + along.x * stepAlong, -across.y * stepAcross + along.y * stepAlong },
            { -across.x * stepAcross - along.x * stepAlong, -across.y * stepAcross - along.y * stepAlong },
            { across.x * 2.0f * stepAcross, across.y * 2.0f * stepAcross },
            { -across.x * 2.0f * stepAcross, -across.y * 2.0f * stepAcross },
        };
        std::array<std::size_t, std::size (offsets)> candidateOrder;
        std::iota (candidateOrder.begin (), candidateOrder.end (), 0);
        if (placementHistory != nullptr) {
            const auto previous = placementHistory->candidateByPrimitive.find (label.sourcePrimitive);
            if (previous != placementHistory->candidateByPrimitive.end () && previous->second < candidateOrder.size ())
                std::swap (candidateOrder[0], candidateOrder[previous->second]);
        }
        bool placed = false;
        for (int pass = 0; pass < 2 && !placed; ++pass) {
            for (const std::size_t candidateIndex : candidateOrder) {
                const ScreenPoint& offset = offsets[candidateIndex];
                const ScreenPoint anchor { original.x + offset.x, original.y + offset.y };
                const ScreenPoint center { originalCenter.x + offset.x, originalCenter.y + offset.y };
                const LabelBox candidate { center, extent.width * 0.5f, extent.height * 0.5f, label.rotationRadians };
                const float aabbHalfWidth =
                    std::fabs (cosine) * candidate.halfWidth + std::fabs (sine) * candidate.halfHeight;
                const float aabbHalfHeight =
                    std::fabs (sine) * candidate.halfWidth + std::fabs (cosine) * candidate.halfHeight;
                if (center.x - aabbHalfWidth < inset || center.y - aabbHalfHeight < inset ||
                    center.x + aabbHalfWidth > float (width) - inset ||
                    center.y + aabbHalfHeight > float (height) - inset)
                    continue;
                if (std::any_of (occupied.begin (), occupied.end (),
                                 [&] (const LabelBox& item) { return Overlaps (candidate, item, gap); }))
                    continue;
                if (pass == 0 && !label.backgroundPanel) {
                    bool intersectsGeometry = false;
                    for (std::size_t lineIndex = 0; lineIndex < out.lines.size (); ++lineIndex) {
                        if (lineIndex >= label.ownLineBegin && lineIndex < label.ownLineEnd)
                            continue;
                        if (out.lines[lineIndex].collisionObstacle &&
                            Intersects (out.lines[lineIndex], candidate, 2.0f * labelScale)) {
                            intersectsGeometry = true;
                            break;
                        }
                    }
                    if (intersectsGeometry)
                        continue;
                }
                label.anchor = anchor;
                occupied.push_back (candidate);
                placed = true;
                if (placementHistory != nullptr)
                    placementHistory->candidateByPrimitive[label.sourcePrimitive] =
                        static_cast<uint8_t> (candidateIndex);
                break;
            }
        }
        if (!placed) {
            occupied.push_back ({ originalCenter, extent.width * 0.5f, extent.height * 0.5f, label.rotationRadians });
        }
    }
}

void AddTrimmedPath (ProjectedDrawList& out, const std::vector<ScreenPoint>& path, const std::vector<float>& distances,
                     float gapStart, float gapEnd, uint32_t color, float lineWidth)
{
    for (std::size_t index = 1; index < path.size (); ++index) {
        const float start = distances[index - 1];
        const float end = distances[index];
        if (end <= gapStart || start >= gapEnd) {
            AddLine (out, path[index - 1], path[index], color, lineWidth);
            continue;
        }
        const float length = end - start;
        if (length <= 1.0e-3f)
            continue;
        const auto pointAt = [&] (float distance) {
            const float parameter = std::clamp ((distance - start) / length, 0.0f, 1.0f);
            return ScreenPoint { path[index - 1].x + (path[index].x - path[index - 1].x) * parameter,
                                 path[index - 1].y + (path[index].y - path[index - 1].y) * parameter };
        };
        if (start < gapStart)
            AddLine (out, path[index - 1], pointAt (gapStart), color, lineWidth);
        if (end > gapEnd)
            AddLine (out, pointAt (gapEnd), path[index], color, lineWidth);
    }
}

void AddArcDimension (ProjectedDrawList& out, const Primitive& primitive, const float viewProj[16], uint32_t width,
                      uint32_t height, uint32_t color, float furnitureScale, const ScreenTextMeasure& measureText,
                      float fontSizePixels)
{
    const std::size_t ownLineBegin = out.lines.size ();
    annotation::Point3 center;
    double radius = 0.0;
    double direction = 0.0;
    if (!annotation::FitCircularPath (primitive, center, radius, direction))
        return;
    const double dimensionRadius = radius - direction * primitive.offset;
    if (dimensionRadius <= 1.0e-12)
        return;
    std::vector<ScreenPoint> sourcePath;
    std::vector<ScreenPoint> dimensionPath;
    std::vector<float> dimensionDepths;
    sourcePath.reserve (primitive.points.size ());
    dimensionPath.reserve (primitive.points.size ());
    dimensionDepths.reserve (primitive.points.size ());
    for (const annotation::Point3& point : primitive.points) {
        const double source[3] = { point.x, point.y, point.z };
        const double radialX = point.x - center.x;
        const double radialY = point.y - center.y;
        const double dimension[3] = { center.x + radialX * dimensionRadius / radius,
                                      center.y + radialY * dimensionRadius / radius, point.z };
        ScreenPoint projectedSource;
        ScreenPoint projectedDimension;
        float dimensionDepth = 1.0f;
        if (!Project (source, viewProj, width, height, projectedSource) ||
            !Project (dimension, viewProj, width, height, projectedDimension, &dimensionDepth))
            return;
        sourcePath.push_back (projectedSource);
        dimensionPath.push_back (projectedDimension);
        dimensionDepths.push_back (dimensionDepth);
    }
    if (primitive.offset == 0.0) {
        const std::vector<ScreenPoint> unoffset = dimensionPath;
        for (std::size_t index = 0; index < dimensionPath.size (); ++index) {
            const std::size_t before = index == 0 ? 0 : index - 1;
            const std::size_t after = index + 1 < dimensionPath.size () ? index + 1 : index;
            const float tangentX = unoffset[after].x - unoffset[before].x;
            const float tangentY = unoffset[after].y - unoffset[before].y;
            const float tangentLength = std::hypot (tangentX, tangentY);
            if (tangentLength <= 1.0e-3f)
                return;
            dimensionPath[index].x -= tangentY / tangentLength * kDimensionOffset * furnitureScale;
            dimensionPath[index].y += tangentX / tangentLength * kDimensionOffset * furnitureScale;
        }
    }
    std::vector<float> distances (dimensionPath.size (), 0.0f);
    for (std::size_t index = 1; index < dimensionPath.size (); ++index)
        distances[index] = distances[index - 1] + std::hypot (dimensionPath[index].x - dimensionPath[index - 1].x,
                                                              dimensionPath[index].y - dimensionPath[index - 1].y);
    const float screenLength = distances.back ();
    if (screenLength <= 1.0e-3f)
        return;
    const float midpoint = screenLength * 0.5f;
    const auto upper = std::upper_bound (distances.begin (), distances.end (), midpoint);
    const std::size_t segment =
        std::clamp<std::size_t> (std::size_t (upper - distances.begin ()), 1, dimensionPath.size () - 1);
    const float segmentLength = distances[segment] - distances[segment - 1];
    const float parameter = segmentLength > 1.0e-3f ? (midpoint - distances[segment - 1]) / segmentLength : 0.0f;
    ScreenPoint labelCenter {
        dimensionPath[segment - 1].x + (dimensionPath[segment].x - dimensionPath[segment - 1].x) * parameter,
        dimensionPath[segment - 1].y + (dimensionPath[segment].y - dimensionPath[segment - 1].y) * parameter,
    };
    float tangentX = dimensionPath[segment].x - dimensionPath[segment - 1].x;
    float tangentY = dimensionPath[segment].y - dimensionPath[segment - 1].y;
    float tangentLength = std::hypot (tangentX, tangentY);
    if (tangentLength <= 1.0e-3f)
        return;
    const float fontSize = fontSizePixels;
    const std::string text = primitive.text;
    const ScreenTextExtent textExtent = MeasureText (text, fontSize, measureText);
    const float padding = 4.0f * furnitureScale;
    const bool fitsInside = screenLength >= textExtent.width + 2.0f * (kArrowLength + 3.0f) * furnitureScale;
    if (!fitsInside) {
        const ScreenPoint startTangent { dimensionPath[1].x - dimensionPath[0].x,
                                         dimensionPath[1].y - dimensionPath[0].y };
        const ScreenPoint endTangent { dimensionPath.back ().x - dimensionPath[dimensionPath.size () - 2].x,
                                       dimensionPath.back ().y - dimensionPath[dimensionPath.size () - 2].y };
        const float startLength = std::hypot (startTangent.x, startTangent.y);
        const float endLength = std::hypot (endTangent.x, endTangent.y);
        if (startLength <= 1.0e-3f || endLength <= 1.0e-3f)
            return;
        const float labelDistance = (kArrowLength + 2.0f) * furnitureScale + textExtent.width * 0.5f + padding;
        const ScreenPoint before { dimensionPath.front ().x - startTangent.x / startLength * labelDistance,
                                   dimensionPath.front ().y - startTangent.y / startLength * labelDistance };
        const ScreenPoint after { dimensionPath.back ().x + endTangent.x / endLength * labelDistance,
                                  dimensionPath.back ().y + endTangent.y / endLength * labelDistance };
        if (LabelOverflow (before, textExtent, width, height, padding) <=
            LabelOverflow (after, textExtent, width, height, padding)) {
            labelCenter = before;
            tangentX = startTangent.x;
            tangentY = startTangent.y;
            tangentLength = startLength;
        }
        else {
            labelCenter = after;
            tangentX = endTangent.x;
            tangentY = endTangent.y;
            tangentLength = endLength;
        }
    }
    AddLine (out, sourcePath.front (), dimensionPath.front (), color, 2.0f * furnitureScale);
    AddLine (out, sourcePath.back (), dimensionPath.back (), color, 2.0f * furnitureScale);
    if (fitsInside)
        AddTrimmedPath (out, dimensionPath, distances, midpoint - textExtent.width * 0.5f - padding,
                        midpoint + textExtent.width * 0.5f + padding, color, 2.0f * furnitureScale);
    else
        AddTrimmedPath (out, dimensionPath, distances, -1.0f, -1.0f, color, 2.0f * furnitureScale);
    AddArrowhead (out, dimensionPath[1], dimensionPath.front (), color, furnitureScale);
    AddArrowhead (out, dimensionPath[dimensionPath.size () - 2], dimensionPath.back (), color, furnitureScale);
    if (!text.empty ()) {
        out.labels.push_back ({ { labelCenter.x, labelCenter.y - textExtent.height * 0.5f },
                                text,
                                color,
                                fontSize,
                                true,
                                0xFFFFFF48u,
                                0.25f * furnitureScale,
                                false });
        out.labels.back ().rotationRadians = ReadableRotation (std::atan2 (tangentY, tangentX));
        out.labels.back ().ownLineBegin = ownLineBegin;
        out.labels.back ().ownLineEnd = out.lines.size ();
        FadeLabelWhenOccluded (out.labels.back (),
                               dimensionDepths[segment - 1] +
                                   (dimensionDepths[segment] - dimensionDepths[segment - 1]) * parameter);
    }
}

void AddDimension (ProjectedDrawList& out, const Primitive& primitive, const float viewProj[16], uint32_t width,
                   uint32_t height, uint32_t color, float furnitureScale, const ScreenTextMeasure& measureText,
                   float fontSizePixels)
{
    if (primitive.points.size () > 2) {
        AddArcDimension (out, primitive, viewProj, width, height, color, furnitureScale, measureText, fontSizePixels);
        return;
    }
    const std::size_t ownLineBegin = out.lines.size ();
    ScreenPoint a, b;
    double worldA[3], worldB[3];
    if (!PointAt (primitive, 0, worldA) || !PointAt (primitive, 1, worldB) ||
        !Project (worldA, viewProj, width, height, a) || !Project (worldB, viewProj, width, height, b))
        return;
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float screenLength = std::sqrt (dx * dx + dy * dy);
    if (screenLength <= 1.0e-3f)
        return;
    const ScreenPoint projectedUnit { dx / screenLength, dy / screenLength };
    char measured[64];
    const double wx = worldB[0] - worldA[0], wy = worldB[1] - worldA[1], wz = worldB[2] - worldA[2];
    std::snprintf (measured, sizeof (measured), "%.3f m", std::sqrt (wx * wx + wy * wy + wz * wz));
    const std::string text = primitive.text.empty () ? measured : primitive.text;
    const float fontSize = fontSizePixels;
    const ScreenTextExtent textExtent = MeasureText (text, fontSize, measureText);
    ScreenPoint da;
    ScreenPoint db;
    double depthWorld[3] = { (worldA[0] + worldB[0]) * 0.5, (worldA[1] + worldB[1]) * 0.5,
                             (worldA[2] + worldB[2]) * 0.5 };
    if (primitive.offset != 0.0) {
        const double planX = worldB[0] - worldA[0], planY = worldB[1] - worldA[1];
        const double planLength = std::sqrt (planX * planX + planY * planY);
        if (planLength <= 1.0e-12)
            return;
        const double ox = -planY / planLength * primitive.offset;
        const double oy = planX / planLength * primitive.offset;
        const double dimensionA[3] = { worldA[0] + ox, worldA[1] + oy, worldA[2] };
        const double dimensionB[3] = { worldB[0] + ox, worldB[1] + oy, worldB[2] };
        depthWorld[0] += ox;
        depthWorld[1] += oy;
        if (!Project (dimensionA, viewProj, width, height, da) || !Project (dimensionB, viewProj, width, height, db))
            return;
    }
    else {
        const ScreenPoint normal { -projectedUnit.y * kDimensionOffset * furnitureScale,
                                   projectedUnit.x * kDimensionOffset * furnitureScale };
        const ScreenPoint positiveA { a.x + normal.x, a.y + normal.y };
        const ScreenPoint positiveB { b.x + normal.x, b.y + normal.y };
        const ScreenPoint negativeA { a.x - normal.x, a.y - normal.y };
        const ScreenPoint negativeB { b.x - normal.x, b.y - normal.y };
        const ScreenPoint positiveCenter { (positiveA.x + positiveB.x) * 0.5f, (positiveA.y + positiveB.y) * 0.5f };
        const ScreenPoint negativeCenter { (negativeA.x + negativeB.x) * 0.5f, (negativeA.y + negativeB.y) * 0.5f };
        if (EdgeClearance (positiveA, positiveB, positiveCenter, textExtent, width, height) >=
            EdgeClearance (negativeA, negativeB, negativeCenter, textExtent, width, height)) {
            da = positiveA;
            db = positiveB;
        }
        else {
            da = negativeA;
            db = negativeB;
        }
    }
    AddLine (out, a, da, color, 2.0f * furnitureScale);
    AddLine (out, b, db, color, 2.0f * furnitureScale);
    const float dimensionDx = db.x - da.x;
    const float dimensionDy = db.y - da.y;
    const float dimensionLength = std::hypot (dimensionDx, dimensionDy);
    if (dimensionLength <= 1.0e-3f)
        return;
    const ScreenPoint unit { dimensionDx / dimensionLength, dimensionDy / dimensionLength };
    const float panelPadding = 4.0f * furnitureScale;
    const float textAlongLine = textExtent.width;
    const bool fitsInside = dimensionLength >= textAlongLine + 2.0f * (kArrowLength + 3.0f) * furnitureScale;
    ScreenPoint labelCenter;
    ScreenPoint lineFrom = da;
    ScreenPoint lineTo = db;
    if (fitsInside) {
        labelCenter = { (da.x + db.x) * 0.5f, (da.y + db.y) * 0.5f };
        AddArrowhead (out, db, da, color, furnitureScale);
        AddArrowhead (out, da, db, color, furnitureScale);
    }
    else {
        const float labelDistance = (kArrowLength + 2.0f) * furnitureScale + textAlongLine * 0.5f + panelPadding;
        const ScreenPoint before { da.x - unit.x * labelDistance, da.y - unit.y * labelDistance };
        const ScreenPoint after { db.x + unit.x * labelDistance, db.y + unit.y * labelDistance };
        const bool useBefore = LabelOverflow (before, textExtent, width, height, panelPadding) <=
                               LabelOverflow (after, textExtent, width, height, panelPadding);
        labelCenter = useBefore ? before : after;
        const float overhang = (kArrowLength + 4.0f) * furnitureScale + textAlongLine + panelPadding * 2.0f;
        if (useBefore)
            lineFrom = { da.x - unit.x * overhang, da.y - unit.y * overhang };
        else
            lineTo = { db.x + unit.x * overhang, db.y + unit.y * overhang };
        AddArrowhead (out, { da.x + unit.x, da.y + unit.y }, da, color, furnitureScale);
        AddArrowhead (out, { db.x - unit.x, db.y - unit.y }, db, color, furnitureScale);
    }
    AddLineTrimmedAgainstLabel (out, lineFrom, lineTo, labelCenter, textExtent, panelPadding, color,
                                2.0f * furnitureScale);
    if (!text.empty ())
        out.labels.push_back ({ { labelCenter.x, labelCenter.y - textExtent.height * 0.5f },
                                text,
                                color,
                                fontSize,
                                true,
                                0xFFFFFF48u,
                                0.25f * furnitureScale,
                                false });
    if (!text.empty ()) {
        out.labels.back ().rotationRadians = ReadableRotation (std::atan2 (dimensionDy, dimensionDx));
        out.labels.back ().ownLineBegin = ownLineBegin;
        out.labels.back ().ownLineEnd = out.lines.size ();
        ScreenPoint unused;
        float depth = 1.0f;
        if (Project (depthWorld, viewProj, width, height, unused, &depth))
            FadeLabelWhenOccluded (out.labels.back (), depth);
    }
}

double ModelAngleDegrees (const Primitive& primitive)
{
    if (primitive.points.size () < 3)
        return 0.0;
    const annotation::Point3& center = primitive.points[0];
    const annotation::Point3& first = primitive.points[1];
    const annotation::Point3& second = primitive.points[2];
    const double ax = first.x - center.x, ay = first.y - center.y, az = first.z - center.z;
    const double bx = second.x - center.x, by = second.y - center.y, bz = second.z - center.z;
    const double aLength = std::sqrt (ax * ax + ay * ay + az * az);
    const double bLength = std::sqrt (bx * bx + by * by + bz * bz);
    if (aLength <= 1.0e-12 || bLength <= 1.0e-12)
        return 0.0;
    const double cosine = std::clamp ((ax * bx + ay * by + az * bz) / (aLength * bLength), -1.0, 1.0);
    return std::acos (cosine) * 180.0 / 3.14159265358979323846;
}

void AddAngle (ProjectedDrawList& out, const Primitive& primitive, const float viewProj[16], uint32_t width,
               uint32_t height, uint32_t color, float furnitureScale, const ScreenTextMeasure& measureText,
               float fontSizePixels)
{
    const std::size_t ownLineBegin = out.lines.size ();
    ScreenPoint center, first, second;
    float centerDepth = 1.0f;
    if (!ProjectPoint (primitive, 0, viewProj, width, height, center, &centerDepth) ||
        !ProjectPoint (primitive, 1, viewProj, width, height, first) ||
        !ProjectPoint (primitive, 2, viewProj, width, height, second))
        return;
    if (primitive.direction)
        std::swap (first, second);
    annotation::ArchitecturalAngleGlyph glyph;
    if (!annotation::BuildArchitecturalAngleGlyph ({ center.x, center.y }, { first.x, first.y }, { second.x, second.y },
                                                   furnitureScale, glyph))
        return;
    for (std::size_t index = 1; index < glyph.arc.size (); ++index)
        AddLine (out, { float (glyph.arc[index - 1].x), float (glyph.arc[index - 1].y) },
                 { float (glyph.arc[index].x), float (glyph.arc[index].y) }, color, float (glyph.arcWidthPixels));
    for (const annotation::ScreenTriangle& arrowhead : glyph.arrowheads)
        out.triangles.push_back ({ { { float (arrowhead.first.x), float (arrowhead.first.y) },
                                     { float (arrowhead.second.x), float (arrowhead.second.y) },
                                     { float (arrowhead.third.x), float (arrowhead.third.y) } },
                                   color });
    char measured[64];
    std::snprintf (measured, sizeof (measured), "%.1f\xC2\xB0", ModelAngleDegrees (primitive));
    const std::string text = primitive.text.empty () ? measured : primitive.text;
    const float fontSize = fontSizePixels;
    const ScreenTextExtent textExtent = MeasureText (text, fontSize, measureText);
    const float radialX = float (glyph.labelAnchor.x) - center.x;
    const float radialY = float (glyph.labelAnchor.y) - center.y;
    const float radialLength = std::hypot (radialX, radialY);
    if (radialLength <= 1.0e-3f)
        return;
    const float arcRadius =
        std::hypot (float (glyph.arc.front ().x) - center.x, float (glyph.arc.front ().y) - center.y);
    const float labelRadius = std::max (radialLength, arcRadius + textExtent.width * 0.5f + 5.0f * furnitureScale);
    const ScreenPoint labelCenter { center.x + radialX / radialLength * labelRadius,
                                    center.y + radialY / radialLength * labelRadius };
    AddText (out, primitive, { labelCenter.x, labelCenter.y - textExtent.height * 0.5f }, color, measured, fontSize,
             true, false, 0xFFFFFF48u, 0.25f * furnitureScale);
    ScreenLabel& label = out.labels.back ();
    label.rotationRadians = ReadableRotation (std::atan2 (radialY, radialX));
    label.ownLineBegin = ownLineBegin;
    label.ownLineEnd = out.lines.size ();
    FadeLabelWhenOccluded (label, centerDepth);
}

} // namespace

bool FitFrameProjection (const Frame& frame, const float viewProj[16], uint32_t width, uint32_t height,
                         float marginPixels, float fittedViewProj[16])
{
    if (width == 0 || height == 0 || !std::isfinite (marginPixels) || marginPixels < 0.0f ||
        float (width) <= 2.0f * marginPixels || float (height) <= 2.0f * marginPixels)
        return false;
    bool hasPoint = false;
    float minimumX = 0.0f, minimumY = 0.0f, maximumX = 0.0f, maximumY = 0.0f;
    for (const Primitive& primitive : frame.primitives) {
        if (!annotation::IsDrawable (primitive) || primitive.kind == PrimitiveKind::Element)
            continue;
        for (const annotation::Point3& point : primitive.points) {
            const double values[3] = { point.x, point.y, point.z };
            ScreenPoint projected;
            if (!Project (values, viewProj, width, height, projected))
                continue;
            if (!hasPoint) {
                minimumX = maximumX = projected.x;
                minimumY = maximumY = projected.y;
                hasPoint = true;
            }
            else {
                minimumX = std::min (minimumX, projected.x);
                minimumY = std::min (minimumY, projected.y);
                maximumX = std::max (maximumX, projected.x);
                maximumY = std::max (maximumY, projected.y);
            }
        }
    }
    if (!hasPoint)
        return false;
    const float extentX = std::max (maximumX - minimumX, 1.0e-3f);
    const float extentY = std::max (maximumY - minimumY, 1.0e-3f);
    const float scale =
        std::min ((float (width) - 2.0f * marginPixels) / extentX, (float (height) - 2.0f * marginPixels) / extentY);
    const float offsetX = float (width) * 0.5f - (minimumX + maximumX) * 0.5f * scale;
    const float offsetY = float (height) * 0.5f - (minimumY + maximumY) * 0.5f * scale;
    std::copy (viewProj, viewProj + 16, fittedViewProj);
    const float translateX = scale + 2.0f * offsetX / float (width) - 1.0f;
    const float translateY = 1.0f - scale - 2.0f * offsetY / float (height);
    for (int row = 0; row < 4; ++row) {
        fittedViewProj[row * 4] = scale * viewProj[row * 4] + translateX * viewProj[row * 4 + 3];
        fittedViewProj[row * 4 + 1] = scale * viewProj[row * 4 + 1] + translateY * viewProj[row * 4 + 3];
    }
    return true;
}

std::optional<std::size_t> HitTestTraceDimension (const Frame& frame, const float viewProj[16], uint32_t width,
                                                  uint32_t height, float dpiScale, bool fitSelectedFrame,
                                                  const ScreenPoint& cursor, float textHeightMetres,
                                                  float hideBelowPixels, float capAbovePixels)
{
    if (width == 0 || height == 0 || !std::isfinite (dpiScale) || dpiScale <= 0.0f || !std::isfinite (cursor.x) ||
        !std::isfinite (cursor.y) || !std::isfinite (textHeightMetres) || textHeightMetres <= 0.0f ||
        !std::isfinite (hideBelowPixels) || hideBelowPixels < 0.0f || !std::isfinite (capAbovePixels) ||
        capAbovePixels < hideBelowPixels)
        return std::nullopt;
    float fitted[16];
    const float* projection = viewProj;
    if (fitSelectedFrame && FitFrameProjection (frame, viewProj, width, height, 24.0f * dpiScale, fitted))
        projection = fitted;

    const float hitRadiusSquared = 36.0f * dpiScale * dpiScale;
    float bestDistanceSquared = std::numeric_limits<float>::infinity ();
    float bestPathLength = std::numeric_limits<float>::infinity ();
    std::optional<std::size_t> best;
    for (std::size_t primitiveIndex = 0; primitiveIndex < frame.primitives.size (); ++primitiveIndex) {
        const Primitive& primitive = frame.primitives[primitiveIndex];
        if (primitive.kind != PrimitiveKind::Dimension || !annotation::IsDrawable (primitive))
            continue;
        float fontPixels = 0.0f;
        float furnitureScale = 0.0f;
        if (!ModelAnnotationScale (primitive, projection, width, height, textHeightMetres, hideBelowPixels,
                                   capAbovePixels, fontPixels, furnitureScale))
            continue;

        std::vector<ScreenPoint> path;
        path.reserve (primitive.points.size ());
        bool projected = true;
        for (std::size_t pointIndex = 0; pointIndex < primitive.points.size (); ++pointIndex) {
            ScreenPoint point;
            if (!ProjectPoint (primitive, pointIndex, projection, width, height, point)) {
                projected = false;
                break;
            }
            path.push_back (point);
        }
        if (!projected || path.size () < 2)
            continue;

        float distanceSquared = std::numeric_limits<float>::infinity ();
        float pathLength = 0.0f;
        for (std::size_t pointIndex = 1; pointIndex < path.size (); ++pointIndex) {
            distanceSquared = std::min (distanceSquared,
                                        PointSegmentDistanceSquared (cursor, path[pointIndex - 1], path[pointIndex]));
            pathLength +=
                std::hypot (path[pointIndex].x - path[pointIndex - 1].x, path[pointIndex].y - path[pointIndex - 1].y);
        }
        if (distanceSquared > hitRadiusSquared)
            continue;
        constexpr float tieToleranceSquared = 0.25f;
        const bool nearer = distanceSquared < bestDistanceSquared - tieToleranceSquared;
        const bool sameDistance = std::fabs (distanceSquared - bestDistanceSquared) <= tieToleranceSquared;
        const bool shorter = pathLength < bestPathLength - 1.0e-3f;
        if (!best.has_value () || nearer || (sameDistance && shorter)) {
            best = primitiveIndex;
            bestDistanceSquared = distanceSquared;
            bestPathLength = pathLength;
        }
    }
    return best;
}

std::optional<std::size_t>
UpdateDimensionHover (DimensionHoverState& state, const std::shared_ptr<const annotation::DrawList>& source,
                      std::size_t nodeIndex, std::size_t frameIndex, std::optional<std::size_t> hit, bool eligible,
                      std::chrono::steady_clock::time_point now, std::chrono::milliseconds delay)
{
    const bool sourceChanged = state.source != source || state.nodeIndex != nodeIndex || state.frameIndex != frameIndex;
    if (sourceChanged) {
        state = {};
        state.source = source;
        state.nodeIndex = nodeIndex;
        state.frameIndex = frameIndex;
    }
    if (!eligible || !hit.has_value ()) {
        state.candidate.reset ();
        state.visible.reset ();
        state.startedAt = {};
        return std::nullopt;
    }
    if (state.candidate != hit) {
        state.candidate = hit;
        state.visible.reset ();
        state.startedAt = now;
        return std::nullopt;
    }
    if (now - state.startedAt >= delay)
        state.visible = hit;
    return state.visible;
}

ProjectedDrawList BuildTraceAnnotations (const Frame& frame, const float viewProj[16], uint32_t width, uint32_t height,
                                         float dpiScale, bool fitSelectedFrame, const ScreenTextMeasure& measureText,
                                         AnnotationPlacementHistory* placementHistory, float textHeightMetres,
                                         float hideBelowPixels, float capAbovePixels,
                                         const AnnotationPrimitiveFilter& primitiveFilter)
{
    ProjectedDrawList out;
    if (width == 0 || height == 0 || !std::isfinite (dpiScale) || dpiScale <= 0.0f ||
        !std::isfinite (textHeightMetres) || textHeightMetres <= 0.0f || !std::isfinite (hideBelowPixels) ||
        hideBelowPixels < 0.0f || !std::isfinite (capAbovePixels) || capAbovePixels < hideBelowPixels)
        return out;
    float fitted[16];
    const float* projection = viewProj;
    if (fitSelectedFrame && FitFrameProjection (frame, viewProj, width, height, 24.0f * dpiScale, fitted))
        projection = fitted;
    for (std::size_t primitiveIndex = 0; primitiveIndex < frame.primitives.size (); ++primitiveIndex) {
        const Primitive& primitive = frame.primitives[primitiveIndex];
        const std::size_t firstLabel = out.labels.size ();
        const auto markLabels = [&] {
            for (std::size_t labelIndex = firstLabel; labelIndex < out.labels.size (); ++labelIndex)
                out.labels[labelIndex].sourcePrimitive = primitiveIndex;
        };
        const uint32_t color = annotation::PackRgba (annotation::RoleColour (primitive.role));
        if (primitiveFilter && !primitiveFilter (primitiveIndex, primitive))
            continue;
        if (primitive.kind == PrimitiveKind::Element)
            continue;
        const bool scaledAnnotation = primitive.kind == PrimitiveKind::Dimension ||
                                      primitive.kind == PrimitiveKind::Angle ||
                                      primitive.kind == PrimitiveKind::Point ||
                                      primitive.kind == PrimitiveKind::Label || primitive.kind == PrimitiveKind::Arrow;
        float fontSizePixels = 0.0f;
        float furnitureScale = 0.0f;
        if (scaledAnnotation && !ModelAnnotationScale (primitive, projection, width, height, textHeightMetres,
                                                       hideBelowPixels, capAbovePixels, fontSizePixels, furnitureScale))
            continue;
        if (primitive.kind == PrimitiveKind::Dimension) {
            AddDimension (out, primitive, projection, width, height, color, furnitureScale, measureText,
                          fontSizePixels);
            markLabels ();
            continue;
        }
        if (primitive.kind == PrimitiveKind::Angle) {
            AddAngle (out, primitive, projection, width, height, color, furnitureScale, measureText, fontSizePixels);
            markLabels ();
            continue;
        }
        ScreenPoint first;
        float firstDepth = 1.0f;
        if (!ProjectPoint (primitive, 0, projection, width, height, first, &firstDepth))
            continue;
        if (primitive.kind == PrimitiveKind::Point) {
            AddLine (out, { first.x - 4.0f * furnitureScale, first.y }, { first.x + 4.0f * furnitureScale, first.y },
                     color, 2.0f * furnitureScale);
            AddLine (out, { first.x, first.y - 4.0f * furnitureScale }, { first.x, first.y + 4.0f * furnitureScale },
                     color, 2.0f * furnitureScale);
            AddText (out, primitive, { first.x + 6.0f * furnitureScale, first.y + 6.0f * furnitureScale }, color, {},
                     fontSizePixels);
            markLabels ();
            if (out.labels.size () > firstLabel)
                FadeLabelWhenOccluded (out.labels.back (), firstDepth);
            continue;
        }
        if (primitive.kind == PrimitiveKind::Label) {
            AddText (out, primitive, first, color, {}, fontSizePixels);
            markLabels ();
            if (out.labels.size () > firstLabel)
                FadeLabelWhenOccluded (out.labels.back (), firstDepth);
            continue;
        }
        ScreenPoint previous = first;
        const std::size_t pointCount = primitive.points.size ();
        const bool collisionObstacle = primitive.role != SemanticRole::Context;
        const float lineWidth = primitive.kind == PrimitiveKind::Arrow ? 2.0f * furnitureScale
                                                                       : (collisionObstacle ? 2.0f : 1.0f) * dpiScale;
        for (std::size_t index = 1; index < pointCount; ++index) {
            ScreenPoint next;
            if (ProjectPoint (primitive, index, projection, width, height, next)) {
                AddLine (out, previous, next, color, lineWidth, collisionObstacle);
                previous = next;
            }
        }
        if (primitive.kind == PrimitiveKind::Polyline && primitive.closed && pointCount > 2)
            AddLine (out, previous, first, color, lineWidth, collisionObstacle);
        if (primitive.kind == PrimitiveKind::Arrow) {
            AddArrowhead (out, first, previous, color, furnitureScale);
            AddText (out, primitive, { (first.x + previous.x) * 0.5f, (first.y + previous.y) * 0.5f }, color, {},
                     fontSizePixels);
            markLabels ();
            if (out.labels.size () > firstLabel)
                FadeLabelWhenOccluded (out.labels.back (), firstDepth);
        }
    }
    ResolveLabelOverlaps (out, width, height, dpiScale, measureText, placementHistory);
    return out;
}

} // namespace geomsrv::archviz
