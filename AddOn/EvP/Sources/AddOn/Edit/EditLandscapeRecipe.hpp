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
    uint64_t revision = 0, hash = 0; // the compiled plan's, as a build of this revision publishes it
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

// #105 step 4: read-only quantity rows over one PUBLISHED terrain (EditPublishedTerrain.hpp), named
// after the recipe's rows. Nothing is measured here: the rows are the publication's own
// triangle-exact final-owner partition (each triangle whole to the last region whose footprint
// holds it, else to its last moving owner, else to the baseline row), and the totals are its
// whole-surface, 0.25 m display and rebuild-scope figures, each labelled with its method.
// Three reports are kept apart and never summed into one another:
//   - final totals: baseline versus proposal over the whole published surface;
//   - final-owner attribution: the rows, which plus the baseline row reconcile to that total;
//   - sequential per-operation deltas: not computed (each would need its own partial build).
// A publication of another document revision, or one the caller reports stale (another request
// or capture), is shown as stale: inspectable, never current commit input.
struct PublishedTerrain;
struct RecipeQuantityRow {
    uint32_t order = 0; // 1-based stack position; 0 for the baseline row
    Id object = kNoId, operation = kNoId, path = kNoId;
    bool baseline = false; // triangles no domain claims and no owner moved
    std::string name;      // the recipe row's name, or "Stack position N" when the recipe has none
    double cut = 0, fill = 0, net = 0, area = 0; // m3, m3, m3 (fill - cut), m2
    uint64_t triangles = 0;
    bool operator== (const RecipeQuantityRow&) const = default;
};
struct RecipeVolumes {
    double cut = 0, fill = 0, net = 0, area = 0;
    bool operator== (const RecipeVolumes&) const = default;
};
struct RecipeQuantities {
    bool available = false; // false: `reason` says why there are no rows
    std::string reason;
    bool current = false;                      // the publication is of the recipe's revision and not stale
    uint64_t revision = 0, hash = 0;           // the publication's
    RecipeVolumes total;                       // exact, whole published surface
    RecipeVolumes site, context;               // 0.25 m grid against the property (the Terrain tab's figures)
    RecipeVolumes rebuildSite, rebuildContext; // exact, inside/outside the rebuilt area
    double residual = 0;                       // |sum of row nets - total net|, m3
    std::vector<RecipeQuantityRow> rows;       // stack order, then the baseline row
    std::vector<std::string> notes;            // method, scope and what is not reported
    bool operator== (const RecipeQuantities&) const = default;
};
// `published` null: nothing built on the unified lane (the legacy outline lane publishes no
// attributed snapshot). `stale`: the caller already knows the publication is not current.
RecipeQuantities ProjectRecipeQuantities (const LandscapeRecipe& recipe, const PublishedTerrain* published, bool stale);
// Tab-separated table (header, rows, total) for the clipboard.
std::string RecipeQuantitiesText (const RecipeQuantities& quantities);

const char* RecipeEffectName (RegionTerrainEffect effect);
const char* RecipeOperationName (OperationKind kind);

} // namespace tapioca::geometry
#endif
