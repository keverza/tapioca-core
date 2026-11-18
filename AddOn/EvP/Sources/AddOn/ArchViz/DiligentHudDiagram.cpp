// ArchViz/DiligentHudDiagram -- see the header.
#include "ArchViz/DiligentHudDiagram.hpp"
#include "ArchViz/DiagramControls.hpp"

#include <imgui.h>

namespace geomsrv::archviz::viewerhud {
bool DiagramPage (float ui)
{
    (void) ui;
    static diagramcontrols::Memory memory; // RENDER THREAD: this HUD's slider ranges
    diagramcontrols::Draw (memory);
    diagramcontrols::DrawSelection (memory);
    diagramcontrols::DrawCapture ();
    ImGui::SeparatorText ("Workflow");
    const bool openWorkflow = ImGui::Button ("Open Massing workflow");
    if (ImGui::IsItemHovered ())
        ImGui::SetTooltip ("The Massing feasibility workflow -- buildings, rules, calculation and this diagram -- in "
                           "this viewer.");
    return openWorkflow;
}
} // namespace geomsrv::archviz::viewerhud
