#include "ArchViz/HudGumball.hpp"
#include "ArchViz/MatrixMath.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv::archviz {

namespace {

bool Project (const double point[3], const float matrix[16], uint32_t width, uint32_t height, ImVec2& pixel)
{
    const float world[4] = { float (point[0]), float (point[1]), float (point[2]), 1.0f };
    float clip[4];
    TransformPoint (clip, world, matrix);
    if (!std::isfinite (clip[3]) || clip[3] <= 1.0e-6f || clip[2] < 0.0f || clip[2] > clip[3])
        return false;
    pixel =
        ImVec2 ((clip[0] / clip[3] * 0.5f + 0.5f) * float (width), (0.5f - clip[1] / clip[3] * 0.5f) * float (height));
    return std::isfinite (pixel.x) && std::isfinite (pixel.y);
}

} // namespace

Gumball MakeGumball (const double point[3], const float matrix[16], uint32_t width, uint32_t height)
{
    Gumball result;
    result.valid = Project (point, matrix, width, height, result.center);
    if (!result.valid)
        return result;
    float pixelsPerMetre = 0.0f;
    for (int axis = 0; axis < 3; ++axis) {
        double tip[3] = { point[0], point[1], point[2] };
        tip[axis] += 1.0;
        ImVec2 pixel;
        if (Project (tip, matrix, width, height, pixel))
            pixelsPerMetre =
                (std::max) (pixelsPerMetre, std::hypot (pixel.x - result.center.x, pixel.y - result.center.y));
    }
    if (pixelsPerMetre <= 1.0e-4f)
        return result;
    const double length = 70.0 / pixelsPerMetre;
    for (int axis = 0; axis < 3; ++axis) {
        double tip[3] = { point[0], point[1], point[2] };
        tip[axis] += length;
        result.visible[axis] =
            Project (tip, matrix, width, height, result.ends[axis]) &&
            std::hypot (result.ends[axis].x - result.center.x, result.ends[axis].y - result.center.y) > 15.0f;
    }
    return result;
}

int HitGumball (const Gumball& gizmo, float x, float y)
{
    if (!gizmo.valid)
        return -1;
    if (std::hypot (x - gizmo.center.x, y - gizmo.center.y) <= 9.0f)
        return 3;
    int picked = -1;
    float best = 9.0f;
    for (int axis = 0; axis < 3; ++axis) {
        if (!gizmo.visible[axis])
            continue;
        const float dx = gizmo.ends[axis].x - gizmo.center.x, dy = gizmo.ends[axis].y - gizmo.center.y;
        const float t = (std::max) (0.18f, (std::min) (1.1f, ((x - gizmo.center.x) * dx + (y - gizmo.center.y) * dy) /
                                                                 (dx * dx + dy * dy)));
        const float distance = std::hypot (x - gizmo.center.x - t * dx, y - gizmo.center.y - t * dy);
        if (distance < best) {
            best = distance;
            picked = axis;
        }
    }
    return picked;
}

namespace {

ImU32 Lighter (ImU32 colour)
{
    ImVec4 c = ImGui::ColorConvertU32ToFloat4 (colour);
    c.x += (1.0f - c.x) * 0.45f;
    c.y += (1.0f - c.y) * 0.45f;
    c.z += (1.0f - c.z) * 0.45f;
    return ImGui::ColorConvertFloat4ToU32 (c);
}

} // namespace

void DrawGumballMarker (ImVec2 at, ImDrawList* draw, bool hot, ImU32 centre)
{
    if (draw == nullptr)
        return;
    draw->AddCircleFilled (at, hot ? 8.0f : 6.0f, hot ? Lighter (centre) : centre);
    draw->AddCircle (at, hot ? 10.5f : 8.0f, hot ? IM_COL32 (255, 255, 255, 230) : IM_COL32 (20, 24, 28, 200), 0,
                     hot ? 2.0f : 1.5f);
}

void DrawGumball (const Gumball& gizmo, ImDrawList* draw, int hot, ImU32 centre)
{
    if (!gizmo.valid || draw == nullptr)
        return;
    const ImU32 colours[3] = { IM_COL32 (235, 80, 70, 255), IM_COL32 (70, 190, 100, 255),
                               IM_COL32 (80, 140, 245, 255) };
    const ImU32 hotColours[3] = { IM_COL32 (255, 160, 150, 255), IM_COL32 (160, 235, 175, 255),
                                  IM_COL32 (165, 200, 255, 255) };
    const char* names[3] = { "X", "Y", "Z" };
    for (int axis = 0; axis < 3; ++axis) {
        if (!gizmo.visible[axis])
            continue;
        const bool lit = hot == axis;
        const ImU32 colour = lit ? hotColours[axis] : colours[axis];
        const float head = lit ? 13.0f : 10.0f;
        const auto tip = gizmo.ends[axis];
        draw->AddLine (gizmo.center, tip, colour, lit ? 5.0f : 3.0f);
        const float dx = tip.x - gizmo.center.x, dy = tip.y - gizmo.center.y;
        const float length = std::hypot (dx, dy), ux = dx / length, uy = dy / length;
        draw->AddTriangleFilled (
            tip, ImVec2 (tip.x - ux * head - uy * head * 0.5f, tip.y - uy * head + ux * head * 0.5f),
            ImVec2 (tip.x - ux * head + uy * head * 0.5f, tip.y - uy * head - ux * head * 0.5f), colour);
        draw->AddText (ImVec2 (tip.x + 5, tip.y + 5), colour, names[axis]);
    }
    if (hot == 3) {
        draw->AddCircleFilled (gizmo.center, 9.5f, Lighter (centre));
        draw->AddCircle (gizmo.center, 12.0f, IM_COL32 (255, 255, 255, 220), 0, 2.0f);
    }
    else
        draw->AddCircleFilled (gizmo.center, 7.0f, centre);
}

} // namespace geomsrv::archviz
