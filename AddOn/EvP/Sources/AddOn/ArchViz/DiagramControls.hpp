#ifndef EVP_ARCHVIZ_DIAGRAMCONTROLS_HPP
#define EVP_ARCHVIZ_DIAGRAMCONTROLS_HPP
// The exploded diagram's controls (#201), drawn into whichever HUD hosts them: the viewer's
// renderer-owned Massing tab (render thread) and the Massing feasibility workflow's page when
// it is hosted in the viewer (main thread). Every value lives in DiagramModel behind its
// mutex, so either ImGui context may draw them; neither calls ACAPI.
//
// The explode is a distance (user, 2026-11-18): the storey gap in metres on a slider whose
// range follows its value, as the viewpoint radius does, and a building picked in the
// diagram can carry its own gap.

namespace geomsrv::archviz::diagramcontrols {
// Per host, kept between frames: the sliders' adaptive ranges and whether one is held.
struct Memory {
    double gapRange = 0, buildingRange = 0;
    bool gapHeld = false, buildingHeld = false;
};
// Diagram mode, gap, detail, colouring, hide source, frame, per-building gap, status.
void Draw (Memory& memory);
// The selected storey's source fields; nothing while no storey is selected.
void DrawSelection (Memory& memory);
// Save the diagram's input as a replayable capture (DiagramReplay; RenderBench --diagram).
void DrawCapture ();
} // namespace geomsrv::archviz::diagramcontrols
#endif
