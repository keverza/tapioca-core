#include "ArchViz/TraceAnnotationLayer.hpp"

#include "ArchViz/MatrixMath.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace geomsrv::archviz {
namespace {

using annotation::Frame;
using annotation::Primitive;
using annotation::PrimitiveKind;
using annotation::SemanticRole;

constexpr float kArrowLength = 10.0f;
constexpr float kArrowWidth = 5.0f;
constexpr float kDimensionOffset = 14.0f;
constexpr float kDimensionTextSize = 18.0f;

bool PointAt (const Primitive& primitive, std::size_t index, double out[3])
{
    if (index >= primitive.points.size ())
        return false;
    out[0] = primitive.points[index].x;
    out[1] = primitive.points[index].y;
    out[2] = primitive.points[index].z;
    return std::isfinite (out[0]) && std::isfinite (out[1]) && std::isfinite (out[2]);
}

bool Project (const double point[3], const float viewProj[16], uint32_t width, uint32_t height, ScreenPoint& screen)
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
    return std::isfinite (screen.x) && std::isfinite (screen.y);
}

void AddLine (ProjectedDrawList& out, const ScreenPoint& from, const ScreenPoint& to, uint32_t color,
              float width = 2.0f)
{
    out.lines.push_back ({ from, to, color, width });
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
                   uint32_t height, ScreenPoint& screen)
{
    double point[3];
    return PointAt (primitive, index, point) && Project (point, viewProj, width, height, screen);
}

void AddText (ProjectedDrawList& out, const Primitive& primitive, const ScreenPoint& anchor, uint32_t color,
              const std::string& fallback = {}, float fontSize = 0.0f, bool centered = false,
              bool backgroundPanel = true, uint32_t haloRgba = 0, float haloWidthPixels = 0.0f)
{
    const std::string text = primitive.text.empty () ? fallback : primitive.text;
    if (!text.empty ())
        out.labels.push_back (
            { anchor, text, color, fontSize, centered, haloRgba, haloWidthPixels, backgroundPanel });
}

std::size_t Utf8CodepointCount (std::string_view text)
{
    return std::count_if (text.begin (), text.end (), [] (char value) {
        return (static_cast<unsigned char> (value) & 0xC0u) != 0x80u;
    });
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
    return std::min ({ first.x, second.x, labelCenter.x - label.width * 0.5f,
                       float (width) - first.x, float (width) - second.x,
                       float (width) - labelCenter.x - label.width * 0.5f, first.y, second.y,
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
    const float halfGap = std::fabs (ux) * (label.width * 0.5f + padding) +
                          std::fabs (uy) * (label.height * 0.5f + padding);
    const float before = std::clamp (centerDistance - halfGap, 0.0f, length);
    const float after = std::clamp (centerDistance + halfGap, 0.0f, length);
    if (before > 1.0e-3f)
        AddLine (out, from, { from.x + ux * before, from.y + uy * before }, color, lineWidth);
    if (after < length - 1.0e-3f)
        AddLine (out, { from.x + ux * after, from.y + uy * after }, to, color, lineWidth);
}

struct LabelBounds {
    float left;
    float top;
    float right;
    float bottom;
};

bool Overlaps (const LabelBounds& left, const LabelBounds& right, float gap)
{
    return left.left < right.right + gap && left.right + gap > right.left && left.top < right.bottom + gap &&
           left.bottom + gap > right.top;
}

void ResolveLabelOverlaps (ProjectedDrawList& out, uint32_t width, uint32_t height, float dpiScale,
                           const ScreenTextMeasure& measureText)
{
    std::vector<LabelBounds> occupied;
    occupied.reserve (out.labels.size ());
    const float inset = 4.0f * dpiScale;
    const float gap = 5.0f * dpiScale;
    for (ScreenLabel& label : out.labels) {
        const float fontSize = label.fontSize > 0.0f ? label.fontSize : 18.0f * dpiScale;
        const ScreenTextExtent extent = MeasureText (label.text, fontSize, measureText);
        const float cosine = std::fabs (std::cos (label.rotationRadians));
        const float sine = std::fabs (std::sin (label.rotationRadians));
        const ScreenTextExtent collisionExtent { cosine * extent.width + sine * extent.height,
                                                 sine * extent.width + cosine * extent.height };
        const ScreenPoint original = label.anchor;
        const float stepX = collisionExtent.width + 7.0f * dpiScale;
        const float stepY = collisionExtent.height + 7.0f * dpiScale;
        const ScreenPoint offsets[] = {
            { 0.0f, 0.0f },       { 0.0f, -stepY },     { 0.0f, stepY },
            { -stepX, 0.0f },     { stepX, 0.0f },      { -stepX, -stepY },
            { stepX, -stepY },    { -stepX, stepY },    { stepX, stepY },
            { 0.0f, -2.0f * stepY }, { 0.0f, 2.0f * stepY },
        };
        bool placed = false;
        for (const ScreenPoint& offset : offsets) {
            const ScreenPoint anchor { original.x + offset.x, original.y + offset.y };
            const ScreenPoint center { label.centered ? anchor.x : anchor.x + extent.width * 0.5f,
                                       label.centered ? anchor.y + extent.height * 0.5f
                                                      : anchor.y - extent.height * 0.5f };
            const float left = center.x - collisionExtent.width * 0.5f;
            const float top = center.y - collisionExtent.height * 0.5f;
            const LabelBounds candidate { left, top, left + collisionExtent.width,
                                          top + collisionExtent.height };
            if (candidate.left < inset || candidate.top < inset || candidate.right > float (width) - inset ||
                candidate.bottom > float (height) - inset)
                continue;
            if (std::any_of (occupied.begin (), occupied.end (), [&] (const LabelBounds& item) {
                    return Overlaps (candidate, item, gap);
                }))
                continue;
            label.anchor = anchor;
            occupied.push_back (candidate);
            placed = true;
            if (offset.x != 0.0f || offset.y != 0.0f) {
                const ScreenPoint movedCenter { candidate.left + collisionExtent.width * 0.5f,
                                                candidate.top + collisionExtent.height * 0.5f };
                AddLine (out, original, movedCenter, label.rgba, std::max (1.0f, dpiScale));
            }
            break;
        }
        if (!placed) {
            const ScreenPoint center { label.centered ? original.x : original.x + extent.width * 0.5f,
                                       label.centered ? original.y + extent.height * 0.5f
                                                      : original.y - extent.height * 0.5f };
            occupied.push_back ({ center.x - collisionExtent.width * 0.5f,
                                  center.y - collisionExtent.height * 0.5f,
                                  center.x + collisionExtent.width * 0.5f,
                                  center.y + collisionExtent.height * 0.5f });
        }
    }
}

void AddDimension (ProjectedDrawList& out, const Primitive& primitive, const float viewProj[16], uint32_t width,
                    uint32_t height, uint32_t color, float dpiScale, const ScreenTextMeasure& measureText)
{
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
    const float fontSize = kDimensionTextSize * dpiScale;
    const ScreenTextExtent textExtent = MeasureText (text, fontSize, measureText);
    ScreenPoint da;
    ScreenPoint db;
    if (primitive.offset != 0.0) {
        const double planX = worldB[0] - worldA[0], planY = worldB[1] - worldA[1];
        const double planLength = std::sqrt (planX * planX + planY * planY);
        if (planLength <= 1.0e-12)
            return;
        const double ox = -planY / planLength * primitive.offset;
        const double oy = planX / planLength * primitive.offset;
        const double dimensionA[3] = { worldA[0] + ox, worldA[1] + oy, worldA[2] };
        const double dimensionB[3] = { worldB[0] + ox, worldB[1] + oy, worldB[2] };
        if (!Project (dimensionA, viewProj, width, height, da) || !Project (dimensionB, viewProj, width, height, db))
            return;
    }
    else {
        const ScreenPoint normal { -projectedUnit.y * kDimensionOffset * dpiScale,
                                   projectedUnit.x * kDimensionOffset * dpiScale };
        const ScreenPoint positiveA { a.x + normal.x, a.y + normal.y };
        const ScreenPoint positiveB { b.x + normal.x, b.y + normal.y };
        const ScreenPoint negativeA { a.x - normal.x, a.y - normal.y };
        const ScreenPoint negativeB { b.x - normal.x, b.y - normal.y };
        const ScreenPoint positiveCenter { (positiveA.x + positiveB.x) * 0.5f,
                                           (positiveA.y + positiveB.y) * 0.5f };
        const ScreenPoint negativeCenter { (negativeA.x + negativeB.x) * 0.5f,
                                           (negativeA.y + negativeB.y) * 0.5f };
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
    AddLine (out, a, da, color, 2.0f * dpiScale);
    AddLine (out, b, db, color, 2.0f * dpiScale);
    const float dimensionDx = db.x - da.x;
    const float dimensionDy = db.y - da.y;
    const float dimensionLength = std::hypot (dimensionDx, dimensionDy);
    if (dimensionLength <= 1.0e-3f)
        return;
    const ScreenPoint unit { dimensionDx / dimensionLength, dimensionDy / dimensionLength };
    const float panelPadding = 4.0f * dpiScale;
    const float textAlongLine = std::fabs (unit.x) * textExtent.width + std::fabs (unit.y) * textExtent.height;
    const bool fitsInside = dimensionLength >= textAlongLine + 2.0f * (kArrowLength + 3.0f) * dpiScale;
    ScreenPoint labelCenter;
    ScreenPoint lineFrom = da;
    ScreenPoint lineTo = db;
    if (fitsInside) {
        labelCenter = { (da.x + db.x) * 0.5f, (da.y + db.y) * 0.5f };
        AddArrowhead (out, db, da, color, dpiScale);
        AddArrowhead (out, da, db, color, dpiScale);
    }
    else {
        const float labelDistance = (kArrowLength + 2.0f) * dpiScale + textAlongLine * 0.5f + panelPadding;
        const ScreenPoint before { da.x - unit.x * labelDistance, da.y - unit.y * labelDistance };
        const ScreenPoint after { db.x + unit.x * labelDistance, db.y + unit.y * labelDistance };
        const bool useBefore = LabelOverflow (before, textExtent, width, height, panelPadding) <=
                               LabelOverflow (after, textExtent, width, height, panelPadding);
        labelCenter = useBefore ? before : after;
        const float overhang = (kArrowLength + 4.0f) * dpiScale + textAlongLine + panelPadding * 2.0f;
        if (useBefore)
            lineFrom = { da.x - unit.x * overhang, da.y - unit.y * overhang };
        else
            lineTo = { db.x + unit.x * overhang, db.y + unit.y * overhang };
        AddArrowhead (out, { da.x + unit.x, da.y + unit.y }, da, color, dpiScale);
        AddArrowhead (out, { db.x - unit.x, db.y - unit.y }, db, color, dpiScale);
    }
    AddLineTrimmedAgainstLabel (out, lineFrom, lineTo, labelCenter, textExtent, panelPadding, color,
                                2.0f * dpiScale);
    if (!text.empty ())
        out.labels.push_back ({ { labelCenter.x, labelCenter.y - textExtent.height * 0.5f }, text, color, fontSize,
                                true, 0xFFFFFFC0u, 1.25f * dpiScale, false });
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
               uint32_t height, uint32_t color, float dpiScale)
{
    ScreenPoint center, first, second;
    if (!ProjectPoint (primitive, 0, viewProj, width, height, center) ||
        !ProjectPoint (primitive, 1, viewProj, width, height, first) ||
        !ProjectPoint (primitive, 2, viewProj, width, height, second))
        return;
    if (primitive.direction)
        std::swap (first, second);
    annotation::ArchitecturalAngleGlyph glyph;
    if (!annotation::BuildArchitecturalAngleGlyph ({ center.x, center.y }, { first.x, first.y }, { second.x, second.y },
                                                   dpiScale, glyph))
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
    AddText (out, primitive, { float (glyph.labelAnchor.x), float (glyph.labelAnchor.y) }, color, measured,
             float (glyph.fontSizePixels), true, false, 0xFFFFFFC0u, 1.25f * dpiScale);
    ScreenLabel& label = out.labels.back ();
    label.rotationRadians = std::atan2 (label.anchor.y - center.y, label.anchor.x - center.x);
    constexpr float halfPi = 1.57079632679489661923f;
    constexpr float pi = 3.14159265358979323846f;
    if (label.rotationRadians > halfPi)
        label.rotationRadians -= pi;
    else if (label.rotationRadians < -halfPi)
        label.rotationRadians += pi;
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

ProjectedDrawList BuildTraceAnnotations (const Frame& frame, const float viewProj[16], uint32_t width, uint32_t height,
                                         float dpiScale, bool fitSelectedFrame, const ScreenTextMeasure& measureText)
{
    ProjectedDrawList out;
    if (width == 0 || height == 0 || !std::isfinite (dpiScale) || dpiScale <= 0.0f)
        return out;
    float fitted[16];
    const float* projection = viewProj;
    if (fitSelectedFrame && FitFrameProjection (frame, viewProj, width, height, 24.0f * dpiScale, fitted))
        projection = fitted;
    for (const Primitive& primitive : frame.primitives) {
        const uint32_t color = annotation::PackRgba (annotation::RoleColour (primitive.role));
        if (primitive.kind == PrimitiveKind::Element)
            continue;
        if (primitive.kind == PrimitiveKind::Dimension) {
            AddDimension (out, primitive, projection, width, height, color, dpiScale, measureText);
            continue;
        }
        if (primitive.kind == PrimitiveKind::Angle) {
            AddAngle (out, primitive, projection, width, height, color, dpiScale);
            continue;
        }
        ScreenPoint first;
        if (!ProjectPoint (primitive, 0, projection, width, height, first))
            continue;
        if (primitive.kind == PrimitiveKind::Point) {
            AddLine (out, { first.x - 4.0f * dpiScale, first.y }, { first.x + 4.0f * dpiScale, first.y }, color,
                     2.0f * dpiScale);
            AddLine (out, { first.x, first.y - 4.0f * dpiScale }, { first.x, first.y + 4.0f * dpiScale }, color,
                     2.0f * dpiScale);
            AddText (out, primitive, { first.x + 6.0f * dpiScale, first.y + 6.0f * dpiScale }, color);
            continue;
        }
        if (primitive.kind == PrimitiveKind::Label) {
            AddText (out, primitive, first, color);
            continue;
        }
        ScreenPoint previous = first;
        const std::size_t pointCount = primitive.points.size ();
        for (std::size_t index = 1; index < pointCount; ++index) {
            ScreenPoint next;
            if (ProjectPoint (primitive, index, projection, width, height, next)) {
                AddLine (out, previous, next, color, 2.0f * dpiScale);
                previous = next;
            }
        }
        if (primitive.kind == PrimitiveKind::Polyline && primitive.closed && pointCount > 2)
            AddLine (out, previous, first, color, 2.0f * dpiScale);
        if (primitive.kind == PrimitiveKind::Arrow) {
            AddArrowhead (out, first, previous, color, dpiScale);
            AddText (out, primitive, { (first.x + previous.x) * 0.5f, (first.y + previous.y) * 0.5f }, color);
        }
    }
    ResolveLabelOverlaps (out, width, height, dpiScale, measureText);
    return out;
}

} // namespace geomsrv::archviz
