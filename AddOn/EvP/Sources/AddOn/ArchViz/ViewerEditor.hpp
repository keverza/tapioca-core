#ifndef TAPIOCA_ARCHVIZ_VIEWEREDITOR_HPP
#define TAPIOCA_ARCHVIZ_VIEWEREDITOR_HPP

// OVERLAY-INVARIANTS.md §8/§11: main-thread editor and input; owned draw data
// only across the render boundary. The viewer has its own camera, never the census.
#include "ArchViz/OverlayScene.hpp"
#include "ArchViz/HudShell.hpp"
#include <array>
#include <memory>

namespace geomsrv::archviz::vieweredit {
struct Frame {
    std::array<float, 16> view {}, projection {};
    uint32_t width = 0, height = 0;
    bool Project (double x, double y, double z, float& px, float& py) const;
};
struct Drawing {
    overlayscene::Scene scene, hud;
    bool sceneChanged = false;
    float scale = 1.0f;
};
// MAIN THREAD. Start the native workflow dispatcher when the viewer opens. Stop before the child
// HWND/renderer goes away. A surface switch preserves the session, a close does not.
bool Start (void* window);
void Stop (bool preserve = false);
// ANY THREAD. These exchange values, never a Session/ImGui pointer.
bool Active ();
bool Running ();
void RequestWorkflow (const hudshell::DockPlacement& dock, uint32_t fontStep);
bool TakeAnalysisRequest (hudshell::DockPlacement& dock, uint32_t& fontStep);
uint64_t Generation ();
void PublishFrame (Frame frame);
std::unique_ptr<Drawing> TakeDrawing ();
void Drawn (uint64_t generation);
} // namespace geomsrv::archviz::vieweredit
#endif
