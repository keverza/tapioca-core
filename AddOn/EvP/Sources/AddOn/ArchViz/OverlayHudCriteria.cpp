// OVERLAY-INVARIANTS.md: ImGui layout/input only; emits events after layout's lock.
#include "OverlayHudEngine.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <set>

namespace geomsrv::archviz::overlayhud {
void Engine::Impl::Criteria (const layers::Panel& panel, const layers::PanelItem& item, size_t index, PanelState& state,
                             float width)
{
    std::set<std::string> current;
    for (const auto& entry : panel.items)
        for (const auto& row : entry.criteria)
            current.insert (row.id);
    for (auto it = state.criteriaText.begin (); it != state.criteriaText.end ();)
        if (!current.contains (it->first))
            it = state.criteriaText.erase (it);
        else
            ++it;
    if (!ImGui::BeginTable ("##criteria", 8,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                ImGuiTableFlags_SizingStretchProp,
                            { width, 0 }))
        return;
    const float em = ImGui::GetFontSize ();
    ImGui::TableSetupColumn ("(", ImGuiTableColumnFlags_WidthFixed, 2 * em);
    ImGui::TableSetupColumn ("Criterion", ImGuiTableColumnFlags_WidthFixed, 10 * em);
    ImGui::TableSetupColumn ("Condition", ImGuiTableColumnFlags_WidthFixed, 7 * em);
    ImGui::TableSetupColumn ("Value", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn (")", ImGuiTableColumnFlags_WidthFixed, 2 * em);
    ImGui::TableSetupColumn ("Link", ImGuiTableColumnFlags_WidthFixed, 4 * em);
    ImGui::TableSetupColumn ("Order", ImGuiTableColumnFlags_WidthFixed, 6 * em);
    ImGui::TableSetupColumn ("Remove", ImGuiTableColumnFlags_WidthFixed, 4.5f * em);
    ImGui::TableHeadersRow ();
    for (const auto& row : item.criteria) {
        ImGui::PushID (row.id.c_str ());
        ImGui::TableNextRow ();
        const auto said = [&] (const char* kind, double value, const std::string& text = "") {
            changes.push_back ({ kind, key, panel.title, row.id, int32_t (index), value, text, true });
        };
        const auto combo = [&] (const char* id, const std::vector<std::string>& options, uint32_t selected,
                                const char* kind) {
            ImGui::SetNextItemWidth (-1);
            if (ImGui::BeginCombo (id, selected < options.size () ? options[selected].c_str () : "")) {
                for (size_t i = 0; i < options.size (); ++i)
                    if (ImGui::Selectable (options[i].c_str (), i == selected))
                        said (kind, double (i));
                ImGui::EndCombo ();
            }
        };
        ImGui::TableSetColumnIndex (0);
        combo ("##open", { "", "(", "((", "(((" }, row.open, "criteriaOpen");
        ImGui::TableSetColumnIndex (1);
        combo ("##field", item.labels, row.field, "criteriaField");
        ImGui::TableSetColumnIndex (2);
        combo ("##condition", item.columns, row.condition, "criteriaCondition");
        ImGui::TableSetColumnIndex (3);
        auto [it, inserted] = state.criteriaText.try_emplace (row.id);
        auto& held = it->second;
        if (inserted || held.field != row.field) {
            held.field = row.field;
            held.search.clear ();
        }
        if (inserted || held.sent != row.value) {
            held.sent = held.current = row.value;
        }
        std::array<char, 513> buffer {};
        std::memcpy (buffer.data (), held.current.data (), (std::min) (held.current.size (), buffer.size () - 1));
        const float buttonWidth = ImGui::GetFrameHeight ();
        ImGui::SetNextItemWidth ((std::max) (40.0f, ImGui::GetContentRegionAvail ().x - buttonWidth - 4));
        const bool enter =
            ImGui::InputText ("##value", buffer.data (), buffer.size (), ImGuiInputTextFlags_EnterReturnsTrue);
        held.current = buffer.data ();
        if (ImGui::IsItemActive ())
            store->criteriaTextActive = true;
        if (enter || ImGui::IsItemDeactivatedAfterEdit ())
            said ("criteriaValue", double (row.field), held.current);
        ImGui::SameLine (0, 4);
        if (ImGui::BeginCombo ("##values", "", ImGuiComboFlags_NoPreview | ImGuiComboFlags_HeightLarge)) {
            std::array<char, 513> search {};
            std::memcpy (search.data (), held.search.data (), (std::min) (held.search.size (), search.size () - 1));
            if (ImGui::IsWindowAppearing ())
                ImGui::SetKeyboardFocusHere ();
            ImGui::SetNextItemWidth ((std::max) (200.0f, ImGui::GetContentRegionAvail ().x));
            ImGui::InputTextWithHint ("##search", "Search captured values...", search.data (), search.size ());
            held.search = search.data ();
            if (ImGui::IsItemActive ())
                store->criteriaTextActive = true;
            const auto same = [] (unsigned char a, unsigned char b) {
                const auto fold = [] (unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c; };
                return fold (a) == fold (b);
            };
            size_t matches = 0;
            for (const auto& option : row.options)
                if ((held.search.empty () || std::search (option.begin (), option.end (), held.search.begin (),
                                                          held.search.end (), same) != option.end ()) &&
                    (++matches, ImGui::Selectable (option.c_str (), option == held.current))) {
                    held.current = option;
                    said ("criteriaValue", double (row.field), option);
                }
            if (!matches)
                ImGui::TextUnformatted ("No matching captured values");
            ImGui::EndCombo ();
        }
        ImGui::TableSetColumnIndex (4);
        combo ("##close", { "", ")", "))", ")))" }, row.close, "criteriaClose");
        ImGui::TableSetColumnIndex (5);
        combo ("##link", { "AND", "OR" }, row.orNext ? 1 : 0, "criteriaLink");
        ImGui::TableSetColumnIndex (6);
        if (ImGui::SmallButton ("Up"))
            said ("criteriaMove", -1);
        ImGui::SameLine ();
        if (ImGui::SmallButton ("Down"))
            said ("criteriaMove", 1);
        ImGui::TableSetColumnIndex (7);
        if (ImGui::SmallButton ("Remove"))
            said ("criteriaRemove", 1);
        ImGui::PopID ();
    }
    ImGui::EndTable ();
}
bool CriteriaTextActive (const State& state)
{
    return state.criteriaTextActive && state.shown;
}
} // namespace geomsrv::archviz::overlayhud
