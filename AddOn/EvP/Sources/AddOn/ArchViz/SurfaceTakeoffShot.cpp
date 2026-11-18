// The Exposed surfaces screenshot's area table: see SurfaceTakeoffShot.hpp.
#include "ArchViz/SurfaceTakeoffShot.hpp"

#include "Geometry/SurfaceTakeoffResult.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <sstream>

namespace geomsrv::archviz::surfacetakeoff {
namespace {

std::string Fixed (double value, int digits)
{
    std::ostringstream text;
    text << std::fixed << std::setprecision (digits) << value;
    return text.str ();
}

const char* ScopeText (const std::string& classification)
{
    if (classification == "allElementFaces-contactOnly")
        return "Captured construction: walls, slabs, columns and beams, interiors included";
    if (classification == "allWallFaces-contactOnly")
        return "Captured walls, interiors included";
    if (classification == "userExteriorSeeds-connectedCoplanar")
        return "User-confirmed exterior facade faces";
    return nullptr;
}

// Rounded the way the table shows it, for the rounding footnote.
double Shown (double area)
{
    return std::round (area * 1000.0) / 1000.0;
}

void RightAligned (const std::string& text)
{
    const float width = ImGui::CalcTextSize (text.c_str ()).x;
    const float room = ImGui::GetContentRegionAvail ().x;
    if (room > width)
        ImGui::SetCursorPosX (ImGui::GetCursorPosX () + room - width);
    ImGui::TextUnformatted (text.c_str ());
}

// A flat swatch on the draw list: no widget frame in the image.
void Swatch (uint32_t rgba)
{
    const float size = ImGui::GetTextLineHeight ();
    const ImVec2 at = ImGui::GetCursorScreenPos ();
    const ImU32 colour = IM_COL32 ((rgba >> 24) & 255u, (rgba >> 16) & 255u, (rgba >> 8) & 255u, 255);
    ImDrawList* list = ImGui::GetWindowDrawList ();
    list->AddRectFilled (at, ImVec2 (at.x + size, at.y + size), colour);
    list->AddRect (at, ImVec2 (at.x + size, at.y + size), IM_COL32 (0, 0, 0, 60));
    ImGui::Dummy (ImVec2 (size, size));
}

} // namespace

std::string ShotArea (double areaM2)
{
    return areaM2 > 0 && areaM2 < 0.0005 ? std::string ("<0.001") : Fixed (areaM2, 3);
}

bool BuildShotTable (const geomsrv::surfacetakeoff::Result& result, const HighlightSummary& highlight,
                     uint64_t snapshotId, const std::string& timestamp, ShotTable& table, std::string& error)
{
    table = {};
    error.clear ();
    const char* scope = ScopeText (result.classification);
    if (highlight.generation == 0 || scope == nullptr) {
        error = "there is no current calculation to export";
        return false;
    }
    ShotTable pinned;
    pinned.generation = highlight.generation;
    pinned.snapshotId = snapshotId;
    pinned.material = highlight.material;
    pinned.materials = highlight.materials;
    pinned.contextStyle = highlight.contextStyle;
    pinned.contextOpacity = highlight.contextOpacity;
    std::string isolatedName;
    for (const auto& row : result.surfaces) {
        if (highlight.material && *highlight.material != row.surfaceAttributeIndex)
            continue;
        const auto area = highlight.rowAreas.find (row.surfaceAttributeIndex);
        if (area == highlight.rowAreas.end () || area->second <= 0)
            continue;
        const auto elements = highlight.rowElements.find (row.surfaceAttributeIndex);
        pinned.rows.push_back ({ row.surfaceAttributeIndex, row.name, row.rgba,
                                 elements == highlight.rowElements.end () ? 0 : elements->second, area->second });
        pinned.totalM2 += area->second;
        if (highlight.material)
            isolatedName = row.name + " [" + std::to_string (row.surfaceAttributeIndex) + "]";
    }
    if (pinned.rows.empty ()) {
        error = "no measured surface is highlighted";
        return false;
    }
    // The table and the image are one subset: what the table sums is what is highlighted.
    if (!std::isfinite (pinned.totalM2) || std::abs (pinned.totalM2 - highlight.areaM2) > 1e-6) {
        error = "the area table and the highlighted surfaces differ";
        return false;
    }
    pinned.elements = highlight.elements;
    double shownSum = 0;
    for (const auto& row : pinned.rows)
        shownSum += Shown (row.areaM2);
    pinned.roundingNote = std::abs (shownSum - Shown (pinned.totalM2)) > 0.0005;

    pinned.title = highlight.material ? "ISOLATED MATERIAL - SUBTOTAL" : "EXPOSED SURFACE AREAS";
    pinned.subtitle = std::string ("Exposed | ") + scope;
    if (highlight.material)
        pinned.subtitle += " | Isolated: " + isolatedName + ". Other materials are not in this subtotal.";
    else if (!highlight.materials.empty ())
        pinned.subtitle += " | Filtered to " + std::to_string (highlight.materials.size ()) + " selected material" +
                           (highlight.materials.size () == 1 ? "" : "s");
    pinned.subtitle += " | Complete for the captured scope";

    pinned.footer.push_back ("Source material swatches. Measured faces are highlighted in their own surface colour" +
                             std::string (highlight.Focused () ? highlight.contextStyle == ContextStyle::WhiteModel
                                                                     ? "; the model is shown white."
                                                                     : "; the model is shown desaturated."
                                                               : "."));
    pinned.footer.push_back ("Covers: " + result.coverScopeId.substr (0, result.coverScopeId.find (':')) + " (" +
                             std::to_string (result.coverBodyCount) +
                             " bodies) | Contact gap: " + Fixed (result.contactToleranceM * 1000.0, 1) + " mm");
    pinned.footer.push_back ("Capture " + std::to_string (snapshotId) + " | Result " +
                             std::to_string (highlight.generation) + " | " + timestamp);
    if (pinned.roundingNote)
        pinned.footer.push_back ("Rows are rounded to 0.001 m2; the total is summed before rounding.");
    table = std::move (pinned);
    return true;
}

bool ShotMatches (const ShotTable& table, const HighlightSummary& highlight, uint64_t snapshotId)
{
    return table.generation != 0 && table.generation == highlight.generation && table.snapshotId == snapshotId &&
           table.material == highlight.material && table.materials == highlight.materials &&
           table.contextStyle == highlight.contextStyle && table.contextOpacity == highlight.contextOpacity;
}

float DrawShotTable (const ShotTable& table, float width, float height)
{
    const float font = ImGui::GetFontSize ();
    ImGui::SetNextWindowPos (ImVec2 (0.0f, 0.0f));
    ImGui::SetNextWindowSize (ImVec2 (width, height));
    ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (font, 0.75f * font));
    ImGui::PushStyleVar (ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar (ImGuiStyleVar_CellPadding, ImVec2 (0.5f * font, 0.25f * font));
    ImGui::PushStyleColor (ImGuiCol_WindowBg, ImVec4 (1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::PushStyleColor (ImGuiCol_Text, ImVec4 (0.12f, 0.12f, 0.12f, 1.0f));
    ImGui::PushStyleColor (ImGuiCol_TableRowBg, ImVec4 (1.0f, 1.0f, 1.0f, 0.0f));
    ImGui::PushStyleColor (ImGuiCol_TableRowBgAlt, ImVec4 (0.0f, 0.0f, 0.0f, 0.04f));
    ImGui::PushStyleColor (ImGuiCol_TableBorderLight, ImVec4 (0.0f, 0.0f, 0.0f, 0.15f));
    ImGui::PushStyleColor (ImGuiCol_TableBorderStrong, ImVec4 (0.0f, 0.0f, 0.0f, 0.35f));
    ImGui::PushStyleColor (ImGuiCol_TableHeaderBg, ImVec4 (0.0f, 0.0f, 0.0f, 0.06f));
    ImGui::PushStyleColor (ImGuiCol_Separator, ImVec4 (0.0f, 0.0f, 0.0f, 0.25f));
    ImGui::Begin ("##exposed-surfaces-shot", nullptr,
                  ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar);

    ImGui::PushFont (nullptr, font * 1.2f);
    ImGui::TextUnformatted (table.title.c_str ());
    ImGui::PopFont ();
    ImGui::SameLine ();
    RightAligned ("m\xC2\xB2");
    ImGui::PushTextWrapPos (0.0f);
    ImGui::TextUnformatted (table.subtitle.c_str ());
    ImGui::PopTextWrapPos ();
    ImGui::Spacing ();

    std::string widestArea = "Area (m\xC2\xB2)";
    for (const auto& row : table.rows)
        widestArea = (std::max) (widestArea, ShotArea (row.areaM2),
                                 [] (const std::string& a, const std::string& b) { return a.size () < b.size (); });
    const std::string total = ShotArea (table.totalM2);
    const float areaWidth =
        (std::max) (ImGui::CalcTextSize (widestArea.c_str ()).x, ImGui::CalcTextSize (total.c_str ()).x);
    const std::string unique = std::to_string (table.elements) + " unique";
    const float elementWidth = (std::max) (ImGui::CalcTextSize ("Elements").x, ImGui::CalcTextSize (unique.c_str ()).x);
    if (ImGui::BeginTable ("##exposed-surfaces-rows", 4,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersOuterH |
                               ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn ("##swatch", ImGuiTableColumnFlags_WidthFixed, ImGui::GetTextLineHeight ());
        ImGui::TableSetupColumn ("Surface material", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn ("Elements", ImGuiTableColumnFlags_WidthFixed, elementWidth);
        ImGui::TableSetupColumn ("Area (m\xC2\xB2)", ImGuiTableColumnFlags_WidthFixed, areaWidth);
        ImGui::TableNextRow (ImGuiTableRowFlags_Headers);
        for (int column = 0; column < 4; ++column) {
            ImGui::TableSetColumnIndex (column);
            const char* name = ImGui::TableGetColumnName (column);
            if (column >= 2)
                RightAligned (name);
            else if (column == 1)
                ImGui::TextUnformatted (name);
        }
        for (const auto& row : table.rows) {
            ImGui::TableNextRow ();
            ImGui::TableSetColumnIndex (0);
            Swatch (row.rgba);
            ImGui::TableSetColumnIndex (1);
            // Wrapped, never clipped: a long name makes its row taller.
            ImGui::PushTextWrapPos (0.0f);
            ImGui::Text ("%s [%d]", row.name.c_str (), int (row.surface));
            ImGui::PopTextWrapPos ();
            ImGui::TableSetColumnIndex (2);
            RightAligned (std::to_string (row.elements));
            ImGui::TableSetColumnIndex (3);
            RightAligned (ShotArea (row.areaM2));
        }
        ImGui::TableNextRow ();
        ImGui::TableSetBgColor (ImGuiTableBgTarget_RowBg0, IM_COL32 (0, 0, 0, 20));
        ImGui::TableSetColumnIndex (1);
        ImGui::TextUnformatted (table.material ? "SUBTOTAL" : "TOTAL MEASURED");
        ImGui::TableSetColumnIndex (2);
        RightAligned (unique);
        ImGui::TableSetColumnIndex (3);
        RightAligned (total);
        ImGui::EndTable ();
    }
    ImGui::Spacing ();
    ImGui::PushStyleColor (ImGuiCol_Text, ImVec4 (0.35f, 0.35f, 0.35f, 1.0f));
    ImGui::PushTextWrapPos (0.0f);
    for (const auto& line : table.footer)
        ImGui::TextUnformatted (line.c_str ());
    ImGui::PopTextWrapPos ();
    ImGui::PopStyleColor ();
    // Where the content ends, with the bottom padding: the strip's true height.
    const float used = ImGui::GetCursorPosY () + ImGui::GetStyle ().WindowPadding.y;
    ImGui::End ();
    ImGui::PopStyleColor (8);
    ImGui::PopStyleVar (3);
    return used;
}

} // namespace geomsrv::archviz::surfacetakeoff
