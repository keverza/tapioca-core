#ifndef TAPIOCA_ARCHVIZ_SUNPATHBANDDRAW_HPP
#define TAPIOCA_ARCHVIZ_SUNPATHBANDDRAW_HPP

#include "ArchViz/SunPathBand.hpp"

#include <imgui.h>

namespace geomsrv::archviz {

struct SunPathBandDrawing {
    size_t shown = 0, total = 0;
    bool decimated = false, hot = false;
};

inline float SunPathSegmentDistance (ImVec2 mouse, ImVec2 a, ImVec2 b)
{
    const float dx = b.x - a.x, dy = b.y - a.y, length2 = dx * dx + dy * dy;
    const float t =
        length2 > 0.0f ? std::clamp (((mouse.x - a.x) * dx + (mouse.y - a.y) * dy) / length2, 0.0f, 1.0f) : 0.0f;
    return std::hypot (mouse.x - a.x - t * dx, mouse.y - a.y - t * dy);
}

template <class Projection>
SunPathBandDrawing DrawSunPathBand (ImDrawList* draw, const SunPathBand& band, const SunPathMoment& selected, int month,
                                    int day, bool highlightAll, double sliceFrom, double sliceTo, const Projection& at)
{
    SunPathBandDrawing result;
    result.total = band.steps;
    const ImVec2 mouse = ImGui::GetIO ().MousePos;
    size_t highlightedDays = 0;
    for (const auto& traced : band.days)
        if ((month == 0 || traced.month == month) && (day == 0 || traced.day == day))
            ++highlightedDays;
    const int baseAlpha = (std::max) (1, 55 / int ((std::max) (size_t (1), band.days.size ())));
    const int hotAlpha = (std::max) (1, 100 / int ((std::max) (size_t (1), highlightedDays)));
    for (const auto& traced : band.days) {
        const bool highlighted =
            highlightAll || (month > 0 && traced.month == month && (day == 0 || traced.day == day));
        for (const auto& run : traced.runs) {
            size_t selectedIndex = run.size ();
            if (selected.valid && traced.year == selected.year && traced.month == selected.month &&
                traced.day == selected.day)
                for (size_t i = 0; i < run.size (); ++i)
                    if (run[i].step == selected.step)
                        selectedIndex = i;
            const auto indices = SunPathRunDisplayIndices (run, selectedIndex, sliceFrom, sliceTo);
            result.shown += indices.size ();
            result.decimated = result.decimated || indices.size () < run.size ();
            ImVec2 previous;
            bool had = false;
            size_t next = 0;
            int previousMinute = -1;
            // Project skipped samples too, so clipping still breaks a LOD run.
            for (size_t i = 0; i < run.size (); ++i) {
                ImVec2 screen;
                const bool visible = at (run[i].direction, screen);
                if (!visible)
                    had = false;
                if (next >= indices.size () || i != indices[next])
                    continue;
                ++next;
                if (!visible)
                    continue;
                const bool inSlice = run[i].minute >= sliceFrom && run[i].minute < sliceTo;
                const int alpha = highlighted && inSlice ? hotAlpha : baseAlpha;
                const ImU32 colour = IM_COL32 (255, 225, 90, alpha);
                if (traced.discrete) {
                    draw->AddCircleFilled (screen, 3.5f, colour, 12);
                    result.hot = result.hot || std::hypot (mouse.x - screen.x, mouse.y - screen.y) <= 7.0f;
                }
                else if (had) {
                    const bool previousInSlice = previousMinute >= sliceFrom && previousMinute < sliceTo;
                    draw->AddLine (previous, screen,
                                   highlighted && inSlice && previousInSlice ? IM_COL32 (255, 225, 90, hotAlpha)
                                                                             : IM_COL32 (255, 225, 90, baseAlpha),
                                   8.0f);
                    result.hot = result.hot || SunPathSegmentDistance (mouse, previous, screen) <= 7.0f;
                }
                else {
                    draw->AddCircleFilled (screen, 3.5f, colour, 12);
                    result.hot = result.hot || std::hypot (mouse.x - screen.x, mouse.y - screen.y) <= 7.0f;
                }
                previous = screen;
                previousMinute = run[i].minute;
                had = true;
            }
        }
    }
    return result;
}

inline std::string SunPathBandCaption (const SunPathBandDrawing& drawing, bool estimate)
{
    if (drawing.total == 0)
        return "No analysis suns in this window";
    std::string text = drawing.decimated
                           ? "LOD preview: " + std::to_string (drawing.shown) + " of " +
                                 std::to_string (drawing.total) + " analysis steps (before clipping)"
                           : "All " + std::to_string (drawing.total) + " analysis steps (before clipping)";
    if (estimate)
        text += "\nPreview estimate: traced days retain their calendar weights";
    return text;
}

} // namespace geomsrv::archviz

#endif
