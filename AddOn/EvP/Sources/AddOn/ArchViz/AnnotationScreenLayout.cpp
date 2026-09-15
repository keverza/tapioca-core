#include "ArchViz/AnnotationScreenLayout.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <numeric>

namespace geomsrv::archviz {
namespace {

constexpr float kBaseFontPixels = 18.0f;

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

} // namespace

float AnnotationCandidateOccupancyPenalty (const ProjectedDrawList& drawList, const ScreenPoint& center,
                                           const ScreenTextExtent& extent, float rotationRadians, float gap,
                                           const ScreenTextMeasure& measureText)
{
    const LabelBox candidate { center, extent.width * 0.5f, extent.height * 0.5f, rotationRadians };
    float penalty = 0.0f;
    for (const ScreenLabel& label : drawList.labels) {
        const float fontSize = label.fontSize > 0.0f ? label.fontSize : kBaseFontPixels;
        const ScreenTextExtent occupiedExtent = MeasureText (label.text, fontSize, measureText);
        const ScreenPoint occupiedCenter {
            label.centered ? label.anchor.x : label.anchor.x + occupiedExtent.width * 0.5f,
            label.centered ? label.anchor.y + occupiedExtent.height * 0.5f
                           : label.anchor.y - occupiedExtent.height * 0.5f
        };
        if (Overlaps (candidate,
                      { occupiedCenter, occupiedExtent.width * 0.5f, occupiedExtent.height * 0.5f,
                        label.rotationRadians },
                      gap))
            penalty += 1000.0f;
    }
    for (const ScreenLine& line : drawList.lines) {
        if (line.collisionObstacle && Intersects (line, candidate, gap))
            penalty += 250.0f;
    }
    return penalty;
}

void ResolveAnnotationLabelOverlaps (ProjectedDrawList& drawList, uint32_t width, uint32_t height, float dpiScale,
                                     const ScreenTextMeasure& measureText,
                                     AnnotationPlacementHistory* placementHistory)
{
    std::vector<LabelBox> occupied;
    occupied.reserve (drawList.labels.size ());
    const float inset = 4.0f * dpiScale;
    for (ScreenLabel& label : drawList.labels) {
        const float fontSize = label.fontSize > 0.0f ? label.fontSize : kBaseFontPixels * dpiScale;
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
        if (label.resolvedPlacement) {
            occupied.push_back ({ originalCenter, extent.width * 0.5f, extent.height * 0.5f,
                                  label.rotationRadians });
            continue;
        }
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
                    for (std::size_t lineIndex = 0; lineIndex < drawList.lines.size (); ++lineIndex) {
                        if (lineIndex >= label.ownLineBegin && lineIndex < label.ownLineEnd)
                            continue;
                        if (drawList.lines[lineIndex].collisionObstacle &&
                            Intersects (drawList.lines[lineIndex], candidate, 2.0f * labelScale)) {
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
        if (!placed)
            occupied.push_back ({ originalCenter, extent.width * 0.5f, extent.height * 0.5f, label.rotationRadians });
    }
}

} // namespace geomsrv::archviz
