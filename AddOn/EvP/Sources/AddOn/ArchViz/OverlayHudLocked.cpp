#include "ArchViz/EditLayers.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include <iomanip>
#include <map>
#include <sstream>

namespace geomsrv::archviz::overlayhud {
namespace {
// The flat selected in each building's editor, by building: equal when nothing was picked.
std::map<std::string, std::string> Picked (const std::map<std::string, hudfloorscheme::EditorPtr>& editors)
{
    std::map<std::string, std::string> picked;
    for (const auto& [key, editor] : editors)
        if (const auto s = hudfloorscheme::Selected (editor)) {
            std::ostringstream out;
            out << std::setprecision (9) << s->story << '@' << s->flat.front ().x << ',' << s->flat.front ().y;
            picked[key] = out.str ();
        }
    return picked;
}
// The slabs of the floor a flat was just picked on: what Archicad selects, so the Selection tab is
// the one a slab picked there opens -- its metadata to edit, its building's plan (user, 2026-10-15:
// "Selecting UNIT in locked mode still only switched tab, but does not open editing options for
// the slab"). Empty when nothing new was picked or the floor's slabs are not known.
// The building whose flat was just picked; empty when none was.
std::string PickedIn (const std::map<std::string, std::string>& before, const std::map<std::string, std::string>& after)
{
    for (const auto& [key, flat] : after)
        if (const auto was = before.find (key); was == before.end () || was->second != flat)
            return key;
    return {};
}
std::vector<std::string> SlabsPicked (const State& state, const std::map<std::string, std::string>& before,
                                      const std::map<std::string, std::string>& after)
{
    for (const auto& [key, flat] : after) {
        const auto was = before.find (key);
        if (was != before.end () && was->second == flat)
            continue;
        const auto s = hudfloorscheme::Selected (state.planEditors.at (key));
        const auto plan = state.floorPlanSnapshots.find (key);
        if (!s || plan == state.floorPlanSnapshots.end ())
            continue;
        for (const auto& floor : plan->second.floors)
            if (floor.story == s->story && !floor.guids.empty ())
                return floor.guids;
    }
    return {};
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
        else if (store->virtualEditor && LayerShown (*store, editlayers::kLayer)) {
            const auto style = roofslope::EditorStyle ();
            const int colours = hudshell::PushLook (style, ui);
            ImGui::PushFont (FontFor (style.font), style.sizePixels * ui);
            edithud::OnView (*store->virtualEditor, input, input.pointer && ImGui::IsWindowHovered (), ui);
            ImGui::PopFont ();
            ImGui::PopStyleColor (colours);
            ImGui::PopStyleVar (hudshell::kLookVars);
        }
        else if (!store->roofEditor && !store->virtualEditor) {
            hudfloorscheme::ViewOnto onto;
            onto.planar = input.planar;
            std::copy (std::begin (input.plan), std::end (input.plan), onto.plan);
            onto.project = input.project;
            // 3D with the cells shown: the solids are what is picked (PickCell).
            if (!input.planar && store->previewCells && input.project && input.hasCameraDirection)
                onto.pick = [&input, this] (float x, float y) { return PickCell (*store, input, x, y); };
            onto.storey = input.planar ? store->planStorey : INT_MIN;
            // The panels' look -- the menu a right click opens, the tips -- not ImGui's dark
            // default (user, 2026-10-15: "Right click menu in overlay locked mode should also be
            // same background as in the imGUI - white").
            const layers::Panel& panel = look ? *look : hudshell::PlainLook ();
            const int colours = hudshell::PushLook (panel, ui);
            ImGui::PushFont (FontFor (panel.font), panel.sizePixels * ui);
            const auto before = Picked (store->planEditors);
            hudfloorscheme::OnView (store->planEditors, store->floorPlanner, store->floorPlanSnapshots,
                                    store->buildingPlans, store->massingProgramme, store->massingCoefficients, onto,
                                    input.pointer && ImGui::IsWindowHovered (), ui);
            // A flat picked on the view opens the Selection tab, as a slab picked in Archicad does
            // (user, 2026-10-15): its building's plan is there, the flat selected in it, and its
            // floor's slabs are selected in Archicad for the tab to edit them.
            // One flat selected at a time; Esc selects none (user, 2026-10-16).
            if (const std::string in = PickedIn (before, Picked (store->planEditors)); !in.empty ())
                hudfloorscheme::Deselect (store->planEditors, in);
            else if (input.pointer && ImGui::IsKeyPressed (ImGuiKey_Escape, false))
                hudfloorscheme::Deselect (store->planEditors);
            if (const auto after = Picked (store->planEditors); !after.empty () && after != before) {
                store->host.known = true;
                store->host.open = true;
                store->host.selected = kSelectionKey;
                std::string slabs;
                for (const auto& guid : SlabsPicked (*store, before, after))
                    slabs += (slabs.empty () ? "" : ",") + guid;
                if (!slabs.empty ())
                    changes.push_back ({ "selectSlabs", {}, "Selection", "picked flat's floor", -1, 1, slabs, true });
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
