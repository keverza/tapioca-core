#ifndef TAPIOCA_ARCHVIZ_VIEWEREDITOR_HPP
#define TAPIOCA_ARCHVIZ_VIEWEREDITOR_HPP

// OVERLAY-INVARIANTS.md §8/§11: main-thread editor and input; owned draw data
// only across the render boundary. The viewer has its own camera, never the census.
#include "ArchViz/OverlayScene.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/SurfaceTakeoffPage.hpp"
#include <array>
#include <memory>

namespace geomsrv {
struct Snapshot;
}
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
struct SurfaceHighlight {
    std::shared_ptr<const Snapshot> capture;
    surfacetakeoff::HighlightSummary summary;
    overlayscene::Scene scene;
};
std::shared_ptr<const SurfaceHighlight> ReadSurfaceHighlight ();
// MAIN THREAD. Start the native workflow dispatcher when the viewer opens. Stop before the child
// HWND/renderer goes away. A surface switch preserves the session, a close does not.
bool Start (void* window);
void Stop (bool preserve = false);
// ANY THREAD. These exchange values, never a Session/ImGui pointer.
bool Active ();
bool Running ();
void RequestWorkflow (const hudshell::DockPlacement& dock, uint32_t fontStep);
bool TakeAnalysisRequest (hudshell::DockPlacement& dock, uint32_t& fontStep);
void RequestSurfaceTakeoff (surfacetakeoff::PageAction action);
std::string SurfaceTakeoffError ();
uint64_t Generation ();
void PublishFrame (Frame frame);
std::unique_ptr<Drawing> TakeDrawing ();
// #124 Focus in view: a model box (metres, margin included) for the viewer camera to frame.
// MAIN THREAD requests; the RENDER THREAD takes the latest one after its navigation, once.
struct FocusBox {
    std::array<float, 3> lo {}, hi {};
};
void RequestFocus (const std::array<float, 3>& lo, const std::array<float, 3>& hi);
bool TakeFocusRequest (FocusBox& box);
void Drawn (uint64_t generation);
} // namespace geomsrv::archviz::vieweredit
#endif
