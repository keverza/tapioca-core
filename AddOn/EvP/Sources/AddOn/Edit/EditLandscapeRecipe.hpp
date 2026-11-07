#ifndef TAPIOCA_EDIT_LANDSCAPE_RECIPE_HPP
#define TAPIOCA_EDIT_LANDSCAPE_RECIPE_HPP

// #105 slice 1 / #157 G2: a read-only projection of the terrain recipe that would actually be
// built for a document and its current design request. SDK-free and pure: it compiles the
// one field plan (EditLandscapeFieldPlan.hpp) and reads the routing rule the terrain job
// dispatches on (EditRegionTerrain.hpp LandscapeTerrainRoute); it never evaluates a second
// way, never writes the document and never runs a terrain build.
//
// What it states, and why each is separate:
//   - the route: the legacy outline lane, or the unified build;
//   - the effective stack order: region entries in compiled order (root priority, topological
//     position, object id; later entries compose over earlier ones), then the one designed
//     path, which always follows every region; masks and fixed controls are constraints,
//     listed apart and never ordered with the stack;
//   - owners that are not in the stack, with the reason (no terrain effect, no live root...);
//   - each entry's footprint construction as a dependency tree: an operand used by more than
//     one parent is shown as a reference, not copied; a disabled node is a footprint bypass
//     (its first input passes through), which is not the same as "no terrain effect";
//   - located compiler diagnostics and the current limitations (band grading grades one
//     selected path; an outline request grades every path's paved area, #149 T1).
#include "Edit/EditLandscapeFieldPlan.hpp"
#include <optional>
#include <string>
#include <vector>

namespace tapioca::geometry {

enum class RecipeRoute : uint8_t { Nothing, Legacy, Unified };
enum class RecipeStage : uint8_t { Region, Path, Mask, Control, Excluded };

// One node of an entry's footprint construction, depth-first from its root.
struct RecipeNode {
    uint32_t depth = 0;
    Id operation = kNoId;   // kNoId: a source leaf
    Id source = kNoId;      // the leaf's source
    std::string label;      // "Union", "Difference (A minus B)", "Region boundary 12"...
    bool bypassed = false;  // a disabled operation: its first input passes through
    bool reference = false; // already shown above (shared operand): not expanded again
    bool operator== (const RecipeNode&) const = default;
};

struct RecipeRow {
    RecipeStage stage = RecipeStage::Region;
    uint32_t order = 0; // 1-based stack position; 0 for masks, controls and excluded owners
    Id object = kNoId, operation = kNoId, source = kNoId, path = kNoId;
    std::string name, kind; // "Plaza", "Path", "Protected zone"...
    std::string effect;     // "Grade", "Raise only (fill)", "Lower only (cut)", "None"
    std::string target;     // "Boundary curve Z", "Constant 12.50 m", "Existing", "Path profile"
    int priority = 0;
    std::vector<Id> sources;
    std::vector<RecipeNode> construction;
    std::vector<std::string> notes; // located diagnostics and per-row limitations
    bool operator== (const RecipeRow&) const = default;
};

struct LandscapeRecipe {
    bool ok = false; // false: no recipe can be stated (`error` says why)
    std::string error;
    RecipeRoute route = RecipeRoute::Nothing;
    std::string routeText;
    uint64_t revision = 0, hash = 0; // the compiled plan's (hash 0 on the legacy lane)
    std::vector<RecipeRow> rows;     // stack rows in order, then masks, controls, excluded
    std::vector<std::string> limitations;
    std::vector<std::string> diagnostics; // plan diagnostics not located on a row
    bool operator== (const LandscapeRecipe&) const = default;
};

// `path`: the current design request (PathRequestFor of the design's options), absent when
// the design grades regions only or no design exists.
LandscapeRecipe ProjectLandscapeRecipe (const geomsrv::edit::Document& doc,
                                        const std::optional<PathDesignRequest>& path);

// #105 slice 2: the one supported recipe reorder. Moves a region one place earlier (step -1,
// toward order 1, composed under) or later (+1, composed over) in the compiled terrain stack by
// renumbering the stack roots' explicit priorities to the new order (the top root gets 0, so a
// newly added region still stacks above by topological position). One document edit.
// Refuses, with a user-facing reason and the document untouched: a region not in the stack
// (no terrain effect, or no live root), a path (paths always compose after every region), a
// region already at that end, and a region whose footprint root another region shares (their
// relative order is fixed by object id).
bool MoveRecipeRegion (geomsrv::edit::Document& doc, Id object, int step, std::string& error);

const char* RecipeEffectName (RegionTerrainEffect effect);
const char* RecipeOperationName (OperationKind kind);

} // namespace tapioca::geometry
#endif
