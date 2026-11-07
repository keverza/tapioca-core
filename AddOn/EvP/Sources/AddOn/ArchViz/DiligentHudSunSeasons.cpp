#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentHudSunControls.hpp"
#include "NativeCommands/SunStudySeasonal.hpp"

#include <imgui.h>

namespace geomsrv::archviz {

void DrawSunStudySeasonalClocks (HudState& state, const sunstudyhud::PeriodProfile* profile, bool estimate)
{
    if (profile == nullptr)
        return;
    ImGui::Checkbox ("Seasonal clocks##sunpoint", &state.sunPeriodSeasons);
    ImGui::SetItemTooltip ("Display-only DJF/MAM/JJA/SON small clocks from the retained point profile. No whole-map "
                           "archive and no independent selection state.");
    if (!state.sunPeriodSeasons)
        return;
    const auto seasons = sunstudyhud::SeasonalPointDays (*profile);
    if (ImGui::BeginTable ("##sunpointseasons", 2, ImGuiTableFlags_SizingStretchSame)) {
        for (const auto& season : seasons) {
            ImGui::TableNextColumn ();
            ImGui::TextDisabled ("%s: %zu traced day(s), %.0f represented", season.label.c_str (), season.tracedDays,
                                 season.representedDays);
            const std::string id = "##season" + season.label;
            DrawSunStudyDayClock (state, season.day.minutes.empty () ? nullptr : &season.day, id.c_str (), -1,
                                  "no measured sun");
        }
        ImGui::EndTable ();
    }
    SunStudyNote (estimate ? "Seasonal clocks: Preview estimate over representative traced days"
                           : "Seasonal clocks: every traced day at weight 1",
                  "Meteorological seasons DJF/MAM/JJA/SON. Each clock's centre is its weighted mean direct hours on a "
                  "traced day. Clock steps average only days that measured that sun time. Weights never create extra "
                  "frames. These clocks are display-only and have no separate cell-range claim.");
}

} // namespace geomsrv::archviz
