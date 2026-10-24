#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/HudShell.hpp"
#include "Edit/EditLandscapeEntrance.hpp"
#include "Edit/EditPath.hpp"
#include <cmath>
#include <cstdio>

// Building entrances placed and shown in the view: points on a building outline, each at its
// threshold height, that a path is sent to reach.
namespace geomsrv::archviz::edithud {
namespace lg = tapioca::geometry;

namespace {
// The pointer's pick reach in metres at `mouse`, on the working plane.
double Reach (const detail::Map& m, ImVec2 mouse, double z, float scale)
{
    return 12.0 * scale * m.MetresPerPixel (mouse.x, mouse.y, z, {});
}
} // namespace

bool PlaceEntrance (Session& s, edit::Vec3 point, double reach)
{
    const auto& doc = s.edits.Doc ();
    const auto building = doc.landscape ? lg::BuildingNear (doc, point, reach) : edit::kNoId;
    if (!building) {
        s.message = "Click on a building outline to place an entrance (set a closed line's role to Building).";
        return false;
    }
    const double z = s.plane.origin.z;
    lg::Id made = 0;
    const bool ok = s.edits.Apply ([building, point, z, &made] (edit::Document& d, std::string& error) {
        return lg::AddLandscapeEntrance (d, building, point, z, "", made, error);
    });
    s.message =
        ok ? "Entrance placed at the working plane's height; set its threshold in the panel." : s.edits.Message ();
    return ok;
}

bool ReachSelectedPathEntrance (Session& s)
{
    const auto& doc = s.edits.Doc ();
    const edit::Path* path = nullptr;
    for (const auto& ref : s.selection.Items ())
        if (const auto* p = edit::FindPath (doc, ref.entity))
            path = p;
    if (!path || !doc.landscape || doc.landscape->entrances.empty ()) {
        s.message = "Select a path, with an entrance placed on a building.";
        return false;
    }
    // The entrance nearest either end of the path.
    const lg::LandscapeEntrance* best = nullptr;
    double nearest = INFINITY;
    for (const auto& entrance : doc.landscape->entrances) {
        edit::Vec3 at;
        if (!lg::LandscapeEntrancePoint (doc, entrance, at))
            continue;
        for (const auto* end : { &path->points.front (), &path->points.back () })
            if (edit::DistanceXY (end->p, at) < nearest)
                nearest = edit::DistanceXY (end->p, at), best = &entrance;
    }
    if (!best)
        return false;
    const auto pathId = path->id, entrance = best->id;
    const bool ok = s.edits.Apply ([pathId, entrance] (edit::Document& d, std::string& error) {
        return lg::ReachLandscapeEntrance (d, pathId, entrance, error);
    });
    s.message = ok ? "The path's nearer end now reaches the entrance at its threshold height." : s.edits.Message ();
    return ok;
}

bool detail::EntrancesView (Session& s, const Map& m, ImVec2 mouse, bool hovered, float scale)
{
    const auto& doc = s.edits.Doc ();
    if (!doc.landscape)
        return false;
    for (const auto& entrance : doc.landscape->entrances) {
        edit::Vec3 at;
        if (!lg::LandscapeEntrancePoint (doc, entrance, at))
            continue;
        Dot (s, m, at, 6.0f * scale, kAccent);
        char text[48];
        std::snprintf (text, sizeof text, "Entrance %+.2f m", entrance.z);
        Note (s, m, at, { 8.0f * scale, -8.0f * scale }, text, kInk);
    }
    if (s.landscapeMode != LandscapeMode::Entrance)
        return false;
    edit::Vec3 point;
    if (hovered && m.Onto (mouse.x, mouse.y, s.plane.origin.z, {}, point)) {
        const double reach = Reach (m, mouse, s.plane.origin.z, scale);
        if (lg::BuildingNear (doc, point, reach))
            Dot (s, m, point, 5.0f * scale, kHover, false);
        if (ImGui::IsMouseClicked (0))
            PlaceEntrance (s, point, reach);
    }
    if (m.v.pointer && !m.v.overHud) {
        hudshell::OwnCursor (false);
        ImGui::SetMouseCursor (ImGuiMouseCursor_Arrow);
    }
    return true;
}

void EntrancesPanel (Session& s)
{
    auto& doc = s.edits.Doc ();
    if (!doc.landscape)
        return;
    ImGui::BeginDisabled (GizmoModeLocked (s) || !s.drawing.empty () || s.edits.InGesture ());
    if (ImGui::RadioButton ("Place entrances", s.landscapeMode == LandscapeMode::Entrance))
        SetLandscapeMode (s,
                          s.landscapeMode == LandscapeMode::Entrance ? LandscapeMode::Select : LandscapeMode::Entrance);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Path to entrance"))
        ReachSelectedPathEntrance (s);
    if (ImGui::IsItemHovered ())
        ImGui::SetTooltip ("The selected path's nearer end moves onto the nearest entrance, fixed at its threshold.");
    const auto entrances = doc.landscape->entrances;
    for (const auto& entrance : entrances) {
        ImGui::PushID (static_cast<int> (entrance.id));
        double z = entrance.z;
        ImGui::SetNextItemWidth (90);
        if (ImGui::InputDouble ("##z", &z, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue) && z != entrance.z) {
            const auto id = entrance.id;
            s.edits.Apply ([id, z] (edit::Document& d, std::string& error) {
                for (auto& e : d.landscape->entrances)
                    if (e.id == id) {
                        e.z = z;
                        return std::isfinite (z) || (error = "a threshold height must be finite", false);
                    }
                error = "the entrance no longer exists";
                return false;
            });
            s.message = s.edits.Message ();
        }
        ImGui::SameLine ();
        ImGui::Text ("Entrance %llu threshold (m)%s", static_cast<unsigned long long> (entrance.id),
                     entrance.doorGuid.empty () ? "" : ", from a door");
        ImGui::SameLine ();
        if (ImGui::SmallButton ("Remove")) {
            const auto id = entrance.id;
            s.edits.Apply (
                [id] (edit::Document& d, std::string& error) { return lg::RemoveLandscapeEntrance (d, id, error); });
        }
        ImGui::PopID ();
    }
    ImGui::EndDisabled ();
}
} // namespace geomsrv::archviz::edithud
