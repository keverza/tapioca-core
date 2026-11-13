#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/HudVirtualEditorDraft.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include <imgui.h>

// #191 area A, the command bar: pinned over the page. What the editor is doing and what it said,
// the running command's prompt, and the history and exit buttons.
namespace geomsrv::archviz::edithud {
using namespace detail;

namespace lg = tapioca::geometry;

void detail::CommandBar (Session& s)
{
    TerrainActivityBadge (s);
    ImGui::BeginDisabled (s.drag != Drag::None || s.profileTyping);
    if (ImGui::SmallButton ("Undo"))
        History (s, true);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Redo"))
        History (s, false);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Delete"))
        DeleteSelection (s);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Done"))
        RequestClose (s);
    ImGui::EndDisabled ();
    // The landscape command running and what it asks for next (it was under the Booleans).
    if (s.edits.Doc ().landscape) {
        if (s.landscapeCommand) {
            const auto kind = s.landscapeCommand->kind;
            ImGui::TextUnformatted (kind == lg::OperationKind::Union        ? "Union"
                                    : kind == lg::OperationKind::Difference ? "Subtract"
                                                                            : "Intersection");
        }
        ImGui::TextWrapped ("%s", LandscapeCommandPrompt (s));
        if (s.landscapeCommand) {
            ImGui::Text ("%zu shapes picked", s.landscapeCommand->picks.size ());
            if (ImGui::SmallButton ("Cancel (Esc)"))
                CancelLandscapeCommand (s);
        }
    }
    if (!s.message.empty ())
        ImGui::TextWrapped ("%s", s.message.c_str ());
}
} // namespace geomsrv::archviz::edithud
