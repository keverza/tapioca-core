#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentHudSunControls.hpp"
#include "NativeCommands/SunStudySeasonal.hpp"

#include <imgui.h>

namespace geomsrv::archviz {

void DrawSunStudySeasonalClocks (HudState& state, const sunstudyhud::PeriodProfile* profile, bool estimate)
{
    if (profile == nullptr)
        return;
    ImGui::Checkbox ("Seasons##sunpoint", &state.sunPeriodSeasons);
    ImGui::SetItemTooltip (
        "Winter, spring, summer and autumn clocks from the retained point profile (meteorological seasons, "
        "named for the project's hemisphere). Display only.");
    if (!state.sunPeriodSeasons)
        return;
    const auto seasons = sunstudyhud::SeasonalPointDays (*profile);
    // One column, each clock the panel's full width.
    for (const auto& season : seasons) {
        ImGui::Spacing ();
        ImGui::TextUnformatted (season.name.c_str ());
        ImGui::SameLine ();
        ImGui::TextDisabled ("%s, %zu traced day(s)", season.months.c_str (), season.tracedDays);
        ImGui::SetItemTooltip ("%.0f represented day(s)", season.representedDays);
        const std::string id = "##season" + season.label;
        DrawSunStudyDayClock (state, season.day.minutes.empty () ? nullptr : &season.day, id.c_str (), -1,
                              "no measured sun", nullptr, 4096.0f, nullptr, SunChart::Season);
    }
    SunStudyNote (estimate ? "Seasons: Preview estimate over representative traced days"
                           : "Seasons: every traced day at weight 1",
                  "Meteorological seasons (Dec-Feb, Mar-May, Jun-Aug, Sep-Nov), named winter to autumn for the "
                  "hemisphere the measured sun shows. Each clock's centre is its weighted mean direct hours on a "
                  "traced day. Clock steps average only days that measured that sun time. Weights never create extra "
                  "frames. These clocks are display-only and have no separate cell-range claim.");
}

} // namespace geomsrv::archviz
