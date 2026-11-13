#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/HudShellTabs.hpp"
#include <imgui.h>

// #191 area D, the Outliner: under the Inspector. The document's objects, layers and groups, and the
// landscape's compiled terrain recipe and its warnings.
namespace geomsrv::archviz::edithud {
using namespace detail;

void detail::Outliner (Session& s)
{
    const auto& doc = Doc (s);
    ImGui::Text ("%zu objects   %zu paths   %zu selected", doc.entities.size (), doc.paths.size (),
                 s.selection.Items ().size ());
    OrganizeControls (s);
    if (!doc.landscape)
        return;
    ImGui::BeginDisabled (s.landscapeCommand.has_value ());
    if (hudshell::BeginSubTabs ("##outliner")) {
        if (ImGui::BeginTabItem ("Operations")) {
            OperationsTab (s);
            ImGui::EndTabItem ();
        }
        if (ImGui::BeginTabItem ("Warnings")) {
            WarningsTab (s);
            ImGui::EndTabItem ();
        }
        hudshell::EndSubTabs ();
    }
    ImGui::EndDisabled ();
}
} // namespace geomsrv::archviz::edithud
