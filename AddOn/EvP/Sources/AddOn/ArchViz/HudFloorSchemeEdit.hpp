#ifndef EVP_ARCHVIZ_HUDFLOORSCHEMEEDIT_HPP
#define EVP_ARCHVIZ_HUDFLOORSCHEMEEDIT_HPP

// The Plan view: one building's floor as a typology scheme on a canvas the panel's width, edited
// like a limited drawing program that follows the skeleton (user, 2026-10-10). Only the building
// is shown; the other buildings at the elevation are its context, the walls against them blind
// and thick. Click selects a flat, stair, party wall, corridor end or the wall against a
// neighbour, and arrows on it drag it in 0.4 m steps (walls snap to programme sizes); a flat
// dragged onto another changes places with it, dragged off the floor it goes, as a stair does.
// Squares above the plan -- flats of 1 to 5 rooms, a stair -- drag onto it, a red wall showing
// where a flat would go. A right click opens what can be done there: a flat's rooms, split,
// join, lock; a stair's size; the floor's flat count, access, unique or shared design, reset.
// The slab's own contour is dashed under the plan. (Draw and Cut, which added rectangles to the
// silhouette or took them out, are gone: user, 2026-10-14, "too messy". Designs that hold such
// rectangles still plan with them.)
//
// Every edit is a design (FloorSchemeEdit.hpp) the planner regenerates off the UI thread
// (FloorPlanner.hpp), the newest first, so the canvas never waits; undo and redo swap designs,
// across buildings when a party wall moved. Save writes every changed building's stairs and floor
// designs to its slabs' metadata (one undo step each).
//
// MAIN THREAD, inside ImGui's lock like the rest of the HUD.
#include "ArchViz/FloorPlanner.hpp"
#include <functional>
#include <memory>
#include <optional>

namespace geomsrv::archviz::hudfloorscheme {
struct Editor;
struct EditorDeleter {
    void operator() (Editor* editor) const;
};
using EditorPtr = std::unique_ptr<Editor, EditorDeleter>;

// What the Plan view asks of its owner this frame.
struct Asked {
    std::vector<hudmeta::Edit> edits; // Save: every changed building's stairs and floor designs
    bool exportPlan = false;          // Export plan
};
// The Plan view of building `key`'s shown floor (`drafts[key].story`, else its first). `editor`
// is the building's own; `plans` and `drafts` are every building's.
Asked PlanView (EditorPtr& editor, buildingplan::Planner& planner,
                const std::map<std::string, buildingplan::Plan>& plans,
                std::map<std::string, buildingplan::Draft>& drafts, const std::string& key,
                const floorprogramme::Programme& programme, const massingareas::Coefficients& coefficients,
                float scale);

// Model metres onto a view of the overlay: the plan's transform, the same at every elevation
// (`planar`: pixel = plan[0] x + plan[1] y + plan[2], plan[3] x + plan[4] y + plan[5]), or 3D's
// camera (`project`, false where a point does not project).
struct ViewOnto {
    bool planar = false;
    double plan[6] = {};
    // On the plan, Archicad's storey: only each building's floor there is shown and edited (user,
    // 2026-10-15: "do not allow display or editing of other floor in locked overlay editor"); a
    // building with no floor there has none. INT_MIN: the floor its Plan view shows.
    int storey = INT_MIN;
    std::function<bool (double x, double y, double z, float& px, float& py)> project;
};
// ⚠️ THE FLOOR PLAN EDITED ON THE VIEW ITSELF while the dock's lock is on (the user, 2026-10-10),
// in the HUD's window over the whole view (`hovered`: the pointer is there, not on a panel). In
// the plan each building's shown floor takes the Plan view's gestures -- select, arrows, drag,
// right click, a block dropped from LockedPalette -- through the plan's transform; in 3D a flat clicked is selected
// (its building's Plan view turns to its floor) and a right click opens its menu.
void OnView (std::map<std::string, EditorPtr>& editors, buildingplan::Planner& planner,
             const std::map<std::string, buildingplan::Plan>& plans, std::map<std::string, buildingplan::Draft>& drafts,
             const floorprogramme::Programme& programme, const massingareas::Coefficients& coefficients,
             const ViewOnto& onto, bool hovered, float scale);
// The flats of 1 to 5 rooms and a stair, one above the other, to drag onto the locked plan view (user,
// 2026-10-14: "If overlay is locked below the mini panel show vertical column of the unit block to
// add"), in the current window, each block `width` across (the dock's tab, user 2026-10-15: "thinner
// width same as the blue tab"; 0: the Plan view's size). OnView takes the drop as the canvas does.
void LockedPalette (const floorprogramme::Programme& programme, float scale, float width = 0);

// True while a Plan view's flats are still flowing to a scheme just planned: the owner lays the HUD
// out again until they settle.
bool Animating (const std::map<std::string, EditorPtr>& editors);

// The flat selected in a Plan view, for the overlay to draw it highlighted.
struct Selection {
    std::string building;
    int story = 0;
    floorscheme::Ring flat;
};
std::optional<Selection> Selected (const EditorPtr& editor);
// ⚠️ ONE FLAT SELECTED AT A TIME (user, 2026-10-16: "For each building i am able to make a selection
// and it persists, creating misleading preview"): every editor's selection but `except`'s cleared --
// all of them when it is empty. Nothing held (a drag, an open menu) is touched.
void Deselect (std::map<std::string, EditorPtr>& editors, const std::string& except = {});
} // namespace geomsrv::archviz::hudfloorscheme
#endif
