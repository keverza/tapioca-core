#include "Edit/EditLandscapeRecipe.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditPublishedTerrain.hpp"
#include "Edit/EditRegionTerrain.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>

namespace tapioca::geometry {
namespace {

const char* KindName (ObjectKind kind)
{
    switch (kind) {
        case ObjectKind::Path:
            return "Path";
        case ObjectKind::Playground:
            return "Playground";
        case ObjectKind::Plaza:
            return "Plaza";
        case ObjectKind::Parking:
            return "Parking";
        case ObjectKind::MaterialRegion:
            return "Material region";
        case ObjectKind::Edge:
            return "Edge";
    }
    return "Object";
}

const char* RoleName (SourceRole role)
{
    switch (role) {
        case SourceRole::Guide:
            return "Guide";
        case SourceRole::PathCenterline:
            return "Path centreline";
        case SourceRole::Cutter:
            return "Cutter";
        case SourceRole::MaterialBoundary:
            return "Material boundary";
        case SourceRole::GradingLimit:
            return "Grading limit";
        case SourceRole::ProtectedZone:
            return "Protected zone";
        case SourceRole::Terrain:
            return "Terrain";
        case SourceRole::Building:
            return "Building";
        case SourceRole::ReferenceLine:
            return "Reference line";
        case SourceRole::RegionBoundary:
            return "Region boundary";
    }
    return "Source";
}

std::string Target (const std::optional<RegionSurface>& surface)
{
    if (!surface)
        return "No surface";
    std::string text;
    switch (surface->elevation) {
        case RegionElevationMode::Existing:
            text = "Existing ground";
            break;
        case RegionElevationMode::Constant: {
            char value[48];
            std::snprintf (value, sizeof value, "Constant %.2f m", surface->first.z);
            text = value;
            break;
        }
        case RegionElevationMode::Boundary:
            text = "Boundary curve Z";
            break;
    }
    if (surface->slope == RegionSlopeMode::TwoPoints)
        text += ", two-point slope";
    else if (surface->slope == RegionSlopeMode::ContourLine)
        text += ", contour-line slope";
    return text;
}

std::string PathEffect (TerrainEditEffect effect)
{
    switch (effect) {
        case TerrainEditEffect::Cut:
            return "Lower only (cut)";
        case TerrainEditEffect::Fill:
            return "Raise only (fill)";
        case TerrainEditEffect::Both:
            return "Grade";
    }
    return "Grade";
}

// The footprint construction under `root`, depth first. A node already shown is listed again
// as a reference without its subtree, so a shared operand is never presented as two copies.
struct Tree {
    const LandscapeSession& session;
    std::set<Id> shown, open;
    std::vector<RecipeNode> nodes;

    void Source (Id id, uint32_t depth)
    {
        RecipeNode node;
        node.depth = depth;
        node.source = id;
        const auto source = std::find_if (session.sources.begin (), session.sources.end (),
                                          [&] (const LandscapeSource& s) { return s.id == id; });
        node.label = source == session.sources.end () ? "Missing source " + std::to_string (id)
                                                      : std::string (RoleName (source->role)) + " " +
                                                            std::to_string (id) + (source->deleted ? " (deleted)" : "");
        nodes.push_back (std::move (node));
    }
    void Operation (Id id, uint32_t depth)
    {
        const auto op = std::find_if (session.operations.begin (), session.operations.end (),
                                      [&] (const LandscapeOperation& o) { return o.id == id; });
        if (op == session.operations.end ()) {
            Source (id, depth);
            return;
        }
        RecipeNode node;
        node.depth = depth;
        node.operation = id;
        node.label = RecipeOperationName (op->kind);
        if (op->kind == OperationKind::Difference)
            node.label += " (first minus second)";
        if (op->kind == OperationKind::RegionSelect && !op->faces.empty ())
            node.label += " of " + std::to_string (op->faces.size ()) + " faces";
        node.label += " " + std::to_string (id);
        node.bypassed = !op->enabled;
        node.reference = shown.count (id) != 0 || open.count (id) != 0;
        nodes.push_back (node);
        if (node.reference)
            return;
        shown.insert (id);
        open.insert (id);
        for (const Id input : op->inputs)
            Operation (input, depth + 1);
        open.erase (id);
    }
};

void Locate (LandscapeRecipe& recipe, const FieldPlanDiagnostic& diagnostic)
{
    for (auto& row : recipe.rows)
        if ((diagnostic.object != kNoId && row.object == diagnostic.object) ||
            (diagnostic.object == kNoId && diagnostic.operation != kNoId && row.operation == diagnostic.operation)) {
            row.notes.push_back (diagnostic.message);
            return;
        }
    recipe.diagnostics.push_back (diagnostic.message);
}

} // namespace

const char* RecipeEffectName (RegionTerrainEffect effect)
{
    switch (effect) {
        case RegionTerrainEffect::None:
            return "None";
        case RegionTerrainEffect::Grade:
            return "Grade";
        case RegionTerrainEffect::Add:
            return "Raise only (fill)";
        case RegionTerrainEffect::Subtract:
            return "Lower only (cut)";
    }
    return "None";
}

const char* RecipeOperationName (OperationKind kind)
{
    switch (kind) {
        case OperationKind::Stroke:
            return "Stroke";
        case OperationKind::Offset:
            return "Offset";
        case OperationKind::Union:
            return "Union";
        case OperationKind::Difference:
            return "Difference";
        case OperationKind::Intersection:
            return "Intersection";
        case OperationKind::Xor:
            return "Exclusive or";
        case OperationKind::Split:
            return "Split";
        case OperationKind::RegionSelect:
            return "Region";
        case OperationKind::MaterialMask:
            return "Material mask";
        case OperationKind::GradingLimit:
            return "Grading limit";
        case OperationKind::ProtectedZone:
            return "Protected zone";
    }
    return "Operation";
}

LandscapeRecipe ProjectLandscapeRecipe (const geomsrv::edit::Document& doc,
                                        const std::optional<PathDesignRequest>& path)
{
    LandscapeRecipe recipe;
    if (!doc.landscape) {
        recipe.error = "No landscape session.";
        return recipe;
    }
    const auto& session = *doc.landscape;
    recipe.revision = doc.revision;
    const bool outline = path && path->outline;
    if (LandscapeTerrainRoute (doc, path.has_value (), outline) == TerrainRoute::Legacy) {
        recipe.ok = true;
        // #149 T1: every designed path is unified; only "nothing to grade" stays here.
        recipe.route = RecipeRoute::Nothing;
        recipe.routeText = "Nothing to grade: no region has a terrain effect and no path is designed.";
        return recipe;
    }
    LandscapeSnapshot snapshot;
    snapshot.doc = &doc;
    snapshot.revision = doc.revision;
    // A build of this revision captures the terrain at this revision (EditTerrainCapture.cpp), and
    // its plan hash includes that capture revision: compiling with it makes the recipe's hash the
    // one that build publishes, so the two can be compared.
    snapshot.captureRevision = doc.revision;
    snapshot.session = session.sessionUuid;
    snapshot.sourceTerrainGuid = session.sourceTerrainGuid;
    snapshot.path = path;
    const FieldPlan plan = CompileLandscapeFieldPlan (snapshot);
    recipe.route = RecipeRoute::Unified;
    recipe.routeText = "Unified build: regions in the order below, then the designed path, then masks and fixed "
                       "controls as constraints.";
    if (!plan.ok) {
        recipe.error = plan.error.empty () ? "The recipe cannot be compiled." : plan.error;
        for (const auto& d : plan.diagnostics)
            recipe.diagnostics.push_back (d.message);
        return recipe;
    }
    recipe.ok = true;
    recipe.hash = plan.hash;
    std::set<Id> inStack;
    uint32_t order = 0;
    for (const auto& entry : plan.entries) {
        RecipeRow row;
        row.stage = RecipeStage::Region;
        row.order = ++order;
        row.object = entry.object;
        row.operation = entry.rootOperation;
        row.name = entry.name.empty () ? KindName (entry.kind) : entry.name;
        row.kind = KindName (entry.kind);
        row.effect = RecipeEffectName (entry.terrain.effect);
        row.target = Target (entry.surface);
        row.priority = entry.priority;
        row.sources = entry.sources;
        Tree tree { session };
        tree.Operation (entry.rootOperation, 0);
        row.construction = std::move (tree.nodes);
        for (const auto& node : row.construction)
            if (node.bypassed) {
                row.notes.push_back ("A footprint step is bypassed: its first input passes through. That is not "
                                     "the same as no terrain effect.");
                break;
            }
        if (order > 1)
            row.notes.push_back ("Composes over the rows above it where they overlap.");
        inStack.insert (entry.object);
        recipe.rows.push_back (std::move (row));
    }
    for (const auto& planned : plan.paths) {
        RecipeRow row;
        row.stage = RecipeStage::Path;
        row.order = ++order;
        row.object = planned.object;
        row.operation = planned.operation;
        row.source = planned.source;
        row.path = planned.path;
        const auto owner = std::find_if (session.objects.begin (), session.objects.end (),
                                         [&] (const HardscapeObject& o) { return o.id == planned.object; });
        row.name = owner != session.objects.end () && !owner->name.empty () ? owner->name : "Path";
        row.kind = "Path";
        row.effect = PathEffect (planned.design.effect);
        row.target = "Path profile and cross-sections";
        row.notes.push_back ("Always after every region: a path is not reordered among them.");
        inStack.insert (planned.object);
        recipe.rows.push_back (std::move (row));
    }
    // #149 T1: an outline request is one stage grading every path's paved area.
    for (const auto& planned : plan.outlines) {
        RecipeRow row;
        row.stage = RecipeStage::Path;
        row.order = ++order;
        row.object = planned.object;
        row.operation = planned.operation;
        row.source = planned.source;
        row.name = "Paved outline";
        row.kind = "Paths (" + std::to_string (planned.paths.size ()) + ")";
        row.effect = PathEffect (planned.design.effect);
        row.target = "Paved surface of every path; side slopes from its outline";
        row.sources = planned.pathSources;
        row.notes.push_back ("Always after every region: the side slopes meet the ground the regions composed.");
        inStack.insert (planned.object);
        recipe.rows.push_back (std::move (row));
    }
    for (const auto& mask : plan.masks) {
        RecipeRow row;
        row.stage = RecipeStage::Mask;
        row.operation = mask.operation;
        row.kind = RecipeOperationName (mask.kind);
        row.name = row.kind + " " + std::to_string (mask.operation);
        row.effect = mask.kind == OperationKind::ProtectedZone ? "Keeps existing ground" : "Bounds grading";
        row.sources = mask.sources;
        recipe.rows.push_back (std::move (row));
    }
    for (const auto& control : plan.controls) {
        RecipeRow row;
        row.stage = RecipeStage::Control;
        row.source = control.source;
        row.kind = control.point ? "Fixed point" : "Fixed line";
        row.name = row.kind + " on source " + std::to_string (control.source);
        row.effect = "Fixed height";
        recipe.rows.push_back (std::move (row));
    }
    size_t otherPaths = 0;
    for (const auto& object : session.objects) {
        if (inStack.count (object.id))
            continue;
        if (object.kind == ObjectKind::Path) {
            ++otherPaths;
            continue;
        }
        RecipeRow row;
        row.stage = RecipeStage::Excluded;
        row.object = object.id;
        row.operation = object.footprintOperation;
        row.name = object.name.empty () ? KindName (object.kind) : object.name;
        row.kind = KindName (object.kind);
        row.effect = RecipeEffectName (object.terrain.effect);
        row.target = Target (object.surface);
        row.notes.push_back (object.terrain.effect == RegionTerrainEffect::None
                                 ? "Not in the terrain recipe: its terrain effect is None (footprint and material "
                                   "only)."
                                 : "Not in the terrain recipe: it has no live footprint root.");
        recipe.rows.push_back (std::move (row));
    }
    const size_t designed = plan.outlines.empty () ? plan.paths.size () : plan.outlines.front ().paths.size ();
    const size_t drawn = doc.paths.size ();
    if (drawn > designed)
        recipe.limitations.push_back (
            std::to_string (drawn - designed) + " of " + std::to_string (drawn) + " paths are not in this recipe: " +
            (plan.outlines.empty () ? "band grading grades one selected path." : "they do not validate."));
    else if (otherPaths > 0)
        recipe.limitations.push_back ("Other path objects are not in this recipe: one path per build.");
    if (!path)
        recipe.limitations.push_back ("Paths are off: regions only.");
    for (const auto& diagnostic : plan.diagnostics)
        Locate (recipe, diagnostic);
    return recipe;
}

namespace {
FieldPlan RegionPlan (const geomsrv::edit::Document& doc)
{
    LandscapeSnapshot snapshot;
    snapshot.doc = &doc;
    snapshot.revision = doc.revision;
    snapshot.session = doc.landscape->sessionUuid;
    snapshot.sourceTerrainGuid = doc.landscape->sourceTerrainGuid;
    return CompileLandscapeFieldPlan (snapshot); // Region order does not depend on the path request.
}
std::vector<Id> StackObjects (const FieldPlan& plan)
{
    std::vector<Id> objects;
    for (const auto& entry : plan.entries)
        objects.push_back (entry.object);
    return objects;
}
} // namespace

bool MoveRecipeRegion (geomsrv::edit::Document& doc, Id object, int step, std::string& error)
{
    error.clear ();
    if (!doc.landscape || (step != -1 && step != 1)) {
        error = "Move a region one place up or down.";
        return false;
    }
    const auto& session = *doc.landscape;
    const auto owner = std::find_if (session.objects.begin (), session.objects.end (),
                                     [=] (const HardscapeObject& o) { return o.id == object; });
    if (owner == session.objects.end ()) {
        error = "That region no longer exists.";
        return false;
    }
    if (owner->pathId) {
        error = "Paths always compose after every region; a path cannot be moved among them.";
        return false;
    }
    const FieldPlan plan = RegionPlan (doc);
    if (!plan.ok) {
        error = "The terrain recipe cannot be compiled: " + plan.error;
        return false;
    }
    // Stack units: consecutive entries sharing one root sort together (same priority and
    // topological position), so the root, not the object, is what can move.
    std::vector<Id> roots;
    std::map<Id, size_t> members;
    for (const auto& entry : plan.entries) {
        if (roots.empty () || roots.back () != entry.rootOperation)
            roots.push_back (entry.rootOperation);
        ++members[entry.rootOperation];
    }
    const auto entry = std::find_if (plan.entries.begin (), plan.entries.end (),
                                     [=] (const FieldPlanEntry& e) { return e.object == object; });
    if (entry == plan.entries.end ()) {
        error = owner->terrain.effect == RegionTerrainEffect::None
                    ? "This region has no terrain effect, so it is not in the stack. Set an effect first."
                    : "This region is not in the terrain stack (its footprint has no live root).";
        return false;
    }
    if (members[entry->rootOperation] > 1) {
        error = "This region shares its footprint with another region; their order follows object id and "
                "cannot be changed.";
        return false;
    }
    const size_t at = size_t (std::find (roots.begin (), roots.end (), entry->rootOperation) - roots.begin ());
    if (step < 0 && at == 0) {
        error = "Already the first region in the stack.";
        return false;
    }
    if (step > 0 && at + 1 == roots.size ()) {
        error = "Already the last region. Paths always compose after every region.";
        return false;
    }
    std::vector<Id> expected;
    {
        auto moved = roots;
        std::swap (moved[at], moved[at + step]);
        roots = std::move (moved);
        std::map<Id, std::vector<Id>> byRoot;
        for (const auto& e : plan.entries)
            byRoot[e.rootOperation].push_back (e.object);
        for (Id root : roots)
            expected.insert (expected.end (), byRoot[root].begin (), byRoot[root].end ());
    }
    std::vector<std::pair<Id, int>> priorities;
    for (size_t i = 0; i < roots.size (); ++i)
        priorities.emplace_back (roots[i], int (i) - int (roots.size () - 1));
    geomsrv::edit::Document candidate = doc;
    if (!SetLandscapeOperationPriorities (candidate, priorities, error))
        return false;
    const FieldPlan after = RegionPlan (candidate);
    if (!after.ok || StackObjects (after) != expected) {
        error = "The stack order could not be changed.";
        return false;
    }
    doc = std::move (candidate);
    return true;
}

// ---- #105 step 4: quantity rows over the published terrain -----------------------------------
namespace {
RecipeVolumes Volumes (const HybridVolumes& v)
{
    return { v.cut, v.fill, v.net, v.area };
}

std::string Cubic (double value)
{
    char text[64];
    std::snprintf (text, sizeof text, "%.3f", value);
    return text;
}
} // namespace

RecipeQuantities ProjectRecipeQuantities (const LandscapeRecipe& recipe, const PublishedTerrain* published, bool stale)
{
    RecipeQuantities out;
    if (!published) {
        out.reason = recipe.route == RecipeRoute::Legacy
                         ? "Quantities by owner come from the unified build. The outline lane reports totals only "
                           "(Terrain tab)."
                         : "No terrain has been built yet. Build it to see its quantities.";
        return out;
    }
    out.available = true;
    out.revision = published->revision;
    out.hash = published->planHash;
    // By revision; a request or capture change is the caller's `stale`. The plan hashes agree for a
    // build of this revision (EditPublishedTerrain *105 pins it) and are shown, not re-compared.
    out.current = !stale && recipe.ok && published->revision == recipe.revision;
    out.total = Volumes (published->quantities);
    out.site = Volumes (published->displaySite);
    out.context = Volumes (published->displayContext);
    out.rebuildSite = Volumes (published->exactSite);
    out.rebuildContext = Volumes (published->exactContext);
    double net = 0;
    std::vector<RecipeQuantityRow> baseline;
    for (const auto& domain : published->domainQuantities) {
        RecipeQuantityRow row;
        row.cut = domain.cut;
        row.fill = domain.fill;
        row.net = domain.net;
        row.area = domain.area;
        row.triangles = domain.triangles;
        net += domain.net;
        if (domain.order == UINT32_MAX) {
            row.baseline = true;
            row.name = "Existing ground (no owner)";
            baseline.push_back (std::move (row));
            continue;
        }
        row.order = domain.order + 1;
        row.object = domain.object;
        row.operation = domain.operation;
        // Named after its recipe row only when the recipe is the one this terrain was built from:
        // a stale publication's positions may no longer mean the same rows.
        const auto named = std::find_if (recipe.rows.begin (), recipe.rows.end (), [&] (const RecipeRow& r) {
            return r.order != 0 && r.object == domain.object && r.operation == domain.operation;
        });
        if (named != recipe.rows.end ()) {
            row.name = named->name;
            row.path = named->path;
        }
        else
            row.name = "Stack position " + std::to_string (row.order);
        out.rows.push_back (std::move (row));
    }
    std::stable_sort (out.rows.begin (), out.rows.end (),
                      [] (const RecipeQuantityRow& a, const RecipeQuantityRow& b) { return a.order < b.order; });
    out.rows.insert (out.rows.end (), baseline.begin (), baseline.end ());
    out.residual = std::fabs (net - out.total.net);
    out.notes.push_back ("Exact: each triangle's height above or below the existing ground, integrated over the "
                         "whole published surface.");
    out.notes.push_back ("Rows: each triangle counts once, for the last region whose footprint holds it, else the "
                         "last owner that moved it, else the existing ground. Rows add up to the total.");
    out.notes.push_back ("Not shown: how much each operation changed the ground on its own (sequential deltas). "
                         "A row is not that operation's own cut and fill.");
    if (!out.current)
        out.notes.push_back ("Stale: built from revision " + std::to_string (published->revision) +
                             ". Inspect only; it is not what the document now describes.");
    if (out.residual > 1e-6)
        out.notes.push_back ("Rows and total differ by " + Cubic (out.residual) + " m3.");
    return out;
}

std::string RecipeQuantitiesText (const RecipeQuantities& q)
{
    std::string text = "Stack\tOwner\tCut m3\tFill m3\tNet m3\tArea m2\n";
    const auto line = [&] (const std::string& order, const std::string& name, const RecipeVolumes& v) {
        text += order + "\t" + name + "\t" + Cubic (v.cut) + "\t" + Cubic (v.fill) + "\t" + Cubic (v.net) + "\t" +
                Cubic (v.area) + "\n";
    };
    for (const auto& row : q.rows)
        line (row.baseline ? "-" : std::to_string (row.order), row.name, { row.cut, row.fill, row.net, row.area });
    if (q.available)
        line ("", "Total (revision " + std::to_string (q.revision) + (q.current ? ")" : ", stale)"), q.total);
    return text;
}

} // namespace tapioca::geometry
