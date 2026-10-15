#include "ArchViz/OverlayHudEngine.hpp"
#include <iomanip>
#include <sstream>

namespace geomsrv::archviz::overlayhud {
namespace {
// The flats selected in the buildings' editors, as one string: equal when nothing was picked.
std::string Picked (const std::map<std::string, hudfloorscheme::EditorPtr>& editors)
{
    std::ostringstream out;
    out << std::setprecision (9);
    for (const auto& [key, editor] : editors)
        if (const auto s = hudfloorscheme::Selected (editor))
            out << s->building << '#' << s->story << '@' << s->flat.front ().x << ',' << s->flat.front ().y << ';';
    return out.str ();
}
} // namespace

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
            // The panels' look -- the menu a right click opens, the tips -- not ImGui's dark
            // default (user, 2026-10-15: "Right click menu in overlay locked mode should also be
            // same background as in the imGUI - white").
            const layers::Panel& panel = look ? *look : hudshell::PlainLook ();
            const int colours = hudshell::PushLook (panel, ui);
            ImGui::PushFont (FontFor (panel.font), panel.sizePixels * ui);
            const std::string before = Picked (store->planEditors);
            hudfloorscheme::OnView (store->planEditors, store->floorPlanner, store->floorPlanSnapshots,
                                    store->buildingPlans, store->massingProgramme, store->massingCoefficients, onto,
                                    input.pointer && ImGui::IsWindowHovered (), ui);
            // A flat picked on the view opens the Selection tab, as a slab picked in Archicad does
            // (user, 2026-10-15): its building's plan is there, the flat selected in it.
            if (const std::string after = Picked (store->planEditors); !after.empty () && after != before) {
                store->host.known = true;
                store->host.open = true;
                store->host.selected = kSelectionKey;
            }
            ImGui::PopFont ();
            ImGui::PopStyleColor (colours);
            ImGui::PopStyleVar (hudshell::kLookVars);
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
