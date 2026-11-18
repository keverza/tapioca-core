// ArchViz/DiagramControls -- see the header.
#include "ArchViz/DiagramControls.hpp"
#include "ArchViz/DiagramModel.hpp"
#include "ArchViz/DiligentHud.hpp" // viewerhud::AdaptiveSlider
#include "ArchViz/ViewerEditor.hpp"

#include <imgui.h>

#include <array>
#include <cstdio>
#include <string>

namespace geomsrv::archviz::diagramcontrols {
namespace {
const ImVec4 kMuted (0.62f, 0.66f, 0.72f, 1.0f);

void Wrapped (const std::string& text, const ImVec4* colour = nullptr)
{
    if (colour)
        ImGui::PushStyleColor (ImGuiCol_Text, *colour);
    ImGui::TextWrapped ("%s", text.c_str ());
    if (colour)
        ImGui::PopStyleColor ();
}

void Tip (const char* text)
{
    if (ImGui::IsItemHovered ())
        ImGui::SetTooltip ("%s", text);
}

// "building:A" -> "building A"; a slab without an authored building ID -> "unnamed slab".
std::string BuildingName (const std::string& key)
{
    constexpr char kPrefix[] = "building:";
    if (key.rfind (kPrefix, 0) == 0)
        return "building " + key.substr (sizeof (kPrefix) - 1);
    return "unnamed slab";
}

// Assembled and exploded bounds together, at the spacing asked for, so the camera never
// loses a storey mid-animation.
void Frame (const diagram::Publication& publication, const diagram::Spacing& spacing)
{
    diagram::Vec3 lo {}, hi {};
    diagram::FramingBounds (diagram::Spaced (publication.plan, spacing), lo, hi);
    std::array<float, 3> a {}, b {};
    for (int k = 0; k < 3; ++k) {
        const double margin = 0.08 * (hi[k] - lo[k]) + 1.0;
        a[size_t (k)] = float (lo[k] - margin);
        b[size_t (k)] = float (hi[k] + margin);
    }
    vieweredit::RequestFocus (a, b);
}

// The building of the selected storey, by key; empty while none is selected.
std::string SelectedBuilding (const diagram::Publication& publication)
{
    const auto selected = diagrammodel::Selected ();
    const auto info = selected.Empty () ? publication.storeyInfo.end () : publication.storeyInfo.find (selected);
    return info == publication.storeyInfo.end () ? std::string () : info->second.buildingKey;
}
} // namespace

void Draw (Memory& memory)
{
    auto controls = diagrammodel::ReadControls ();
    const auto before = controls;
    const auto status = diagrammodel::ReadStatus ();
    const auto publication = diagrammodel::Read ();

    ImGui::SeparatorText ("Diagram");
    if (ImGui::Checkbox ("Diagram mode", &controls.enabled) && controls.enabled && publication)
        Frame (*publication, controls.spacing);
    Tip ("An exploded, read-only view of the published massing: one storey per source floor.");
    ImGui::BeginDisabled (!controls.enabled);

    viewerhud::AdaptiveSlider ("Storey gap", controls.spacing.storeyGap, memory.gapRange, memory.gapHeld, 0.0,
                               diagram::kMaxStoreyGap, "%.1f m");
    Tip ("The distance between consecutive storeys. The range rescales on release; ctrl+click to type a value.");

    int detail = int (controls.detail);
    const char* details[] = { "Topology", "Units", "Rooms", "Walls" };
    if (ImGui::BeginCombo ("Detail", details[detail])) {
        for (int i = 0; i < 4; ++i) {
            const auto level = diagram::Detail (i);
            const bool available = diagram::DetailAvailable (level);
            const std::string label =
                std::string (details[i]) + (available ? "" : "  (needs floor plans)") + "##detail" + std::to_string (i);
            if (ImGui::Selectable (label.c_str (), detail == i, available ? 0 : ImGuiSelectableFlags_Disabled))
                detail = i;
        }
        ImGui::EndCombo ();
    }
    controls.detail = diagram::Detail (detail);
    Tip ("Topology: one shell per storey. Units, rooms and walls come with the floor plans in the diagram.");

    int colouring = int (controls.colouring);
    ImGui::TextUnformatted ("Colour");
    ImGui::SameLine ();
    ImGui::RadioButton ("Function", &colouring, int (diagram::Colouring::Function));
    ImGui::SameLine ();
    ImGui::RadioButton ("White", &colouring, int (diagram::Colouring::White));
    controls.colouring = diagram::Colouring (colouring);

    ImGui::Checkbox ("Outlines", &controls.outlines);
    Tip ("Draws the storeys' edges, so white storeys stay readable.");
    ImGui::SameLine ();
    ImGui::Checkbox ("Corner lines", &controls.connectors);
    Tip ("Dashed lines from each storey's corners down to the storey below, while exploded.");

    ImGui::Checkbox ("Hide source slabs", &controls.hideSource);
    Tip ("Hides only the massing slabs the diagram stands in for; terrain and neighbours stay.");
    ImGui::BeginDisabled (!publication);
    if (ImGui::Button ("Frame diagram") && publication)
        Frame (*publication, controls.spacing);
    ImGui::EndDisabled ();

    // ---- per-building gap: pick a storey, override its building only ----
    const std::string building = publication ? SelectedBuilding (*publication) : std::string ();
    auto& overrides = controls.spacing.buildingGap;
    if (!building.empty ()) {
        bool own = overrides.count (building) > 0;
        if (ImGui::Checkbox (("Own gap for " + BuildingName (building)).c_str (), &own)) {
            if (own)
                overrides[building] = controls.spacing.storeyGap;
            else
                overrides.erase (building);
        }
        Tip ("Explode this building by its own distance; the others keep the shared storey gap.");
        if (own)
            viewerhud::AdaptiveSlider ("Building gap", overrides[building], memory.buildingRange, memory.buildingHeld,
                                       0.0, diagram::kMaxStoreyGap, "%.1f m");
    }
    else if (controls.enabled)
        Wrapped ("Click a storey to give its building its own gap.", &kMuted);
    if (!overrides.empty () && ImGui::TreeNode ("BuildingGaps", "Building gaps (%zu)", overrides.size ())) {
        std::string remove;
        for (const auto& [key, gap] : overrides) {
            char line[160];
            std::snprintf (line, sizeof (line), "%s: %.1f m", BuildingName (key).c_str (), gap);
            ImGui::TextUnformatted (line);
            ImGui::SameLine ();
            if (ImGui::SmallButton (("Reset##" + key).c_str ()))
                remove = key;
        }
        if (!remove.empty ())
            overrides.erase (remove);
        if (ImGui::SmallButton ("Reset all"))
            overrides.clear ();
        ImGui::TreePop ();
    }
    ImGui::EndDisabled ();
    if (controls != before)
        diagrammodel::SetControls (controls);

    if (!controls.enabled)
        return;
    Wrapped (status.building         ? "Building the diagram..."
             : status.state.empty () ? std::string ("Waiting for the massing.")
                                     : status.state);
    char line[200];
    std::snprintf (line, sizeof (line), "gap %.2f m  |  %zu draws  |  %llu upload(s), %.1f KB", status.shownGap,
                   status.draws, static_cast<unsigned long long> (status.uploads),
                   double (status.uploadedBytes) / 1024.0);
    Wrapped (line, &kMuted);
    if (publication) {
        std::snprintf (
            line, sizeof (line), "built in %.1f ms  |  %llu published, %llu superseded, %llu refused, %llu unchanged",
            publication->buildMs, static_cast<unsigned long long> (status.publications),
            static_cast<unsigned long long> (status.discarded), static_cast<unsigned long long> (status.refused),
            static_cast<unsigned long long> (status.unchanged));
        Wrapped (line, &kMuted);
    }
    if (!status.notes.empty () && ImGui::TreeNode ("DiagramNotes", "Notes (%zu)", status.notes.size ())) {
        for (size_t i = 0; i < status.notes.size () && i < 12; ++i)
            Wrapped (status.notes[i], &kMuted);
        ImGui::TreePop ();
    }
}

void DrawSelection (Memory& memory)
{
    (void) memory;
    const auto controls = diagrammodel::ReadControls ();
    const auto publication = diagrammodel::Read ();
    if (!controls.enabled || !publication)
        return;
    ImGui::SeparatorText ("Selected storey");
    const auto selected = diagrammodel::Selected ();
    const auto info = selected.Empty () ? publication->storeyInfo.end () : publication->storeyInfo.find (selected);
    if (info == publication->storeyInfo.end ()) {
        Wrapped ("Click a storey in the diagram to inspect it.");
        return;
    }
    const auto& s = info->second;
    char line[200];
    const std::string building = s.buildingId.empty () ? "unnamed slab" : "building " + s.buildingId;
    const std::string story = s.storyName.empty () ? "story " + std::to_string (s.sourceStory) : s.storyName;
    Wrapped (building + ", " + story);
    std::snprintf (line, sizeof (line), "z %.2f to %.2f m (%.2f m)", s.z, s.top, s.top - s.z);
    Wrapped (line);
    if (s.rawArea > 0 || s.allowedArea > 0) {
        std::snprintf (line, sizeof (line), "Area %.1f m2, allowed %.1f m2", s.rawArea, s.allowedArea);
        Wrapped (line);
    }
    std::string functions;
    for (const auto& f : s.functions)
        functions += (functions.empty () ? "" : ", ") + f;
    if (!functions.empty ())
        Wrapped ("Function: " + functions);
    Wrapped (s.planned ? "Planned" : "Not planned: a storey shell, no apartments yet");
    std::snprintf (line, sizeof (line), "%zu source slab(s); read-only", s.slabs.size ());
    Wrapped (line);
    if (ImGui::Button ("Clear selection"))
        diagrammodel::Select ({});
}

void DrawCapture ()
{
    ImGui::SeparatorText ("Capture");
    if (ImGui::Button ("Save capture (JSON)"))
        diagrammodel::RequestSave ();
    Tip ("Writes the diagram's input to %LOCALAPPDATA%/Tapioca/dumps; RenderBench --diagram replays it offline.");
    const auto status = diagrammodel::ReadStatus ();
    if (!status.saveError.empty ()) {
        const ImVec4 red (0.95f, 0.45f, 0.40f, 1.0f);
        Wrapped (status.saveError, &red);
    }
    else if (!status.savedPath.empty ())
        Wrapped ("Saved " + status.savedPath);
}
} // namespace geomsrv::archviz::diagramcontrols
