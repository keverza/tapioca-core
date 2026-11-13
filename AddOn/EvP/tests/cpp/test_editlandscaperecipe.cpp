// #105 slice 1: the read-only terrain recipe (Edit/EditLandscapeRecipe.hpp) states what the
// terrain job will actually build -- its route, the compiled stack order, constraints, owners
// left out and why, footprint construction and bypass, and the current limitations -- from the
// same plan compiler and routing rule the job uses, without changing the document.
#include "Edit/EditLandscapeRecipe.hpp"
#include "Edit/EditPublishedTerrain.hpp"
#include "Edit/EditLandscapeDomain.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditRegionTerrain.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <optional>

namespace geo = tapioca::geometry;
namespace edit = geomsrv::edit;

namespace {
edit::Document Start ()
{
    edit::Document doc;
    std::string error;
    EXPECT_TRUE (geo::StartLandscape (doc, { 1, 2, 3, 4 }, error)) << error;
    return doc;
}
edit::Id Region (edit::Document& doc, double x, double size = 10)
{
    const auto line =
        edit::AddPolyline (doc, { { x, 0, 0 }, { x + size, 0, 0 }, { x + size, size, 0 }, { x, size, 0 } }, true);
    std::string error;
    EXPECT_TRUE (geo::SetLandscapeSourceRole (doc, line, geo::SourceRole::RegionBoundary, error)) << error;
    return doc.landscape->objects.back ().id;
}
void Configure (edit::Document& doc, edit::Id object, double z,
                geo::RegionTerrainEffect effect = geo::RegionTerrainEffect::Grade)
{
    geo::RegionSurface surface;
    surface.elevation = geo::RegionElevationMode::Constant;
    surface.first.z = z;
    std::string error;
    ASSERT_TRUE (geo::SetRegionSurface (doc, object, surface, {}, error)) << error;
    geo::RegionTerrain terrain;
    terrain.effect = effect;
    ASSERT_TRUE (geo::SetRegionTerrain (doc, object, terrain, error)) << error;
}
edit::Id RootOf (const edit::Document& doc, edit::Id object)
{
    for (const auto& o : doc.landscape->objects)
        if (o.id == object)
            return o.footprintOperation;
    return edit::kNoId;
}
// A drawn path with its centreline source and stroke: the design request for it.
geo::PathDesignRequest Path (edit::Document& doc, double y)
{
    const auto path = edit::AddPath (doc, { { -5, y, 0 }, { 30, y, 0 } }, 3.0);
    geo::LandscapeSource centreline;
    centreline.pathId = path;
    centreline.role = geo::SourceRole::PathCenterline;
    std::string error;
    edit::Id source = 0, operation = 0;
    EXPECT_TRUE (geo::AddLandscapeSource (doc, centreline, source, error)) << error;
    geo::LandscapeOperation stroke;
    stroke.kind = geo::OperationKind::Stroke;
    stroke.inputs = { source };
    stroke.width = 3;
    EXPECT_TRUE (geo::AddLandscapeOperation (doc, stroke, operation, error)) << error;
    geo::PathDesignRequest request;
    request.source = source;
    request.operation = operation;
    return request;
}
std::vector<const geo::RecipeRow*> Rows (const geo::LandscapeRecipe& recipe, geo::RecipeStage stage)
{
    std::vector<const geo::RecipeRow*> out;
    for (const auto& row : recipe.rows)
        if (row.stage == stage)
            out.push_back (&row);
    return out;
}
bool Mentions (const std::vector<std::string>& texts, const std::string& word)
{
    return std::any_of (texts.begin (), texts.end (),
                        [&] (const std::string& t) { return t.find (word) != std::string::npos; });
}
} // namespace

TEST (EditLandscapeRecipe, TwoOverlappingRegionsAndAPathShowTheCompiledOrderAndWhatIsLeftOut)
{
    auto doc = Start ();
    const auto a = Region (doc, 0);
    const auto b = Region (doc, 5);  // overlaps a
    const auto c = Region (doc, 40); // a region with no terrain effect
    Configure (doc, a, 2);
    Configure (doc, b, 5, geo::RegionTerrainEffect::Add);
    const auto request = Path (doc, 3);
    Path (doc, 8); // a second drawn path the unified build does not grade
    const edit::Document before = doc;

    const auto recipe = geo::ProjectLandscapeRecipe (doc, request);
    ASSERT_TRUE (recipe.ok) << recipe.error;
    EXPECT_EQ (recipe.route, geo::RecipeRoute::Unified);
    EXPECT_EQ (recipe.revision, doc.revision);
    // The recipe is the plan the job compiles, not a second evaluation.
    geo::LandscapeSnapshot snapshot;
    snapshot.doc = &doc;
    snapshot.revision = doc.revision;
    snapshot.captureRevision = doc.revision; // a build of this revision captures at it
    snapshot.session = doc.landscape->sessionUuid;
    snapshot.sourceTerrainGuid = doc.landscape->sourceTerrainGuid;
    snapshot.path = request;
    const auto plan = geo::CompileLandscapeFieldPlan (snapshot);
    EXPECT_EQ (recipe.hash, plan.hash);

    const auto regions = Rows (recipe, geo::RecipeStage::Region);
    ASSERT_EQ (regions.size (), plan.entries.size ());
    for (size_t i = 0; i < regions.size (); ++i) {
        EXPECT_EQ (regions[i]->object, plan.entries[i].object);
        EXPECT_EQ (regions[i]->order, i + 1);
    }
    ASSERT_EQ (regions.size (), 2u);
    EXPECT_EQ (regions[0]->object, a);
    EXPECT_EQ (regions[0]->effect, "Grade");
    EXPECT_EQ (regions[0]->target, "Constant 2.00 m");
    EXPECT_EQ (regions[1]->object, b);
    EXPECT_EQ (regions[1]->effect, "Raise only (fill)");
    EXPECT_TRUE (Mentions (regions[1]->notes, "Composes over"));
    const auto paths = Rows (recipe, geo::RecipeStage::Path);
    ASSERT_EQ (paths.size (), 1u);
    EXPECT_EQ (paths[0]->order, 3u) << "the path follows every region";
    EXPECT_EQ (paths[0]->source, request.source);
    const auto excluded = Rows (recipe, geo::RecipeStage::Excluded);
    ASSERT_EQ (excluded.size (), 1u);
    EXPECT_EQ (excluded[0]->object, c);
    EXPECT_TRUE (Mentions (excluded[0]->notes, "terrain effect is None"));
    EXPECT_TRUE (Mentions (recipe.limitations, "1 of 2 paths"));
    // Read-only: the document is untouched.
    EXPECT_EQ (doc.landscape, before.landscape);
    EXPECT_EQ (doc.revision, before.revision);
    EXPECT_EQ (doc.nextId, before.nextId);
}

TEST (EditLandscapeRecipe, AnExplicitPriorityChangeIsTheOrderShown)
{
    auto doc = Start ();
    const auto a = Region (doc, 0);
    const auto b = Region (doc, 5);
    Configure (doc, a, 2);
    Configure (doc, b, 5);
    for (auto& op : doc.landscape->operations)
        if (op.id == RootOf (doc, b))
            op.priority = -1;
    const auto recipe = geo::ProjectLandscapeRecipe (doc, std::nullopt);
    const auto regions = Rows (recipe, geo::RecipeStage::Region);
    ASSERT_EQ (regions.size (), 2u);
    EXPECT_EQ (regions[0]->object, b);
    EXPECT_EQ (regions[0]->priority, -1);
    EXPECT_EQ (regions[1]->object, a);
    EXPECT_TRUE (Mentions (recipe.limitations, "regions only"));
}

// #149 T1: an outline request is one unified stage grading every path's paved area, with or
// without regions; adding a region no longer changes what the request means.
TEST (EditLandscapeRecipe, TheRouteIsTheJobsAndAnOutlineIsOneStageWithOrWithoutRegions149)
{
    auto doc = Start ();
    auto request = Path (doc, 3);
    Path (doc, 8);
    request.outline = true;
    ASSERT_EQ (geo::LandscapeTerrainRoute (doc, true, true), geo::TerrainRoute::Unified);
    auto recipe = geo::ProjectLandscapeRecipe (doc, request);
    ASSERT_TRUE (recipe.ok) << recipe.error;
    EXPECT_EQ (recipe.route, geo::RecipeRoute::Unified);
    auto paths = Rows (recipe, geo::RecipeStage::Path);
    ASSERT_EQ (paths.size (), 1u);
    EXPECT_EQ (paths[0]->name, "Paved outline");
    EXPECT_EQ (paths[0]->kind, "Paths (2)") << "both drawn paths are graded";
    EXPECT_EQ (paths[0]->order, 1u);
    EXPECT_FALSE (Mentions (recipe.limitations, "paths are not in this recipe"));
    // Add one graded region: the same request is the same stage, now after the region.
    const auto a = Region (doc, 0);
    Configure (doc, a, 2);
    recipe = geo::ProjectLandscapeRecipe (doc, request);
    EXPECT_EQ (recipe.route, geo::RecipeRoute::Unified);
    paths = Rows (recipe, geo::RecipeStage::Path);
    ASSERT_EQ (paths.size (), 1u);
    EXPECT_EQ (paths[0]->name, "Paved outline");
    EXPECT_EQ (paths[0]->kind, "Paths (2)");
    EXPECT_EQ (paths[0]->order, 2u) << "after the region";
    EXPECT_FALSE (Mentions (paths[0]->notes, "band"));
    // The band request still grades one selected path and says so.
    request.outline = false;
    recipe = geo::ProjectLandscapeRecipe (doc, request);
    EXPECT_TRUE (Mentions (recipe.limitations, "1 of 2 paths"));
    // Nothing designed and nothing graded.
    auto empty = Start ();
    recipe = geo::ProjectLandscapeRecipe (empty, std::nullopt);
    EXPECT_TRUE (recipe.ok);
    EXPECT_EQ (recipe.route, geo::RecipeRoute::Nothing);
}

TEST (EditLandscapeRecipe, FootprintConstructionShowsTheDagAndBypassIsNotNoEffect)
{
    auto doc = Start ();
    const auto a = Region (doc, 0);
    const auto b = Region (doc, 5);
    Configure (doc, a, 2);
    Configure (doc, b, 5);
    geo::DomainCommand command;
    command.kind = geo::OperationKind::Difference;
    command.name = "Subtract";
    command.subject.input = RootOf (doc, a);
    command.operand.input = RootOf (doc, b);
    const auto applied = geo::ApplyDomainCommand (doc, command);
    ASSERT_TRUE (applied.ok) << applied.error;

    auto recipe = geo::ProjectLandscapeRecipe (doc, std::nullopt);
    const auto regions = Rows (recipe, geo::RecipeStage::Region);
    const auto derived = std::find_if (regions.begin (), regions.end (),
                                       [&] (const geo::RecipeRow* r) { return r->object == applied.objectId; });
    ASSERT_NE (derived, regions.end ());
    const auto& tree = (*derived)->construction;
    ASSERT_FALSE (tree.empty ());
    // Its own root first; a Difference node somewhere below names its operand order; the
    // operands' roots are reached, not copied as unrelated trees.
    EXPECT_EQ (tree.front ().depth, 0u);
    const bool difference = std::any_of (tree.begin (), tree.end (), [] (const geo::RecipeNode& n) {
        return n.label.find ("Difference (first minus second)") != std::string::npos;
    });
    EXPECT_TRUE (difference);
    for (const edit::Id root : { RootOf (doc, a), RootOf (doc, b) })
        EXPECT_TRUE (std::any_of (tree.begin (), tree.end (), [&] (const geo::RecipeNode& n) {
            return n.operation == root;
        })) << root;
    for (size_t i = 1; i < tree.size (); ++i)
        EXPECT_LE (tree[i].depth, tree[i - 1].depth + 1) << "depth first";

    // Bypass the Difference: it is shown as a bypass on the derived row, which keeps its effect.
    edit::Id differenceOp = edit::kNoId;
    for (const auto& n : tree)
        if (n.label.find ("Difference") != std::string::npos)
            differenceOp = n.operation;
    ASSERT_NE (differenceOp, edit::kNoId);
    std::string error;
    ASSERT_TRUE (geo::SetLandscapeOperationEnabled (doc, differenceOp, false, error)) << error;
    recipe = geo::ProjectLandscapeRecipe (doc, std::nullopt);
    const auto again = Rows (recipe, geo::RecipeStage::Region);
    const auto bypassed = std::find_if (again.begin (), again.end (),
                                        [&] (const geo::RecipeRow* r) { return r->object == applied.objectId; });
    ASSERT_NE (bypassed, again.end ());
    EXPECT_EQ ((*bypassed)->effect, "Grade");
    EXPECT_TRUE (Mentions ((*bypassed)->notes, "bypassed"));
    EXPECT_TRUE (std::any_of ((*bypassed)->construction.begin (), (*bypassed)->construction.end (),
                              [] (const geo::RecipeNode& n) { return n.bypassed; }));
}

namespace {
std::vector<edit::Id> Stack (const edit::Document& doc)
{
    std::vector<edit::Id> out;
    const auto recipe = geo::ProjectLandscapeRecipe (doc, std::nullopt);
    for (const auto* row : Rows (recipe, geo::RecipeStage::Region))
        out.push_back (row->object);
    return out;
}
} // namespace

// #105 slice 2: move up/down is the one supported reorder. It changes only the region stage's
// order, as one document edit, and refuses unsupported moves with the document untouched.
TEST (EditLandscapeRecipe, MoveUpDownReordersRegionsAndRefusesUnsupportedMoves105)
{
    auto doc = Start ();
    const auto a = Region (doc, 0);
    const auto b = Region (doc, 5);
    const auto c = Region (doc, 8);
    const auto none = Region (doc, 40);
    for (const auto id : { a, b, c })
        Configure (doc, id, 2);
    ASSERT_EQ (Stack (doc), (std::vector<edit::Id> { a, b, c }));
    std::string error;

    const auto revision = doc.revision;
    ASSERT_TRUE (geo::MoveRecipeRegion (doc, c, -1, error)) << error;
    EXPECT_EQ (Stack (doc), (std::vector<edit::Id> { a, c, b }));
    EXPECT_EQ (doc.revision, revision + 1) << "one document edit";
    ASSERT_TRUE (geo::MoveRecipeRegion (doc, a, 1, error)) << error;
    EXPECT_EQ (Stack (doc), (std::vector<edit::Id> { c, a, b }));

    const edit::Document before = doc;
    const auto refuses = [&] (edit::Id object, int step, const char* reason) {
        std::string why;
        EXPECT_FALSE (geo::MoveRecipeRegion (doc, object, step, why));
        EXPECT_NE (why.find (reason), std::string::npos) << why;
        EXPECT_EQ (doc.landscape, before.landscape);
        EXPECT_EQ (doc.revision, before.revision);
    };
    refuses (c, -1, "Already the first");
    refuses (b, 1, "Paths always compose after every region");
    refuses (none, 1, "no terrain effect");
    refuses (999999, 1, "no longer exists");
    refuses (a, 2, "one place");

    // The order survives a save and reload exactly.
    std::string text;
    ASSERT_TRUE (geo::WriteLandscape (doc, geo::LandscapeStorage::Virtual, text, error)) << error;
    edit::Document reloaded;
    ASSERT_TRUE (geo::ReadLandscape (text, reloaded, error)) << error;
    EXPECT_EQ (Stack (reloaded), Stack (doc));

    // A region added after a move still stacks on top, as before any move.
    const auto d = Region (doc, 12);
    Configure (doc, d, 3);
    EXPECT_EQ (Stack (doc).back (), d);
}

TEST (EditLandscapeRecipe, ARegionSharingItsRootCannotMoveButANeighbourMovesPastTheGroup105)
{
    auto doc = Start ();
    const auto a = Region (doc, 0);
    const auto b = Region (doc, 5);
    Configure (doc, a, 2);
    Configure (doc, b, 2);
    // A second owner of a's footprint root.
    geo::HardscapeObject twin;
    twin.name = "Twin";
    twin.footprintOperation = RootOf (doc, a);
    twin.surface.emplace ();
    twin.surface->elevation = geo::RegionElevationMode::Constant;
    twin.terrain.effect = geo::RegionTerrainEffect::Grade;
    edit::Id twinId = 0;
    std::string error;
    ASSERT_TRUE (geo::AddHardscapeObject (doc, twin, twinId, error)) << error;
    ASSERT_EQ (Stack (doc), (std::vector<edit::Id> { a, twinId, b }));
    const edit::Document before = doc;
    EXPECT_FALSE (geo::MoveRecipeRegion (doc, a, 1, error));
    EXPECT_NE (error.find ("shares its footprint"), std::string::npos) << error;
    EXPECT_EQ (doc.landscape, before.landscape);
    ASSERT_TRUE (geo::MoveRecipeRegion (doc, b, -1, error)) << error;
    EXPECT_EQ (Stack (doc), (std::vector<edit::Id> { b, a, twinId }));
}

// #105 step 4: quantity rows over a published terrain, named after the recipe rows, in stack order
// with the baseline row last; they reconcile to the total; a publication of another revision is
// stale; nothing is reported without one, and the outline lane says why.
TEST (EditLandscapeRecipe, QuantityRowsNameThePublishedPartitionAndMarkAStaleOne105)
{
    auto doc = Start ();
    const auto a = Region (doc, 0);
    const auto b = Region (doc, 5);
    Configure (doc, a, 2);
    Configure (doc, b, 5, geo::RegionTerrainEffect::Add);
    const auto recipe = geo::ProjectLandscapeRecipe (doc, std::nullopt);
    ASSERT_TRUE (recipe.ok) << recipe.error;
    const auto stack = Rows (recipe, geo::RecipeStage::Region);
    ASSERT_EQ (stack.size (), 2u);

    geo::PublishedTerrain published;
    published.revision = recipe.revision;
    published.planHash = 77; // a build's hash includes its capture; not compared
    published.quantities = { 3, 10, 7, 300, 0 };
    published.displaySite = { 2.5, 9.5, 7, 250, 0 };
    // Out of stack order on purpose, with the baseline row first, as the publisher emits it.
    published.domainQuantities = {
        { UINT32_MAX, 0, 0, 1, 0, -1, 100, 4 },
        { 1, stack[1]->operation, stack[1]->object, 0, 6, 6, 100, 8 },
        { 0, stack[0]->operation, stack[0]->object, 2, 4, 2, 100, 8 },
    };
    const auto q = geo::ProjectRecipeQuantities (recipe, &published, false);
    ASSERT_TRUE (q.available);
    EXPECT_TRUE (q.current);
    ASSERT_EQ (q.rows.size (), 3u);
    EXPECT_EQ (q.rows[0].order, 1u);
    EXPECT_EQ (q.rows[0].object, stack[0]->object);
    EXPECT_EQ (q.rows[0].name, stack[0]->name);
    EXPECT_EQ (q.rows[1].order, 2u);
    EXPECT_EQ (q.rows[1].fill, 6);
    EXPECT_TRUE (q.rows[2].baseline);
    EXPECT_EQ (q.rows[2].order, 0u);
    EXPECT_EQ (q.total.net, 7);
    EXPECT_EQ (q.site.fill, 9.5);
    EXPECT_NEAR (q.residual, 0, 1e-12);
    EXPECT_TRUE (Mentions (q.notes, "sequential deltas")) << "the report says what it is not";
    EXPECT_FALSE (Mentions (q.notes, "Stale"));
    const auto text = geo::RecipeQuantitiesText (q);
    EXPECT_EQ (text.rfind ("Stack\tOwner\tCut m3\tFill m3\tNet m3\tArea m2\n", 0), 0u) << text;
    EXPECT_NE (text.find ("1\t" + stack[0]->name + "\t2.000\t4.000\t2.000\t100.000\n"), std::string::npos) << text;
    EXPECT_NE (text.find ("Total (revision " + std::to_string (recipe.revision) + ")\t3.000\t10.000\t7.000"),
               std::string::npos)
        << text;

    // Stale: another revision, or the caller's flag (another request or capture).
    auto older = published;
    older.revision = recipe.revision - 1;
    const auto staleByRevision = geo::ProjectRecipeQuantities (recipe, &older, false);
    EXPECT_FALSE (staleByRevision.current);
    EXPECT_TRUE (Mentions (staleByRevision.notes, "Stale"));
    EXPECT_NE (geo::RecipeQuantitiesText (staleByRevision).find (", stale)"), std::string::npos);
    EXPECT_FALSE (geo::ProjectRecipeQuantities (recipe, &published, true).current);
    // A position the recipe no longer has keeps its number instead of borrowing a name.
    older.domainQuantities[1].object = 9999;
    EXPECT_EQ (geo::ProjectRecipeQuantities (recipe, &older, false).rows[1].name, "Stack position 2");

    // Rows that do not add up say so.
    auto broken = published;
    broken.domainQuantities[1].net = 5;
    const auto unreconciled = geo::ProjectRecipeQuantities (recipe, &broken, false);
    EXPECT_NEAR (unreconciled.residual, 1, 1e-12);
    EXPECT_TRUE (Mentions (unreconciled.notes, "differ by 1.000 m3"));

    // Nothing built; the outline lane publishes no attributed snapshot.
    const auto none = geo::ProjectRecipeQuantities (recipe, nullptr, false);
    EXPECT_FALSE (none.available);
    EXPECT_TRUE (none.rows.empty ());
    EXPECT_NE (none.reason.find ("No terrain"), std::string::npos);
    auto legacy = recipe;
    legacy.route = geo::RecipeRoute::Legacy;
    EXPECT_NE (geo::ProjectRecipeQuantities (legacy, nullptr, false).reason.find ("totals only"), std::string::npos);
    EXPECT_EQ (geo::RecipeQuantitiesText (none), "Stack\tOwner\tCut m3\tFill m3\tNet m3\tArea m2\n");
}
