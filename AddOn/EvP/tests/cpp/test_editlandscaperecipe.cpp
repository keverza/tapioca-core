// #105 slice 1: the read-only terrain recipe (Edit/EditLandscapeRecipe.hpp) states what the
// terrain job will actually build -- its route, the compiled stack order, constraints, owners
// left out and why, footprint construction and bypass, and the current limitations -- from the
// same plan compiler and routing rule the job uses, without changing the document.
#include "Edit/EditLandscapeRecipe.hpp"
#include "Edit/EditLandscapeDomain.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditRegionTerrain.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include <gtest/gtest.h>
#include <algorithm>

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

TEST (EditLandscapeRecipe, TheRouteIsTheJobsAndTheOutlineToBandSwitchIsStated)
{
    auto doc = Start ();
    auto request = Path (doc, 3);
    request.outline = true;
    // No region terrain target: the legacy outline lane, as the job dispatches.
    auto recipe = geo::ProjectLandscapeRecipe (doc, request);
    ASSERT_EQ (geo::LandscapeTerrainRoute (doc, true, true), geo::TerrainRoute::Legacy);
    EXPECT_EQ (recipe.route, geo::RecipeRoute::Legacy);
    EXPECT_TRUE (recipe.rows.empty ());
    EXPECT_TRUE (Mentions (recipe.limitations, "#149"));
    // Add one graded region: the same outline request becomes the selected path's band.
    const auto a = Region (doc, 0);
    Configure (doc, a, 2);
    ASSERT_EQ (geo::LandscapeTerrainRoute (doc, true, true), geo::TerrainRoute::Unified);
    recipe = geo::ProjectLandscapeRecipe (doc, request);
    EXPECT_EQ (recipe.route, geo::RecipeRoute::Unified);
    const auto paths = Rows (recipe, geo::RecipeStage::Path);
    ASSERT_EQ (paths.size (), 1u);
    EXPECT_TRUE (Mentions (paths[0]->notes, "graded as its band"));
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
