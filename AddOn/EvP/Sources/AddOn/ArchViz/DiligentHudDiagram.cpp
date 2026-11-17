// ArchViz/DiligentHudDiagram -- see the header.
#include "ArchViz/DiligentHudDiagram.hpp"
#include "ArchViz/DiagramModel.hpp"
#include "ArchViz/ViewerEditor.hpp"

#include <imgui.h>

#include <cmath>
#include <cstdio>

namespace geomsrv::archviz::viewerhud {
namespace {
void Wrapped (const std::string& text, const ImVec4* colour = nullptr)
{
    if (colour)
        ImGui::PushStyleColor (ImGuiCol_Text, *colour);
    ImGui::TextWrapped ("%s", text.c_str ());
    if (colour)
        ImGui::PopStyleColor ();
}

// Assembled and exploded bounds together, so the camera never loses a storey mid-animation.
void FrameDiagram (const diagram::Publication& publication)
{
    diagram::Vec3 lo {}, hi {};
    diagram::FramingBounds (publication.plan, lo, hi);
    std::array<float, 3> a {}, b {};
    for (int k = 0; k < 3; ++k) {
        const double margin = 0.08 * (hi[k] - lo[k]) + 1.0;
        a[size_t (k)] = float (lo[k] - margin);
        b[size_t (k)] = float (hi[k] + margin);
    }
    vieweredit::RequestFocus (a, b);
}
} // namespace

bool DiagramPage (float ui)
{
    (void) ui;
    bool openWorkflow = false;
    auto controls = diagrammodel::ReadControls ();
    const auto before = controls;
    const auto status = diagrammodel::ReadStatus ();
    const auto publication = diagrammodel::Read ();

    ImGui::SeparatorText ("Diagram");
    if (ImGui::Checkbox ("Diagram mode", &controls.enabled) && controls.enabled && publication)
        FrameDiagram (*publication);
    if (ImGui::IsItemHovered ())
        ImGui::SetTooltip ("An exploded, read-only view of the published massing: one storey per source floor.");
    ImGui::BeginDisabled (!controls.enabled);
    float percent = float (controls.explode * 100.0);
    if (ImGui::SliderFloat ("Explode", &percent, 0.0f, 100.0f, "%.0f %%"))
        controls.explode = double (percent) / 100.0;
    ImGui::Checkbox ("Hide source slabs", &controls.hideSource);
    if (ImGui::IsItemHovered ())
        ImGui::SetTooltip ("Hides only the massing slabs the diagram stands in for; terrain and neighbours stay.");
    ImGui::BeginDisabled (!publication);
    if (ImGui::Button ("Frame diagram") && publication)
        FrameDiagram (*publication);
    ImGui::EndDisabled ();
    ImGui::EndDisabled ();
    if (controls != before)
        diagrammodel::SetControls (controls);

    if (controls.enabled) {
        const ImVec4 muted (0.62f, 0.66f, 0.72f, 1.0f);
        Wrapped (status.building         ? "Building the diagram..."
                 : status.state.empty () ? std::string ("Waiting for the massing.")
                                         : status.state);
        char line[160];
        std::snprintf (line, sizeof (line), "t %.2f  |  %zu draws  |  %llu upload(s), %.1f KB", status.shownT,
                       status.draws, static_cast<unsigned long long> (status.uploads),
                       double (status.uploadedBytes) / 1024.0);
        Wrapped (line, &muted);
        if (publication) {
            std::snprintf (line, sizeof (line), "built in %.1f ms  |  %llu published, %llu superseded, %llu refused",
                           publication->buildMs, static_cast<unsigned long long> (status.publications),
                           static_cast<unsigned long long> (status.discarded),
                           static_cast<unsigned long long> (status.refused));
            Wrapped (line, &muted);
        }
        if (!status.notes.empty () && ImGui::TreeNode ("Notes", "Notes (%zu)", status.notes.size ())) {
            for (size_t i = 0; i < status.notes.size () && i < 12; ++i)
                Wrapped (status.notes[i], &muted);
            ImGui::TreePop ();
        }
    }

    if (controls.enabled && publication) {
        ImGui::SeparatorText ("Selected storey");
        const auto selected = diagrammodel::Selected ();
        const auto info = selected.Empty () ? publication->storeyInfo.end () : publication->storeyInfo.find (selected);
        if (info == publication->storeyInfo.end ())
            Wrapped ("Click a storey in the diagram to inspect it.");
        else {
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
    }

    ImGui::SeparatorText ("Capture");
    if (ImGui::Button ("Save capture (JSON)"))
        diagrammodel::RequestSave ();
    if (ImGui::IsItemHovered ())
        ImGui::SetTooltip ("Writes the diagram's input to %%LOCALAPPDATA%%\\Tapioca\\dumps, replayable offline.");
    if (!status.saveError.empty ()) {
        const ImVec4 red (0.95f, 0.45f, 0.40f, 1.0f);
        Wrapped (status.saveError, &red);
    }
    else if (!status.savedPath.empty ())
        Wrapped ("Saved " + status.savedPath);

    ImGui::SeparatorText ("Workflow");
    if (ImGui::Button ("Open Massing workflow"))
        openWorkflow = true;
    if (ImGui::IsItemHovered ())
        ImGui::SetTooltip ("The full Massing page -- buildings, rules, calculation -- in this viewer.");
    return openWorkflow;
}
} // namespace geomsrv::archviz::viewerhud
