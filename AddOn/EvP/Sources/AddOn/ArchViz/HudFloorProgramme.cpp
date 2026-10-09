#include "ArchViz/HudFloorProgramme.hpp"
#include "ArchViz/HudShell.hpp"
#include <imgui.h>
#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::hudprogramme {
namespace fp = floorprogramme;
Outcome Draw (fp::Programme& programme, const fp::Programme& saved, std::vector<TextEdit>& prompts)
{
    if (!ImGui::CollapsingHeader ("Define Programme"))
        return {};
    auto wanted = programme;
    bool changed = false, save = false;
    const float unit = ImGui::GetFontSize ();
    if (ImGui::BeginTable ("##massing.programme", 5,
                           ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn ("Type", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn ("Rooms", ImGuiTableColumnFlags_WidthFixed, 3.5f * unit);
        ImGui::TableSetupColumn ("Net m2", ImGuiTableColumnFlags_WidthFixed, 8 * unit);
        ImGui::TableSetupColumn ("Share", ImGuiTableColumnFlags_WidthFixed, 4.5f * unit);
        ImGui::TableSetupColumn ("##edit", ImGuiTableColumnFlags_WidthFixed, 3.5f * unit);
        ImGui::TableHeadersRow ();
        size_t remove = wanted.types.size ();
        for (size_t i = 0; i < wanted.types.size (); ++i) {
            auto& type = wanted.types[i];
            ImGui::PushID (int (i));
            ImGui::TableNextRow ();
            ImGui::TableSetColumnIndex (0);
            const auto colour = fp::Colour (type.rooms);
            const ImVec2 at = ImGui::GetCursorScreenPos ();
            ImGui::GetWindowDrawList ()->AddRectFilled (at, { at.x + 0.7f * unit, at.y + 0.9f * unit },
                                                        hudshell::Packed (colour));
            ImGui::Dummy ({ 0.8f * unit, 0 });
            ImGui::SameLine ();
            ImGui::AlignTextToFramePadding ();
            ImGui::TextUnformatted (fp::Name (wanted, i).c_str ());
            ImGui::TableSetColumnIndex (1);
            double rooms = type.rooms;
            const double roomsLow = fp::kMinRooms, roomsHigh = fp::kMaxRooms;
            ImGui::SetNextItemWidth (-1);
            if (ImGui::DragScalar ("##rooms", ImGuiDataType_Double, &rooms, 0.05f, &roomsLow, &roomsHigh, "%.1f",
                                   ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp))
                changed |= fp::SetRooms (wanted, i, rooms);
            save |= ImGui::IsItemDeactivatedAfterEdit ();
            ImGui::TableSetColumnIndex (2);
            double low = type.minM2, high = type.maxM2;
            const double floor = fp::kMinArea, ceiling = fp::kMaxArea;
            const double lowTop = high - 0.5, highBottom = low + 0.5;
            const float half = (ImGui::GetContentRegionAvail ().x - ImGui::GetStyle ().ItemSpacing.x) / 2;
            ImGui::SetNextItemWidth (half);
            const bool lowMoved = ImGui::DragScalar ("##min", ImGuiDataType_Double, &low, 0.25f, &floor, &lowTop,
                                                     "%.1f", ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp);
            save |= ImGui::IsItemDeactivatedAfterEdit ();
            ImGui::SameLine ();
            ImGui::SetNextItemWidth (half);
            const bool highMoved =
                ImGui::DragScalar ("##max", ImGuiDataType_Double, &high, 0.25f, &highBottom, &ceiling, "%.1f",
                                   ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp);
            save |= ImGui::IsItemDeactivatedAfterEdit ();
            if (lowMoved || highMoved)
                changed |= fp::SetRange (wanted, i, std::round (low * 2) / 2, std::round (high * 2) / 2);
            ImGui::TableSetColumnIndex (3);
            double share = type.share * 100;
            const double none = 0, all = 100;
            ImGui::SetNextItemWidth (-1);
            if (ImGui::DragScalar ("##share", ImGuiDataType_Double, &share, 0.2f, &none, &all, "%.1f%%",
                                   ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp))
                changed |= fp::SetShare (wanted, i, std::round (share * 10) / 1000);
            save |= ImGui::IsItemDeactivatedAfterEdit ();
            if (ImGui::IsItemHovered ())
                ImGui::SetTooltip ("Share of the flat count. The other types scale so the total stays 100%%.");
            ImGui::TableSetColumnIndex (4);
            if (ImGui::SmallButton ("Set"))
                prompts.push_back ({ programme, int (i), fp::Brief ({ { type } }) });
            if (ImGui::IsItemHovered ())
                ImGui::SetTooltip ("Type this row, e.g. \"30%% 2 room 40-45m2\".");
            ImGui::SameLine ();
            ImGui::BeginDisabled (wanted.types.size () <= 1);
            if (ImGui::SmallButton ("x"))
                remove = i;
            ImGui::EndDisabled ();
            ImGui::PopID ();
        }
        ImGui::EndTable ();
        if (remove < wanted.types.size () && fp::RemoveType (wanted, remove))
            changed = save = true;
    }
    ImGui::BeginDisabled (wanted.types.size () >= fp::kMaxTypes);
    if (ImGui::SmallButton ("Add type") && fp::AddType (wanted) < fp::kMaxTypes)
        changed = save = true;
    ImGui::EndDisabled ();
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Set brief..."))
        prompts.push_back ({ programme, -1, fp::Brief (programme) });
    if (ImGui::IsItemHovered ())
        ImGui::SetTooltip ("Type every type at once: \"5%% 1.5 room 30-36m2; 30%% 2 room 40-45m2; ...\". Shares are "
                           "scaled to 100%%.");
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Default") && !(wanted == fp::Default ())) {
        wanted = fp::Default ();
        changed = save = true;
    }
    double total = 0;
    for (const auto& type : wanted.types)
        total += type.share;
    ImGui::TextDisabled ("Total %.1f%% | mean flat %.1f m2 net | net = inside walls, as the plan view measures",
                         total * 100, fp::MeanArea (wanted));
    if (wanted == saved)
        ImGui::TextDisabled ("Saved with the project.");
    else {
        ImGui::AlignTextToFramePadding ();
        ImGui::TextUnformatted ("Not saved with the project yet.");
        ImGui::SameLine ();
        save |= ImGui::SmallButton ("Save");
        if (ImGui::IsItemHovered ())
            ImGui::SetTooltip ("Store the programme in the project (one Undo step). Edits save when you let go.");
    }
    Outcome outcome;
    if (changed && fp::Valid (wanted)) {
        programme = std::move (wanted);
        outcome.changed = true;
    }
    outcome.save = save && fp::Valid (programme) && !(programme == saved);
    return outcome;
}
bool Answer (fp::Programme& programme, const TextEdit& edit, const std::string& answer, std::string& error)
{
    if (!(programme == edit.before)) {
        error = "The programme changed while the prompt was open; not applied.";
        return false;
    }
    if (edit.type < 0) {
        fp::Programme parsed;
        if (!fp::Parse (answer, parsed, error))
            return false;
        programme = std::move (parsed);
        return true;
    }
    if (size_t (edit.type) >= programme.types.size ()) {
        error = "That flat type no longer exists.";
        return false;
    }
    fp::UnitType row;
    if (!fp::ParseType (answer, row, error))
        return false;
    auto wanted = programme;
    const size_t i = size_t (edit.type);
    wanted.types[i] = { row.rooms, row.minM2, row.maxM2, wanted.types[i].share };
    fp::SetShare (wanted, i, row.share);
    error = fp::Problem (wanted);
    if (!error.empty ())
        return false;
    programme = std::move (wanted);
    return true;
}
} // namespace geomsrv::archviz::hudprogramme
