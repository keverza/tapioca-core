#include "Edit/EditLandscapeRecipe.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditRegionTerrain.hpp"
#include <algorithm>
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
        recipe.route = path ? RecipeRoute::Legacy : RecipeRoute::Nothing;
        recipe.routeText = path ? "Legacy outline lane: every path is graded from its paved outline. No region "
                                  "has a terrain effect, so there is no region stage."
                                : "Nothing to grade: no region has a terrain effect and no path is designed.";
        if (path)
            recipe.limitations.push_back ("Adding a region with a terrain effect switches this to the unified build, "
                                          "which grades only the selected path's band (#149).");
        return recipe;
    }
    LandscapeSnapshot snapshot;
    snapshot.doc = &doc;
    snapshot.revision = doc.revision;
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
        if (planned.design.outline)
            row.notes.push_back ("Outline grading was asked for, but with regions present this path is graded as "
                                 "its band, not its paved outline (#149).");
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
    const size_t designed = plan.paths.size ();
    const size_t drawn = doc.paths.size ();
    if (drawn > designed)
        recipe.limitations.push_back (std::to_string (drawn - designed) + " of " + std::to_string (drawn) +
                                      " paths are not in this recipe: the unified build grades one selected path.");
    else if (otherPaths > 0)
        recipe.limitations.push_back ("Other path objects are not in this recipe: one path per build.");
    if (!path)
        recipe.limitations.push_back ("Paths are off: regions only.");
    for (const auto& diagnostic : plan.diagnostics)
        Locate (recipe, diagnostic);
    return recipe;
}

} // namespace tapioca::geometry
