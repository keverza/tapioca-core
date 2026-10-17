#include "ArchViz/HudFloorProgramme.hpp"
#include "ArchViz/HudShell.hpp"
#include <imgui.h>
#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::hudprogramme {
namespace fp = floorprogramme;
Outcome Draw (fp::Programme& programme, const fp::Programme& saved, std::vector<TableEdit>& tables,
              std::vector<ColourEdit>& colours)
{
    if (!ImGui::CollapsingHeader ("Define Programme"))
        return {};
    auto wanted = programme;
    bool changed = false, save = false;
    const float unit = ImGui::GetFontSize ();
    if (ImGui::BeginTable ("##massing.programme", 6,
                           ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings |
                               ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn ("##colour", ImGuiTableColumnFlags_WidthFixed, 1.4f * unit);
        ImGui::TableSetupColumn ("Rooms", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn ("Min", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn ("Max", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn ("Share", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn ("##remove", ImGuiTableColumnFlags_WidthFixed, 1.2f * unit);
        ImGui::TableHeadersRow ();
        size_t remove = wanted.types.size ();
        for (size_t i = 0; i < wanted.types.size (); ++i) {
            auto& type = wanted.types[i];
            ImGui::PushID (int (i));
            ImGui::TableNextRow ();
            ImGui::TableSetColumnIndex (0);
            const auto colour = fp::Colour (type);
            ImGui::ColorButton ("##colour", hudshell::Colour (colour), ImGuiColorEditFlags_NoTooltip,
                                { unit, ImGui::GetFrameHeight () });
            if (ImGui::IsItemHovered ()) {
                if (ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left))
                    colours.push_back ({ programme, i });
                if (ImGui::IsMouseClicked (ImGuiMouseButton_Right)) {
                    hudshell::ClaimRightClick ();
                    ImGui::OpenPopup ("##colour.menu");
                }
                hudshell::Tip (fp::Name (wanted, i) + ": double-click to choose a colour; right-click to reset it.",
                               hudshell::TipSide::Above);
            }
            if (ImGui::BeginPopup ("##colour.menu")) {
                if (ImGui::MenuItem ("Use default colour", nullptr, false, type.rgba != 0) &&
                    fp::SetColour (wanted, i, 0))
                    changed = save = true;
                ImGui::EndPopup ();
            }
            const auto editCell = [&] (Field field) {
                if (ImGui::IsItemHovered () && ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left))
                    tables.push_back ({ programme, i, field });
                hudshell::Tip ("Drag to adjust; double-click to type in the Programme table.",
                               hudshell::TipSide::Above);
            };
            ImGui::TableSetColumnIndex (1);
            double rooms = type.rooms;
            const double roomsLow = fp::kMinRooms, roomsHigh = fp::kMaxRooms;
            ImGui::SetNextItemWidth (-1);
            if (ImGui::DragScalar ("##rooms", ImGuiDataType_Double, &rooms, 0.05f, &roomsLow, &roomsHigh, "%.1f",
                                   ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp))
                changed |= fp::SetRooms (wanted, i, rooms);
            save |= ImGui::IsItemDeactivatedAfterEdit ();
            editCell (Field::Rooms);
            ImGui::TableSetColumnIndex (2);
            double low = type.minM2, high = type.maxM2;
            const double floor = fp::kMinArea, ceiling = fp::kMaxArea;
            const double lowTop = high - 0.5, highBottom = low + 0.5;
            ImGui::SetNextItemWidth (-1);
            const bool lowMoved = ImGui::DragScalar ("##min", ImGuiDataType_Double, &low, 0.25f, &floor, &lowTop,
                                                     "%.1f", ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp);
            save |= ImGui::IsItemDeactivatedAfterEdit ();
            editCell (Field::Minimum);
            ImGui::TableSetColumnIndex (3);
            ImGui::SetNextItemWidth (-1);
            const bool highMoved =
                ImGui::DragScalar ("##max", ImGuiDataType_Double, &high, 0.25f, &highBottom, &ceiling, "%.1f",
                                   ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp);
            save |= ImGui::IsItemDeactivatedAfterEdit ();
            editCell (Field::Maximum);
            if (lowMoved || highMoved)
                changed |= fp::SetRange (wanted, i, std::round (low * 2) / 2, std::round (high * 2) / 2);
            ImGui::TableSetColumnIndex (4);
            double share = type.share * 100;
            const double none = 0, all = 100;
            ImGui::SetNextItemWidth (-1);
            if (ImGui::DragScalar ("##share", ImGuiDataType_Double, &share, 0.2f, &none, &all, "%.1f%%",
                                   ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp))
                changed |= fp::SetShare (wanted, i, std::round (share * 10) / 1000);
            save |= ImGui::IsItemDeactivatedAfterEdit ();
            editCell (Field::Share);
            hudshell::Tip ("Share of the flat count. Drag scales other types to keep 100%; double-click to type.",
                           hudshell::TipSide::Above);
            ImGui::TableSetColumnIndex (5);
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
    const float right = ImGui::GetCursorScreenPos ().x + ImGui::GetContentRegionAvail ().x;
    const auto beside = [&] (const char* label) {
        const float width = ImGui::CalcTextSize (label).x + 2 * ImGui::GetStyle ().FramePadding.x;
        if (ImGui::GetItemRectMax ().x + ImGui::GetStyle ().ItemSpacing.x + width <= right)
            ImGui::SameLine ();
    };
    ImGui::BeginDisabled (wanted.types.size () >= fp::kMaxTypes);
    if (ImGui::SmallButton ("Add type") && fp::AddType (wanted) < fp::kMaxTypes)
        changed = save = true;
    ImGui::EndDisabled ();
    beside ("Edit table...");
    if (ImGui::SmallButton ("Edit table..."))
        tables.push_back ({ programme });
    beside ("Default");
    if (ImGui::SmallButton ("Default") && !(wanted == fp::Default ())) {
        wanted = fp::Default ();
        changed = save = true;
    }
    double total = 0;
    for (const auto& type : wanted.types)
        total += type.share;
    ImGui::TextDisabled ("Total %.1f%%", total * 100);
    ImGui::TextDisabled ("Mean flat %.1f m2 net", fp::MeanArea (wanted));
    ImGui::TextWrapped ("Min/Max: net m2 inside walls. Share: % of flat count.");
    if (wanted == saved)
        ImGui::TextDisabled ("Saved with the project.");
    else {
        ImGui::AlignTextToFramePadding ();
        ImGui::TextUnformatted ("Unsaved programme.");
        beside ("Save");
        save |= ImGui::SmallButton ("Save");
        hudshell::Tip ("Store the programme in the project (one Undo step). Edits save when you let go.",
                       hudshell::TipSide::Above);
    }
    Outcome outcome;
    if (changed && fp::Valid (wanted)) {
        programme = std::move (wanted);
        outcome.changed = true;
    }
    outcome.save = save && fp::Valid (programme) && !(programme == saved);
    return outcome;
}

bool PrepareTable (fp::Programme values, fp::Programme& answer, std::string& error)
{
    double total = 0;
    auto bounded = values;
    for (auto& type : bounded.types) {
        if (!std::isfinite (type.share) || type.share < 0 || type.share > 1) {
            error = "Each share must be a finite percentage from 0 to 100.";
            return false;
        }
        total += type.share;
        type.share = 1.0 / double (bounded.types.size ());
    }
    error = fp::Problem (bounded);
    if (!error.empty ())
        return false;
    if (total <= 0) {
        error = "Set at least one non-zero share.";
        return false;
    }
    fp::Normalise (values);
    answer = std::move (values);
    return true;
}

bool Answer (fp::Programme& programme, const TableEdit& edit, const fp::Programme& answer, std::string& error)
{
    if (!(programme == edit.before)) {
        error = "The programme changed while the table was open; not applied.";
        return false;
    }
    error = fp::Problem (answer);
    if (!error.empty ())
        return false;
    programme = answer;
    return true;
}
bool Answer (fp::Programme& programme, const ColourEdit& edit, uint32_t rgba, std::string& error)
{
    if (!(programme == edit.before)) {
        error = "The programme changed while the colour picker was open; not applied.";
        return false;
    }
    if (edit.type >= programme.types.size () || (rgba != 0 && (rgba & 0xFFu) != 0xFFu)) {
        error = "Choose an opaque RGB colour for an existing type.";
        return false;
    }
    fp::SetColour (programme, edit.type, rgba);
    error.clear ();
    return true;
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
    wanted.types[i] = { row.rooms, row.minM2, row.maxM2, wanted.types[i].share, wanted.types[i].rgba };
    fp::SetShare (wanted, i, row.share);
    error = fp::Problem (wanted);
    if (!error.empty ())
        return false;
    programme = std::move (wanted);
    return true;
}
} // namespace geomsrv::archviz::hudprogramme
