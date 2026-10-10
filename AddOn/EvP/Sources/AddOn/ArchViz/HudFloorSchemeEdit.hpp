#ifndef EVP_ARCHVIZ_HUDFLOORSCHEMEEDIT_HPP
#define EVP_ARCHVIZ_HUDFLOORSCHEMEEDIT_HPP

// The Plan view's Edit: the whole massing floor at the shown elevation (every building there) as
// one typology scheme on a canvas, edited like a limited drawing program that follows the
// skeleton (user, 2026-10-10): drag stairs, party walls and corridor ends; split, join and
// re-room flats; add or remove stairs; draw rectangles onto or out of the silhouette; set a
// wing's access. Every gesture is a design (FloorSchemeEdit.hpp) regenerated off the UI thread,
// latest gesture first, so the canvas never waits; undo, redo and reset swap designs.
//
// MAIN THREAD, inside ImGui's lock like the rest of the HUD. Session-local: nothing is written
// to the project.
#include "ArchViz/HudBuildingPlan.hpp"
#include <memory>

namespace geomsrv::archviz::hudfloorscheme {
struct Editor;
struct EditorDeleter {
    void operator() (Editor* editor) const;
};
using EditorPtr = std::unique_ptr<Editor, EditorDeleter>;

// The Edit button for `shown` and, while editing that elevation, the editor. `plans` holds every
// building's floors; `owner` names the building whose panel draws the canvas.
void Section (EditorPtr& editor, const std::map<std::string, buildingplan::Plan>& plans,
              const buildingplan::Floor& shown, const std::string& owner, const floorprogramme::Programme& programme,
              float scale);
} // namespace geomsrv::archviz::hudfloorscheme
#endif
