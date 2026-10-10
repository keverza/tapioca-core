#include "ArchViz/OverlayHudEngine.hpp"

namespace geomsrv::archviz::overlayhud {
bool EditLocked (const State& state)
{
    return state.editLocked;
}

void SetEditLocked (State& state, bool locked)
{
    state.editLocked = locked;
}

// ⚠️ LOCKED, THE VIEW IS THE HUD'S (the user, 2026-10-10): a borderless window the size of the
// view, kept behind the panels, takes the clicks the input layer now gives the HUD everywhere,
// and the floor plan is edited there -- the plan through its transform, 3D through the camera.
void Engine::Impl::LockedView (const Input& input, float ui, ImVec2 view)
{
    ImGui::SetNextWindowPos (ImVec2 (0.0f, 0.0f));
    ImGui::SetNextWindowSize (view);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::Begin ("###tapioca.lockedView", nullptr, flags)) {
        hudfloorscheme::ViewOnto onto;
        onto.planar = input.planar;
        std::copy (std::begin (input.plan), std::end (input.plan), onto.plan);
        onto.project = input.project;
        hudfloorscheme::OnView (store->planEditors, store->floorPlanner, store->floorPlanSnapshots,
                                store->buildingPlans, store->massingProgramme, store->massingCoefficients, onto,
                                input.pointer && ImGui::IsWindowHovered (), ui);
    }
    // Behind every panel, whenever it was made.
    ImGui::BringWindowToDisplayBack (ImGui::GetCurrentWindow ());
    ImGui::End ();
}
} // namespace geomsrv::archviz::overlayhud
