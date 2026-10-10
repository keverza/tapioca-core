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
// Draw and Cut add rectangles to the silhouette or take them out, the slab's own contour dashed.
//
// Every edit is a design (FloorSchemeEdit.hpp) the planner regenerates off the UI thread
// (FloorPlanner.hpp), the newest first, so the canvas never waits; undo and redo swap designs,
// across buildings when a party wall moved. Save writes every changed building's stairs and floor
// designs to its slabs' metadata (one undo step each).
//
// MAIN THREAD, inside ImGui's lock like the rest of the HUD.
#include "ArchViz/FloorPlanner.hpp"
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

// The flat selected in a Plan view, for the overlay to draw it highlighted.
struct Selection {
    std::string building;
    int story = 0;
    floorscheme::Ring flat;
};
std::optional<Selection> Selected (const EditorPtr& editor);
} // namespace geomsrv::archviz::hudfloorscheme
#endif
