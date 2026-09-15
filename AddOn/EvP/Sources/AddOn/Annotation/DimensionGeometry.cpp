#include "Annotation/DimensionGeometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace geomsrv::annotation {
namespace {

constexpr double kEpsilon = 1.0e-12;

Point3 Add (const Point3& left, const Point3& right)
{
    return { left.x + right.x, left.y + right.y, left.z + right.z };
}

Point3 Subtract (const Point3& left, const Point3& right)
{
    return { left.x - right.x, left.y - right.y, left.z - right.z };
}

Point3 Scale (const Point3& value, double scale)
{
    return { value.x * scale, value.y * scale, value.z * scale };
}

double Dot (const Point3& left, const Point3& right)
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

Point3 Cross (const Point3& left, const Point3& right)
{
    return { left.y * right.z - left.z * right.y, left.z * right.x - left.x * right.z,
             left.x * right.y - left.y * right.x };
}

std::optional<Point3> Normalize (const Point3& value)
{
    const double length = std::sqrt (Dot (value, value));
    if (!std::isfinite (length) || length <= kEpsilon)
        return std::nullopt;
    return Scale (value, 1.0 / length);
}

std::optional<Point3> ResolvePlaneNormal (const Point3& axis, const DimensionPlaneCandidates& candidates)
{
    const std::array<std::optional<Point3>, 4> ordered = { candidates.explicitNormal, candidates.geometryNormal,
                                                           candidates.declaredNormal,
                                                           candidates.cameraFacingNormal };
    for (const std::optional<Point3>& candidate : ordered) {
        if (!candidate.has_value () || !IsFinite (*candidate))
            continue;
        const Point3 perpendicular = Subtract (*candidate, Scale (axis, Dot (*candidate, axis)));
        if (const auto normal = Normalize (perpendicular); normal.has_value ())
            return normal;
    }
    return std::nullopt;
}

bool FiniteNonNegative (double value)
{
    return std::isfinite (value) && value >= 0.0;
}

} // namespace

bool IsValid (const DimensionStyle& style)
{
    return std::isfinite (style.textHeightModel) && style.textHeightModel > 0.0 &&
           FiniteNonNegative (style.dimensionOffset) && FiniteNonNegative (style.witnessStartGap) &&
           FiniteNonNegative (style.witnessOverhang) && std::isfinite (style.arrowSize) && style.arrowSize > 0.0 &&
           std::isfinite (style.arrowAngle) && style.arrowAngle > 0.0 && style.arrowAngle < 1.5707963267948966 &&
           FiniteNonNegative (style.textPaddingPx) && FiniteNonNegative (style.outsideTextGapPx) &&
           FiniteNonNegative (style.hideBelowPixels) && std::isfinite (style.capAbovePixels) &&
           style.capAbovePixels >= style.hideBelowPixels;
}

std::optional<ResolvedDimensionGeometry> ResolveAlignedDimensionGeometry (const AlignedDimensionInput& input,
                                                                         const DimensionStyle& style)
{
    if (!IsValid (style) || !IsFinite (input.first) || !IsFinite (input.second) ||
        !std::isfinite (input.userOffset) ||
        (input.explicitOffset.has_value () && !std::isfinite (*input.explicitOffset)))
        return std::nullopt;
    const Point3 difference = Subtract (input.second, input.first);
    const auto axis = Normalize (difference);
    if (!axis.has_value ())
        return std::nullopt;
    const auto planeNormal = ResolvePlaneNormal (*axis, input.planes);
    if (!planeNormal.has_value ())
        return std::nullopt;
    auto offsetDirection = Normalize (Cross (*planeNormal, *axis));
    if (!offsetDirection.has_value ())
        return std::nullopt;
    if (input.planes.preferredOffsetDirection.has_value () &&
        Dot (*offsetDirection, *input.planes.preferredOffsetDirection) < 0.0)
        *offsetDirection = Scale (*offsetDirection, -1.0);

    const double offset = input.explicitOffset.value_or (style.dimensionOffset + input.userOffset);
    const Point3 dimensionDelta = Scale (*offsetDirection, offset);
    const Point3 witnessStartDelta = Scale (*offsetDirection, style.witnessStartGap);
    const Point3 witnessEndDelta = Scale (*offsetDirection, offset + style.witnessOverhang);
    ResolvedDimensionGeometry result;
    result.sourceFirst = input.first;
    result.sourceSecond = input.second;
    result.axis = *axis;
    result.planeNormal = *planeNormal;
    result.offsetDirection = *offsetDirection;
    result.dimensionFirst = Add (input.first, dimensionDelta);
    result.dimensionSecond = Add (input.second, dimensionDelta);
    result.witnessFirstStart = Add (input.first, witnessStartDelta);
    result.witnessSecondStart = Add (input.second, witnessStartDelta);
    result.witnessFirstEnd = Add (input.first, witnessEndDelta);
    result.witnessSecondEnd = Add (input.second, witnessEndDelta);
    result.measurement = std::sqrt (Dot (difference, difference));
    result.offset = offset;
    return result;
}

std::optional<ResolvedAngularDimensionGeometry> ResolveAngularDimensionGeometry (const AngularDimensionInput& input)
{
    if (!IsFinite (input.vertex) || !IsFinite (input.firstRayPoint) || !IsFinite (input.secondRayPoint))
        return std::nullopt;
    auto first = Normalize (Subtract (input.firstRayPoint, input.vertex));
    auto second = Normalize (Subtract (input.secondRayPoint, input.vertex));
    if (!first.has_value () || !second.has_value ())
        return std::nullopt;
    if (input.reverse)
        std::swap (first, second);

    const Point3 cross = Cross (*first, *second);
    const double crossLength = std::sqrt (Dot (cross, cross));
    const double dot = std::clamp (Dot (*first, *second), -1.0, 1.0);
    double angle = std::atan2 (crossLength, dot);
    if (!std::isfinite (angle) || angle <= kEpsilon)
        return std::nullopt;
    if (input.reflex)
        angle = 6.28318530717958647692 - angle;

    std::optional<Point3> geometryNormal = crossLength > kEpsilon ? Normalize (cross) : std::nullopt;
    const std::array<std::optional<Point3>, 5> ordered = {
        input.planes.explicitNormal, geometryNormal, input.planes.geometryNormal,
        input.planes.declaredNormal, input.planes.cameraFacingNormal
    };
    std::optional<Point3> planeNormal;
    for (const auto& candidate : ordered) {
        if (!candidate.has_value () || !IsFinite (*candidate))
            continue;
        Point3 perpendicular = Subtract (*candidate, Scale (*first, Dot (*candidate, *first)));
        if (crossLength > kEpsilon)
            perpendicular = Scale (*geometryNormal, Dot (perpendicular, *geometryNormal));
        planeNormal = Normalize (perpendicular);
        if (planeNormal.has_value ())
            break;
    }
    if (!planeNormal.has_value ())
        return std::nullopt;

    return ResolvedAngularDimensionGeometry { input.vertex, *first, *second, *planeNormal, angle };
}

std::optional<ResolvedDimensionFit> ResolveDimensionFit (const DimensionFitInput& input, const DimensionStyle& style)
{
    if (!IsValid (style) || !FiniteNonNegative (input.availableSpanPx) || !FiniteNonNegative (input.textWidthPx) ||
        !FiniteNonNegative (input.beforeOverflowPx) || !FiniteNonNegative (input.afterOverflowPx) ||
        !std::isfinite (input.furnitureScale) || input.furnitureScale <= 0.0)
        return std::nullopt;
    const double arrowClearance = (style.arrowSize + style.outsideTextGapPx) * input.furnitureScale;
    const double padding = style.textPaddingPx;
    const double required = input.textWidthPx + 2.0 * (arrowClearance + padding);
    ResolvedDimensionFit fit;
    if (!style.autoTextPlacement || input.availableSpanPx >= required || !style.allowOutsideText) {
        fit.gapStartPx = std::max (0.0, (input.availableSpanPx - input.textWidthPx) * 0.5 - padding);
        fit.gapEndPx = std::min (input.availableSpanPx, (input.availableSpanPx + input.textWidthPx) * 0.5 + padding);
        return fit;
    }

    const bool before = input.beforeOverflowPx <= input.afterOverflowPx;
    fit.textMode = before ? DimensionTextMode::OutsideBefore : DimensionTextMode::OutsideAfter;
    fit.arrowMode = DimensionArrowMode::Outward;
    fit.textDistancePx = arrowClearance + input.textWidthPx * 0.5 + padding;
    const double extension = fit.textDistancePx + input.textWidthPx * 0.5 + padding;
    fit.extensionBeforePx = before ? extension : 0.0;
    fit.extensionAfterPx = before ? 0.0 : extension;
    const double halfGap = input.textWidthPx * 0.5 + padding;
    const double textCenter = before ? -fit.textDistancePx : input.availableSpanPx + fit.textDistancePx;
    fit.gapStartPx = textCenter - halfGap;
    fit.gapEndPx = textCenter + halfGap;
    return fit;
}

} // namespace geomsrv::annotation
