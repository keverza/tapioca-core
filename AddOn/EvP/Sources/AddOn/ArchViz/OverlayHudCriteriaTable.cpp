// OVERLAY-INVARIANTS.md: ImGui layout/input only; the engine emits events after layout's lock.
#include "ArchViz/OverlayHudCriteriaTable.hpp"
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cstring>

namespace geomsrv::archviz::overlayhud {
namespace {
const std::vector<std::string> kOpen { "", "(", "((", "(((" };
const std::vector<std::string> kClose { "", ")", "))", ")))" };
const std::vector<std::string> kLink { "AND", "OR" };

float Text (const std::string& text)
{
    return ImGui::CalcTextSize (text.c_str ()).x;
}
float Combo (const std::vector<std::string>& options)
{
    float widest = 0;
    for (const auto& option : options)
        widest = (std::max) (widest, Text (option));
    const ImGuiStyle& style = ImGui::GetStyle ();
    return widest + 2 * style.FramePadding.x + ImGui::GetFrameHeight () + 2;
}
float Small (const char* label)
{
    return ImGui::CalcTextSize (label).x + 2 * ImGui::GetStyle ().FramePadding.x;
}
} // namespace

CriteriaWidths MeasureCriteria (const overlaylayers::PanelItem& item)
{
    const ImGuiStyle& style = ImGui::GetStyle ();
    const float em = ImGui::GetFontSize ();
    CriteriaWidths widths;
    widths.open = (std::max) (Combo (kOpen), Text ("("));
    widths.close = (std::max) (Combo (kClose), Text (")"));
    widths.field = (std::max) (Combo (item.labels), Text ("Criterion"));
    widths.condition = (std::max) (Combo (item.columns), Text ("Condition"));
    widths.value = (std::max) ({ 9 * em + 4 + ImGui::GetFrameHeight (), widths.field, Text ("Value") });
    widths.link = (std::max) (Combo (kLink), Text ("Link"));
    widths.order = (std::max) (2 * ImGui::GetFrameHeight () + style.ItemSpacing.x, Text ("Order"));
    widths.remove = (std::max) (Small ("Remove"), Text ("Remove"));
    // The stretch columns are weighted by their own minimum, so at the minimum every one
    // is exactly legible and any extra width is shared in proportion.
    widths.minimum = widths.open + widths.close + widths.link + widths.order + widths.remove + widths.field +
                     widths.condition + widths.value + 8 * (2 * style.CellPadding.x + 1) + 1;
    return widths;
}

void DrawCriteriaTable (const overlaylayers::PanelItem& item, std::map<std::string, CriteriaText>& texts, float width,
                        std::vector<CriteriaEdit>& edits, bool& textActive)
{
    const ImGuiStyle& style = ImGui::GetStyle ();
    const auto widths = MeasureCriteria (item);
    width = (std::max) (width, 1.0f);
    const bool scroll = widths.minimum > width;
    // A horizontally scrolling table is a child window: give it the rows' own height so
    // only this section scrolls, sideways, and the panel keeps its vertical flow.
    const float header = ImGui::GetTextLineHeight () + 2 * style.CellPadding.y + 1;
    const float row = ImGui::GetFrameHeight () + 2 * style.CellPadding.y + 1;
    const float height =
        header + row * float (item.criteria.size ()) + (scroll ? style.ScrollbarSize : 0.0f) + 2 + style.CellPadding.y;
    if (!ImGui::BeginTable ("##criteriaRules", 8,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX |
                                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings,
                            { width, height }, (std::max) (width, widths.minimum)))
        return;
    ImGui::TableSetupColumn ("(", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, widths.open);
    ImGui::TableSetupColumn ("Criterion", ImGuiTableColumnFlags_WidthStretch, widths.field);
    ImGui::TableSetupColumn ("Condition", ImGuiTableColumnFlags_WidthStretch, widths.condition);
    ImGui::TableSetupColumn ("Value", ImGuiTableColumnFlags_WidthStretch, widths.value);
    ImGui::TableSetupColumn (")", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, widths.close);
    ImGui::TableSetupColumn ("Link", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, widths.link);
    ImGui::TableSetupColumn ("Order", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, widths.order);
    ImGui::TableSetupColumn ("Remove", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize,
                             widths.remove);
    ImGui::TableHeadersRow ();
    for (const auto& rule : item.criteria) {
        ImGui::PushID (rule.id.c_str ());
        ImGui::TableNextRow ();
        const auto said = [&] (const char* kind, double value, const std::string& text = "") {
            edits.push_back ({ kind, rule.id, value, text });
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
            if (ImGui::IsItemHovered () && selected < options.size () &&
                ImGui::CalcTextSize (options[selected].c_str ()).x > ImGui::GetItemRectSize ().x)
                ImGui::SetTooltip ("%s", options[selected].c_str ());
        };
        ImGui::TableSetColumnIndex (0);
        combo ("##open", kOpen, rule.open, "criteriaOpen");
        ImGui::TableSetColumnIndex (1);
        combo ("##field", item.labels, rule.field, "criteriaField");
        ImGui::TableSetColumnIndex (2);
        combo ("##condition", item.columns, rule.condition, "criteriaCondition");
        ImGui::TableSetColumnIndex (3);
        auto [it, inserted] = texts.try_emplace (rule.id);
        auto& held = it->second;
        if (inserted || held.field != rule.field) {
            held.field = rule.field;
            held.search.clear ();
        }
        if (inserted || held.sent != rule.value) {
            held.sent = held.current = rule.value;
        }
        std::array<char, 513> buffer {};
        std::memcpy (buffer.data (), held.current.data (), (std::min) (held.current.size (), buffer.size () - 1));
        const float buttonWidth = ImGui::GetFrameHeight ();
        ImGui::SetNextItemWidth ((std::max) (40.0f, ImGui::GetContentRegionAvail ().x - buttonWidth - 4));
        const bool enter =
            ImGui::InputText ("##value", buffer.data (), buffer.size (), ImGuiInputTextFlags_EnterReturnsTrue);
        held.current = buffer.data ();
        if (ImGui::IsItemActive ())
            textActive = true;
        if (enter || ImGui::IsItemDeactivatedAfterEdit ())
            said ("criteriaValue", double (rule.field), held.current);
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
                textActive = true;
            const auto same = [] (unsigned char a, unsigned char b) {
                const auto fold = [] (unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c; };
                return fold (a) == fold (b);
            };
            size_t matches = 0;
            for (const auto& option : rule.options)
                if ((held.search.empty () || std::search (option.begin (), option.end (), held.search.begin (),
                                                          held.search.end (), same) != option.end ()) &&
                    (++matches, ImGui::Selectable (option.c_str (), option == held.current))) {
                    held.current = option;
                    said ("criteriaValue", double (rule.field), option);
                }
            if (!matches)
                ImGui::TextUnformatted ("No matching captured values");
            ImGui::EndCombo ();
        }
        ImGui::TableSetColumnIndex (4);
        combo ("##close", kClose, rule.close, "criteriaClose");
        ImGui::TableSetColumnIndex (5);
        combo ("##link", kLink, rule.orNext ? 1 : 0, "criteriaLink");
        ImGui::TableSetColumnIndex (6);
        if (ImGui::ArrowButton ("##up", ImGuiDir_Up))
            said ("criteriaMove", -1);
        if (ImGui::IsItemHovered ())
            ImGui::SetTooltip ("Move up");
        ImGui::SameLine ();
        if (ImGui::ArrowButton ("##down", ImGuiDir_Down))
            said ("criteriaMove", 1);
        if (ImGui::IsItemHovered ())
            ImGui::SetTooltip ("Move down");
        ImGui::TableSetColumnIndex (7);
        if (ImGui::SmallButton ("Remove"))
            said ("criteriaRemove", 1);
        ImGui::PopID ();
    }
    ImGui::EndTable ();
}

} // namespace geomsrv::archviz::overlayhud
