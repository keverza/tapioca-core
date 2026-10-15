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
        if (store->roofEditor && LayerShown (*store, roofslope::kLayer)) {
            const auto style = roofslope::EditorStyle ();
            const int colours = hudshell::PushLook (style, ui);
            ImGui::PushFont (FontFor (style.font), style.sizePixels * ui);
            roofhud::OnView (*store->roofEditor, input, input.pointer && ImGui::IsWindowHovered (), ui);
            ImGui::PopFont ();
            ImGui::PopStyleColor (colours);
            ImGui::PopStyleVar (hudshell::kLookVars);
        }
        else if (!store->roofEditor) {
            hudfloorscheme::ViewOnto onto;
            onto.planar = input.planar;
            std::copy (std::begin (input.plan), std::end (input.plan), onto.plan);
            onto.project = input.project;
            hudfloorscheme::OnView (store->planEditors, store->floorPlanner, store->floorPlanSnapshots,
                                    store->buildingPlans, store->massingProgramme, store->massingCoefficients, onto,
                                    input.pointer && ImGui::IsWindowHovered (), ui);
        }
    }
    // Behind every panel, whenever it was made.
    ImGui::BringWindowToDisplayBack (ImGui::GetCurrentWindow ());
    ImGui::End ();
    // The blocks to add, one above the other under the dock, to drag onto the plan (user,
    // 2026-10-14: "If overlay is locked below the mini panel show vertical column of the unit block
    // to add").
    // The column is the dock's tab's width, flush under it, and has no panel behind its blocks
    // (user, 2026-10-15: "thinner width same as the blue tab. and no black background").
    if (!store->roofEditor && input.planar && dockMax.y > dockMin.y && dockMax.x > dockMin.x) {
        const float width = dockMax.x - dockMin.x;
        ImGui::SetNextWindowPos (ImVec2 (dockMin.x, dockMax.y + kDockGap * ui));
        const ImGuiWindowFlags column = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoNav;
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (0.0f, 0.0f));
        ImGui::PushStyleVar (ImGuiStyleVar_WindowBorderSize, 0.0f);
        const bool open = ImGui::Begin ("###tapioca.lockedPalette", nullptr, column);
        ImGui::PopStyleVar (2); // the window's own; its tooltips keep the look's padding
        if (open) {
            ImGui::PushStyleVar (ImGuiStyleVar_ItemSpacing, ImVec2 (0.0f, 0.0f));
            hudfloorscheme::LockedPalette (store->massingProgramme, ui, width);
            ImGui::PopStyleVar ();
        }
        ImGui::End ();
    }
}
} // namespace geomsrv::archviz::overlayhud
