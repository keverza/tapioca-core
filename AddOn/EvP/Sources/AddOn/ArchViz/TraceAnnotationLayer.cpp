#include "ArchViz/TraceAnnotationLayer.hpp"

#include "ArchViz/AnnotationScreenLayout.hpp"
#include "ArchViz/MatrixMath.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace geomsrv::archviz {
namespace {

using annotation::Frame;
using annotation::Primitive;
using annotation::PrimitiveKind;
using annotation::SemanticRole;

constexpr float kArrowLength = 10.0f;
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
                    float dpiScale, float arrowLength = kArrowLength,
                    float arrowAngle = 0.4636476090008061f)
{
    const float dx = tip.x - tail.x;
    const float dy = tip.y - tail.y;
    const float length = std::sqrt (dx * dx + dy * dy);
    if (length <= 1.0e-3f)
        return;
    const float ux = dx / length;
    const float uy = dy / length;
    const float scaledLength = arrowLength * dpiScale;
    const float scaledHalfWidth = std::tan (arrowAngle) * scaledLength;
    const ScreenPoint base { tip.x - ux * scaledLength, tip.y - uy * scaledLength };
    out.triangles.push_back ({ { tip,
                                 { base.x - uy * scaledHalfWidth, base.y + ux * scaledHalfWidth },
                                 { base.x + uy * scaledHalfWidth, base.y - ux * scaledHalfWidth } },
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

std::optional<annotation::Point3> CameraFacingNormal (const float viewProj[16])
{
    // In a row-vector perspective matrix the fourth column is the world-space
    // view direction. Orthographic projection has no perspective column, so its
    // depth column supplies the same axis up to sign and scale.
    annotation::Point3 normal { viewProj[3], viewProj[7], viewProj[11] };
    double length = std::hypot (normal.x, std::hypot (normal.y, normal.z));
    if (length <= 1.0e-12) {
        normal = { viewProj[2], viewProj[6], viewProj[10] };
        length = std::hypot (normal.x, std::hypot (normal.y, normal.z));
    }
    if (!std::isfinite (length) || length <= 1.0e-12)
        return std::nullopt;
    return annotation::Point3 { normal.x / length, normal.y / length, normal.z / length };
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

void AddResolvedDimensionLine (ProjectedDrawList& out, const ScreenPoint& origin, const ScreenPoint& unit,
                               float dimensionLength, const annotation::ResolvedDimensionFit& fit, uint32_t color,
                               float lineWidth)
{
    const float lineStart = -float (fit.extensionBeforePx);
    const float lineEnd = dimensionLength + float (fit.extensionAfterPx);
    const float gapStart = float (fit.gapStartPx);
    const float gapEnd = float (fit.gapEndPx);
    const auto pointAt = [&] (float distance) {
        return ScreenPoint { origin.x + unit.x * distance, origin.y + unit.y * distance };
    };
    if (gapStart > lineStart + 1.0e-3f)
        AddLine (out, pointAt (lineStart), pointAt (std::min (gapStart, lineEnd)), color, lineWidth);
    if (gapEnd < lineEnd - 1.0e-3f)
        AddLine (out, pointAt (std::max (gapEnd, lineStart)), pointAt (lineEnd), color, lineWidth);
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
                     float fontSizePixels, const annotation::DimensionStyle& style, std::size_t primitiveIndex,
                     AnnotationPlacementHistory* placementHistory, bool retainDimensionCandidates)
{
    if (primitive.points.size () > 2) {
        AddArcDimension (out, primitive, viewProj, width, height, color, furnitureScale, measureText, fontSizePixels);
        return;
    }
    const annotation::Point3& worldA = primitive.points[0];
    const annotation::Point3& worldB = primitive.points[1];
    const annotation::Point3 difference { worldB.x - worldA.x, worldB.y - worldA.y, worldB.z - worldA.z };
    const double worldLength = std::sqrt (difference.x * difference.x + difference.y * difference.y +
                                          difference.z * difference.z);
    if (worldLength <= 1.0e-12)
        return;
    char measured[64];
    std::snprintf (measured, sizeof (measured), "%.3f m", worldLength);
    const std::string text = primitive.text.empty () ? measured : primitive.text;
    const float fontSize = fontSizePixels;
    const ScreenTextExtent textExtent = MeasureText (text, fontSize, measureText);
    annotation::AlignedDimensionInput input;
    input.first = worldA;
    input.second = worldB;
    input.planes.explicitNormal = primitive.planeNormal;
    input.planes.preferredOffsetDirection = primitive.preferredOffsetDirection;
    const annotation::Point3 axis { difference.x / worldLength, difference.y / worldLength,
                                    difference.z / worldLength };
    if (!input.planes.explicitNormal.has_value () && input.planes.preferredOffsetDirection.has_value ()) {
        const annotation::Point3& preferred = *input.planes.preferredOffsetDirection;
        input.planes.geometryNormal = { axis.y * preferred.z - axis.z * preferred.y,
                                       axis.z * preferred.x - axis.x * preferred.z,
                                       axis.x * preferred.y - axis.y * preferred.x };
    }
    if (std::fabs (axis.z) < 1.0 - 1.0e-6)
        input.planes.declaredNormal = annotation::Point3 { 0.0, 0.0, 1.0 };
    input.planes.cameraFacingNormal = CameraFacingNormal (viewProj);
    if (primitive.offset != 0.0) {
        input.explicitOffset = std::fabs (primitive.offset);
        if (primitive.offset < 0.0) {
            if (input.planes.preferredOffsetDirection.has_value ()) {
                annotation::Point3& preferred = *input.planes.preferredOffsetDirection;
                preferred = { -preferred.x, -preferred.y, -preferred.z };
            }
            else {
                input.planes.preferredOffsetDirection = annotation::Point3 { 0.0, 0.0, -1.0 };
            }
        }
    }

    struct Candidate {
        annotation::ResolvedDimensionGeometry geometry;
        ScreenPoint first;
        ScreenPoint second;
        ScreenPoint labelCenter;
        annotation::ResolvedDimensionFit fit;
        float score = 0.0f;
        uint8_t id = 0;
    };
    std::vector<Candidate> candidates;
    const std::optional<annotation::Point3> preferred = input.planes.preferredOffsetDirection;
    for (uint8_t candidateId = 0; candidateId < 4; ++candidateId) {
        annotation::AlignedDimensionInput candidateInput = input;
        const double sign = candidateId % 2 == 0 ? 1.0 : -1.0;
        if (preferred.has_value ())
            candidateInput.planes.preferredOffsetDirection = annotation::Point3 {
                preferred->x * sign, preferred->y * sign, preferred->z * sign };
        else
            candidateInput.planes.preferredOffsetDirection = annotation::Point3 { 0.0, sign, 0.0 };
        const double baseOffset = input.explicitOffset.value_or (style.dimensionOffset + input.userOffset);
        candidateInput.explicitOffset = baseOffset * (candidateId >= 2 ? 1.75 : 1.0);
        const auto resolved = annotation::ResolveAlignedDimensionGeometry (candidateInput, style);
        if (!resolved.has_value ())
            continue;
        const auto projectPoint = [&] (const annotation::Point3& point, ScreenPoint& screen, float* depth = nullptr) {
            const double value[3] = { point.x, point.y, point.z };
            return Project (value, viewProj, width, height, screen, depth);
        };
        Candidate candidate;
        candidate.geometry = *resolved;
        candidate.id = candidateId;
        if (!projectPoint (resolved->dimensionFirst, candidate.first) ||
            !projectPoint (resolved->dimensionSecond, candidate.second))
            continue;
        const ScreenPoint center { (candidate.first.x + candidate.second.x) * 0.5f,
                                   (candidate.first.y + candidate.second.y) * 0.5f };
        const float dimensionDx = candidate.second.x - candidate.first.x;
        const float dimensionDy = candidate.second.y - candidate.first.y;
        const float dimensionLength = std::hypot (dimensionDx, dimensionDy);
        if (dimensionLength <= 1.0e-3f)
            continue;
        const ScreenPoint unit { dimensionDx / dimensionLength, dimensionDy / dimensionLength };
        const float panelPadding = float (style.textPaddingPx);
        const float labelDistance = float ((style.arrowSize + style.outsideTextGapPx) * furnitureScale) +
                                    textExtent.width * 0.5f + panelPadding;
        const ScreenPoint before { candidate.first.x - unit.x * labelDistance,
                                   candidate.first.y - unit.y * labelDistance };
        const ScreenPoint after { candidate.second.x + unit.x * labelDistance,
                                  candidate.second.y + unit.y * labelDistance };
        const auto fit = annotation::ResolveDimensionFit (
            { dimensionLength, textExtent.width, LabelOverflow (before, textExtent, width, height, panelPadding),
              LabelOverflow (after, textExtent, width, height, panelPadding), furnitureScale },
            style);
        if (!fit.has_value ())
            continue;
        candidate.fit = *fit;
        candidate.labelCenter = fit->textMode == annotation::DimensionTextMode::Centered
                                    ? center
                                    : (fit->textMode == annotation::DimensionTextMode::OutsideBefore ? before : after);
        candidate.id = uint8_t (candidateId | (uint8_t (fit->textMode) << 2) |
                                (uint8_t (fit->arrowMode) << 4));
        candidate.score = EdgeClearance (candidate.first, candidate.second, candidate.labelCenter, textExtent, width,
                                         height) -
                          100.0f * LabelOverflow (candidate.labelCenter, textExtent, width, height, panelPadding) -
                          AnnotationCandidateOccupancyPenalty (
                              out, candidate.labelCenter, textExtent,
                              ReadableRotation (std::atan2 (dimensionDy, dimensionDx)), 5.0f * furnitureScale,
                              measureText);
        candidates.push_back (candidate);
    }
    if (candidates.empty ())
        return;
    const auto best = std::max_element (candidates.begin (), candidates.end (), [] (const Candidate& left,
                                                                                     const Candidate& right) {
        return left.score < right.score;
    });
    auto selected = best;
    const std::string annotationId = primitive.annotationId.empty () ? std::to_string (primitiveIndex)
                                                                      : primitive.annotationId;
    if (placementHistory != nullptr) {
        const auto previous = placementHistory->dimensionCandidateByAnnotation.find (annotationId);
        if (previous != placementHistory->dimensionCandidateByAnnotation.end ()) {
            const auto retained = std::find_if (candidates.begin (), candidates.end (), [&] (const Candidate& item) {
                return item.id == previous->second;
            });
            if (retained != candidates.end () &&
                (retainDimensionCandidates || retained->score >= best->score - 8.0f * furnitureScale))
                selected = retained;
        }
        placementHistory->dimensionCandidateByAnnotation[annotationId] = selected->id;
    }
    const annotation::ResolvedDimensionGeometry& geometry = selected->geometry;
    const ScreenPoint da = selected->first;
    const ScreenPoint db = selected->second;
    const auto projectPoint = [&] (const annotation::Point3& point, ScreenPoint& screen, float* depth = nullptr) {
        const double value[3] = { point.x, point.y, point.z };
        return Project (value, viewProj, width, height, screen, depth);
    };
    ScreenPoint witnessA0, witnessA1, witnessB0, witnessB1;
    if (!projectPoint (geometry.witnessFirstStart, witnessA0) || !projectPoint (geometry.witnessFirstEnd, witnessA1) ||
        !projectPoint (geometry.witnessSecondStart, witnessB0) || !projectPoint (geometry.witnessSecondEnd, witnessB1))
        return;
    const std::size_t ownLineBegin = out.lines.size ();
    AddLine (out, witnessA0, witnessA1, color, 2.0f * furnitureScale);
    AddLine (out, witnessB0, witnessB1, color, 2.0f * furnitureScale);
    const float dimensionDx = db.x - da.x;
    const float dimensionDy = db.y - da.y;
    const float dimensionLength = std::hypot (dimensionDx, dimensionDy);
    if (dimensionLength <= 1.0e-3f)
        return;
    const ScreenPoint unit { dimensionDx / dimensionLength, dimensionDy / dimensionLength };
    const ScreenPoint labelCenter = selected->labelCenter;
    const annotation::ResolvedDimensionFit& fit = selected->fit;
    if (fit.arrowMode == annotation::DimensionArrowMode::Inward) {
        AddArrowhead (out, { da.x - unit.x, da.y - unit.y }, da, color, furnitureScale,
                      float (style.arrowSize), float (style.arrowAngle));
        AddArrowhead (out, { db.x + unit.x, db.y + unit.y }, db, color, furnitureScale,
                      float (style.arrowSize), float (style.arrowAngle));
    }
    else {
        AddArrowhead (out, { da.x + unit.x, da.y + unit.y }, da, color, furnitureScale,
                      float (style.arrowSize), float (style.arrowAngle));
        AddArrowhead (out, { db.x - unit.x, db.y - unit.y }, db, color, furnitureScale,
                      float (style.arrowSize), float (style.arrowAngle));
    }
    AddResolvedDimensionLine (out, da, unit, dimensionLength, fit, color, 2.0f * furnitureScale);
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
        out.labels.back ().resolvedPlacement = true;
        out.labels.back ().ownLineBegin = ownLineBegin;
        out.labels.back ().ownLineEnd = out.lines.size ();
        ScreenPoint depthAnchor;
        float depth = 1.0f;
        const annotation::Point3 depthWorld { (geometry.dimensionFirst.x + geometry.dimensionSecond.x) * 0.5,
                                              (geometry.dimensionFirst.y + geometry.dimensionSecond.y) * 0.5,
                                              (geometry.dimensionFirst.z + geometry.dimensionSecond.z) * 0.5 };
        if (projectPoint (depthWorld, depthAnchor, &depth) &&
            style.depthPolicy == annotation::DepthPolicy::FadeWhenOccluded)
            FadeLabelWhenOccluded (out.labels.back (), depth);
    }
}

void AddAngle (ProjectedDrawList& out, const Primitive& primitive, const float viewProj[16], uint32_t width,
               uint32_t height, uint32_t color, float furnitureScale, const ScreenTextMeasure& measureText,
               float fontSizePixels)
{
    const std::size_t ownLineBegin = out.lines.size ();
    annotation::AngularDimensionInput input;
    input.vertex = primitive.points[0];
    input.firstRayPoint = primitive.points[1];
    input.secondRayPoint = primitive.points[2];
    input.planes.explicitNormal = primitive.planeNormal;
    input.planes.cameraFacingNormal = CameraFacingNormal (viewProj);
    input.reverse = primitive.direction;
    const auto resolved = annotation::ResolveAngularDimensionGeometry (input);
    if (!resolved.has_value ())
        return;
    ScreenPoint center, first, second;
    float centerDepth = 1.0f;
    if (!ProjectPoint (primitive, 0, viewProj, width, height, center, &centerDepth) ||
        !ProjectPoint (primitive, 1, viewProj, width, height, first) ||
        !ProjectPoint (primitive, 2, viewProj, width, height, second))
        return;
    if (input.reverse)
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
    std::snprintf (measured, sizeof (measured), "%.1f\xC2\xB0",
                   resolved->angleRadians * 180.0 / 3.14159265358979323846);
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
                                                  const ScreenPoint& cursor,
                                                  const annotation::DimensionStyle& style)
{
    if (width == 0 || height == 0 || !std::isfinite (dpiScale) || dpiScale <= 0.0f || !std::isfinite (cursor.x) ||
        !std::isfinite (cursor.y) || !annotation::IsValid (style))
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
        if (!ModelAnnotationScale (primitive, projection, width, height, float (style.textHeightModel),
                                   float (style.hideBelowPixels), float (style.capAbovePixels), fontPixels,
                                   furnitureScale))
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
                                         AnnotationPlacementHistory* placementHistory,
                                         const annotation::DimensionStyle& style,
                                         const AnnotationPrimitiveFilter& primitiveFilter,
                                         bool retainDimensionCandidates)
{
    ProjectedDrawList out;
    if (width == 0 || height == 0 || !std::isfinite (dpiScale) || dpiScale <= 0.0f ||
        !annotation::IsValid (style))
        return out;
    float fitted[16];
    const float* projection = viewProj;
    if (fitSelectedFrame && FitFrameProjection (frame, viewProj, width, height, 24.0f * dpiScale, fitted))
        projection = fitted;
    if (placementHistory != nullptr) {
        retainDimensionCandidates |= placementHistory->hasViewProjection &&
                                     !std::equal (projection, projection + 16,
                                                  placementHistory->lastViewProjection);
        std::copy (projection, projection + 16, placementHistory->lastViewProjection);
        placementHistory->hasViewProjection = true;
    }
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
        if (scaledAnnotation &&
            !ModelAnnotationScale (primitive, projection, width, height, float (style.textHeightModel),
                                   float (style.hideBelowPixels), float (style.capAbovePixels), fontSizePixels,
                                   furnitureScale))
            continue;
        if (primitive.kind == PrimitiveKind::Dimension) {
            AddDimension (out, primitive, projection, width, height, color, furnitureScale, measureText, fontSizePixels,
                          style, primitiveIndex, placementHistory, retainDimensionCandidates);
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
    ResolveAnnotationLabelOverlaps (out, width, height, dpiScale, measureText, placementHistory);
    return out;
}

} // namespace geomsrv::archviz
