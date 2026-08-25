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
              const std::string& fallback = {}, float fontSize = 0.0f, bool centered = false)
{
    const std::string text = primitive.text.empty () ? fallback : primitive.text;
    if (!text.empty ())
        out.labels.push_back ({ anchor, text, color, fontSize, centered });
}

void AddDimension (ProjectedDrawList& out, const Primitive& primitive, const float viewProj[16], uint32_t width,
                   uint32_t height, uint32_t color, float dpiScale)
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
    ScreenPoint da;
    ScreenPoint db;
    if (primitive.offset != 0.0) {
        const double wx = worldB[0] - worldA[0], wy = worldB[1] - worldA[1];
        const double planLength = std::sqrt (wx * wx + wy * wy);
        if (planLength <= 1.0e-12)
            return;
        const double ox = -wy / planLength * primitive.offset;
        const double oy = wx / planLength * primitive.offset;
        const double dimensionA[3] = { worldA[0] + ox, worldA[1] + oy, worldA[2] };
        const double dimensionB[3] = { worldB[0] + ox, worldB[1] + oy, worldB[2] };
        if (!Project (dimensionA, viewProj, width, height, da) || !Project (dimensionB, viewProj, width, height, db))
            return;
    }
    else {
        const ScreenPoint normal { -dy / screenLength * 14.0f * dpiScale, dx / screenLength * 14.0f * dpiScale };
        da = { a.x + normal.x, a.y + normal.y };
        db = { b.x + normal.x, b.y + normal.y };
    }
    AddLine (out, a, da, color, 2.0f * dpiScale);
    AddLine (out, b, db, color, 2.0f * dpiScale);
    AddLine (out, da, db, color, 2.0f * dpiScale);
    AddArrowhead (out, db, da, color, dpiScale);
    AddArrowhead (out, da, db, color, dpiScale);
    char measured[64];
    const double wx = worldB[0] - worldA[0], wy = worldB[1] - worldA[1], wz = worldB[2] - worldA[2];
    std::snprintf (measured, sizeof (measured), "%.3f m", std::sqrt (wx * wx + wy * wy + wz * wz));
    AddText (out, primitive, { (da.x + db.x) * 0.5f, (da.y + db.y) * 0.5f }, color, measured, 0.0f, true);
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
    std::snprintf (measured, sizeof (measured), "%.1f\xC2\xB0", glyph.degrees);
    AddText (out, primitive, { float (glyph.labelAnchor.x), float (glyph.labelAnchor.y) }, color, measured,
             float (glyph.fontSizePixels), true);
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
                                         float dpiScale, bool fitSelectedFrame)
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
            AddDimension (out, primitive, projection, width, height, color, dpiScale);
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
    return out;
}

} // namespace geomsrv::archviz
