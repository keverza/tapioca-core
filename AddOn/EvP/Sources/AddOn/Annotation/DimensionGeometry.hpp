#ifndef EVP_ANNOTATION_DIMENSIONGEOMETRY_HPP
#define EVP_ANNOTATION_DIMENSIONGEOMETRY_HPP

#include "Annotation/DrawList.hpp"

#include <optional>

namespace geomsrv::annotation {

enum class DepthPolicy { FadeWhenOccluded, AlwaysVisible };

struct DimensionStyle {
    double textHeightModel = 0.18;
    double dimensionOffset = 0.25;
    double witnessStartGap = 0.02;
    double witnessOverhang = 0.04;
    double arrowSize = 10.0;
    double arrowAngle = 0.4636476090008061;
    double textPaddingPx = 4.0;
    double outsideTextGapPx = 2.0;
    double hideBelowPixels = 10.0;
    double capAbovePixels = 36.0;
    bool autoTextPlacement = true;
    bool allowOutsideText = true;
    bool geometryAlignedText = true;
    DepthPolicy depthPolicy = DepthPolicy::FadeWhenOccluded;
};

bool IsValid (const DimensionStyle& style);

struct DimensionPlaneCandidates {
    std::optional<Point3> explicitNormal;
    std::optional<Point3> geometryNormal;
    std::optional<Point3> declaredNormal;
    std::optional<Point3> cameraFacingNormal;
    std::optional<Point3> preferredOffsetDirection;
};

struct AlignedDimensionInput {
    Point3 first;
    Point3 second;
    DimensionPlaneCandidates planes;
    double userOffset = 0.0;
    std::optional<double> explicitOffset;
};

struct ResolvedDimensionGeometry {
    Point3 sourceFirst;
    Point3 sourceSecond;
    Point3 axis;
    Point3 planeNormal;
    Point3 offsetDirection;
    Point3 dimensionFirst;
    Point3 dimensionSecond;
    Point3 witnessFirstStart;
    Point3 witnessFirstEnd;
    Point3 witnessSecondStart;
    Point3 witnessSecondEnd;
    double measurement = 0.0;
    double offset = 0.0;
};

std::optional<ResolvedDimensionGeometry> ResolveAlignedDimensionGeometry (const AlignedDimensionInput& input,
                                                                          const DimensionStyle& style);

struct AngularDimensionInput {
    Point3 vertex;
    Point3 firstRayPoint;
    Point3 secondRayPoint;
    DimensionPlaneCandidates planes;
    bool reflex = false;
    bool reverse = false;
};

struct ResolvedAngularDimensionGeometry {
    Point3 vertex;
    Point3 firstDirection;
    Point3 secondDirection;
    Point3 planeNormal;
    double angleRadians = 0.0;
};

std::optional<ResolvedAngularDimensionGeometry> ResolveAngularDimensionGeometry (
    const AngularDimensionInput& input);

enum class DimensionTextMode { Centered, OutsideBefore, OutsideAfter };
enum class DimensionArrowMode { Inward, Outward };

struct DimensionFitInput {
    double availableSpanPx = 0.0;
    double textWidthPx = 0.0;
    double beforeOverflowPx = 0.0;
    double afterOverflowPx = 0.0;
    double furnitureScale = 1.0;
};

struct ResolvedDimensionFit {
    DimensionTextMode textMode = DimensionTextMode::Centered;
    DimensionArrowMode arrowMode = DimensionArrowMode::Inward;
    double textDistancePx = 0.0;
    double extensionBeforePx = 0.0;
    double extensionAfterPx = 0.0;
    double gapStartPx = 0.0;
    double gapEndPx = 0.0;
};

std::optional<ResolvedDimensionFit> ResolveDimensionFit (const DimensionFitInput& input,
                                                         const DimensionStyle& style);

} // namespace geomsrv::annotation

#endif
