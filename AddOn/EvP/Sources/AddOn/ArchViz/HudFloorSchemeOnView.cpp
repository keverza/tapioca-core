#include "ArchViz/HudFloorSchemeEditDetail.hpp"
#include "ArchViz/HudShell.hpp"
#include <algorithm>
#include <optional>

// The floor plans edited on the view itself while the dock's lock is on (HudFloorSchemeEdit.hpp
// `OnView`): in the plan, each building's shown floor through the plan's own transform, with the
// Plan view's gestures; in 3D, a flat picked through the camera is selected and its menu opened.
namespace geomsrv::archviz::hudfloorscheme {
using namespace detail;
namespace {
struct Shown {
    std::string key;
    const bp::Plan* plan = nullptr;
    const bp::Floor* floor = nullptr;
};
// Each building's floor as its Plan view shows it.
std::vector<Shown> ShownFloors (const std::map<std::string, bp::Plan>& plans, std::map<std::string, bp::Draft>& drafts)
{
    std::vector<Shown> out;
    for (const auto& [key, plan] : plans) {
        auto& draft = drafts[key];
        bp::Sync (plan, draft);
        if (const auto* floor = bp::Displayed (plan, draft))
            out.push_back ({ key, &plan, floor });
    }
    return out;
}
// Even-odd: a projected flat is a simple ring on screen.
bool Within (const std::vector<ImVec2>& ring, ImVec2 p)
{
    bool inside = false;
    for (size_t i = 0, j = ring.size () - 1; i < ring.size (); j = i++)
        if ((ring[i].y > p.y) != (ring[j].y > p.y) &&
            p.x < ring[j].x + (ring[i].x - ring[j].x) * (p.y - ring[j].y) / (ring[i].y - ring[j].y))
            inside = !inside;
    return inside;
}
} // namespace

void OnView (std::map<std::string, EditorPtr>& editors, bp::Planner& planner,
             const std::map<std::string, bp::Plan>& plans, std::map<std::string, bp::Draft>& drafts,
             const floorprogramme::Programme& programme, const massingareas::Coefficients& coefficients,
             const ViewOnto& onto, bool hovered, float scale)
{
    fs::Options options;
    options.grossFactor = coefficients.grossFactor;
    Context c { planner, plans, drafts, programme, options };
    auto* draw = ImGui::GetWindowDrawList ();
    const ImVec2 mouse = ImGui::GetIO ().MousePos;
    const auto shown = ShownFloors (plans, drafts);
    const bool focused = ImGui::IsWindowFocused ();
    if (onto.planar) {
        View v;
        std::copy (std::begin (onto.plan), std::end (onto.plan), v.m);
        if (std::abs (v.m[0] * v.m[4] - v.m[1] * v.m[3]) < 1e-18)
            return;
        // The editor held -- in a gesture, or its menu open -- else the building under the pointer.
        std::string active;
        for (const auto& [key, editor] : editors)
            if (editor && (editor->drag != Drag::None || editor->menuOpen))
                active = key;
        if (active.empty () && hovered)
            for (const auto& s : shown)
                if (Inside (bp::FloorRings (*s.floor), v.W (mouse)))
                    active = s.key;
        for (const auto& s : shown) {
            auto& editor = editors[s.key];
            const bool mine = s.key == active;
            if (!mine && !editor)
                continue;
            ImGui::PushID (s.key.c_str ());
            auto& e = Refresh (editor, c, s.key, *s.floor);
            const Pointing at = mine ? Interact (e, c, v, hovered) : Pointing {};
            Marks (e, c, v, draw, scale, at);
            if (mine && focused && ImGui::IsKeyChordPressed (ImGuiMod_Ctrl | ImGuiKey_Z))
                UndoRedo (e, c, true);
            if (mine && focused && ImGui::IsKeyChordPressed (ImGuiMod_Ctrl | ImGuiKey_Y))
                UndoRedo (e, c, false);
            Asked asked;
            Menu (e, c, c.drafts[s.key], *s.plan, *s.floor, asked);
            e.menuOpen = ImGui::IsPopupOpen ("##planMenu");
            ImGui::PopID ();
        }
        if (hovered && active.empty ()) {
            hudshell::OwnCursor ();
            ImGui::SetMouseCursor (ImGuiMouseCursor_Arrow);
        }
        return;
    }
    if (!onto.project)
        return;
    // 3D: every planned floor's flats through the camera; the highest under the pointer is picked.
    struct Pick {
        std::string key;
        const bp::Floor* floor = nullptr;
        fs::Vec at;
        std::vector<ImVec2> screen;
    };
    std::optional<Pick> pick;
    if (hovered)
        for (const auto& [key, plan] : plans)
            for (const auto& floor : plan.floors) {
                const auto* planned = planner.Latest (bp::FloorId (key, floor.story));
                if (!planned || (pick && floor.z <= pick->floor->z))
                    continue;
                for (const auto& f : planned->scheme.flats) {
                    std::vector<ImVec2> screen;
                    for (const auto& p : f.shape) {
                        float x = 0, y = 0;
                        if (!onto.project (p.x, p.y, floor.z + 0.05, x, y))
                            break;
                        screen.push_back ({ x, y });
                    }
                    if (screen.size () == f.shape.size () && screen.size () >= 3 && Within (screen, mouse))
                        pick = Pick { key, &floor, Mid (f.shape), std::move (screen) };
                }
            }
    if (hovered) {
        hudshell::OwnCursor ();
        ImGui::SetMouseCursor (pick ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_Arrow);
    }
    if (pick) {
        draw->AddPolyline (pick->screen.data (), int (pick->screen.size ()), IM_COL32 (255, 255, 255, 200),
                           ImDrawFlags_Closed, 2.5f * scale);
        const bool left = ImGui::IsMouseClicked (ImGuiMouseButton_Left),
                   right = ImGui::IsMouseClicked (ImGuiMouseButton_Right);
        if (left || right) {
            // Its building's Plan view turns to its floor, the flat selected there and here.
            c.drafts[pick->key].story = pick->floor->story;
            auto& e = Refresh (editors[pick->key], c, pick->key, *pick->floor);
            e.selected = Again (e.scheme, { Kind::Flat, 0, {}, {} }, pick->at);
            e.selectedAt = pick->at;
            if (right) {
                hudshell::ClaimRightClick ();
                e.menu = e.selected;
                e.menuAt = pick->at;
                ImGui::PushID (pick->key.c_str ());
                ImGui::OpenPopup ("##planMenu");
                ImGui::PopID ();
                e.menuOpen = true;
            }
        }
    }
    // A menu opened here stays open while the pointer moves.
    for (const auto& s : shown) {
        const auto editor = editors.find (s.key);
        if (editor == editors.end () || !editor->second || !editor->second->menuOpen)
            continue;
        ImGui::PushID (s.key.c_str ());
        auto& e = Refresh (editor->second, c, s.key, *s.floor);
        Asked asked;
        Menu (e, c, c.drafts[s.key], *s.plan, *s.floor, asked);
        e.menuOpen = ImGui::IsPopupOpen ("##planMenu");
        ImGui::PopID ();
    }
}
} // namespace geomsrv::archviz::hudfloorscheme
