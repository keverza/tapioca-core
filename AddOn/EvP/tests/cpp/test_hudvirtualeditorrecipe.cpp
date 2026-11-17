// The Operations tab's recipe (HudVirtualEditorRecipe.hpp, #105 slice 1): drawn without editing
// anything, recompiled only when the document changes, and a row click selects that region's
// boundary line in the view.
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorRecipe.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "Edit/EditLandscape.hpp"
#include "Edit/EditLandscapeDomain.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <imgui.h>
#include <imgui_internal.h>

namespace eh = geomsrv::archviz::edithud;
namespace ed = geomsrv::edit;
namespace geo = tapioca::geometry;

namespace {
struct RecipeDrive {
    ImGuiContext* previous = ImGui::GetCurrentContext ();
    ImGuiContext* context = ImGui::CreateContext ();
    eh::Session session;

    explicit RecipeDrive (ed::Document doc) : session (std::move (doc))
    {
        auto& io = ImGui::GetIO ();
        io.IniFilename = nullptr;
        io.DisplaySize = { 600, 800 };
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    }
    RecipeDrive (const RecipeDrive&) = delete;
    RecipeDrive& operator= (const RecipeDrive&) = delete;
    ~RecipeDrive ()
    {
        ImGui::DestroyContext (context);
        ImGui::SetCurrentContext (previous);
    }
    void Frame (ImVec2 pointer = { 590, 790 }, bool down = false)
    {
        auto& io = ImGui::GetIO ();
        io.DeltaTime = 1.0f / 60;
        io.AddMousePosEvent (pointer.x, pointer.y);
        io.AddMouseButtonEvent (0, down);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 580, 780 });
        ImGui::Begin ("recipe");
        eh::RecipePanel (session);
        ImGui::End ();
        ImGui::Render ();
    }
    // Presses whatever item is under the pointer, top to bottom, until a click selects something:
    // the only selecting items in the panel are its rows.
    bool ClickFirstRow ()
    {
        for (float y = 10; y < 770; y += 4) {
            Frame ({ 60, y });
            if (context->HoveredId == 0)
                continue;
            Frame ({ 60, y }, true);
            Frame ({ 60, y }, false);
            if (!session.selection.Items ().empty ())
                return true;
        }
        return false;
    }
};

ed::Id Region (ed::Document& doc, double x, ed::Id& line)
{
    line = ed::AddPolyline (doc, { { x, 0, 0 }, { x + 10, 0, 0 }, { x + 10, 10, 0 }, { x, 10, 0 } }, true);
    std::string error;
    EXPECT_TRUE (geo::SetLandscapeSourceRole (doc, line, geo::SourceRole::RegionBoundary, error)) << error;
    const ed::Id object = doc.landscape->objects.back ().id;
    geo::RegionSurface surface;
    surface.elevation = geo::RegionElevationMode::Constant;
    surface.first.z = 2;
    EXPECT_TRUE (geo::SetRegionSurface (doc, object, surface, {}, error)) << error;
    geo::RegionTerrain terrain;
    terrain.effect = geo::RegionTerrainEffect::Grade;
    EXPECT_TRUE (geo::SetRegionTerrain (doc, object, terrain, error)) << error;
    return object;
}
} // namespace

TEST (HudVirtualEditorRecipe, DrawsReadOnlyCachesPerRevisionAndARowSelectsItsLine)
{
    ed::Document doc;
    std::string error;
    ASSERT_TRUE (geo::StartLandscape (doc, { 1, 2, 3, 4 }, error)) << error;
    ed::Id line = ed::kNoId;
    Region (doc, 0, line);
    RecipeDrive d (std::move (doc));
    const auto before = d.session.edits.Doc ();
    d.Frame ();
    ASSERT_NE (d.session.recipe, nullptr);
    const auto* compiled = d.session.recipe.get ();
    ASSERT_TRUE (compiled->ok) << compiled->error;
    EXPECT_EQ (compiled->route, geo::RecipeRoute::Unified);
    d.Frame ();
    EXPECT_EQ (d.session.recipe.get (), compiled) << "no recompile without a change";
    EXPECT_EQ (d.session.edits.Doc ().revision, before.revision);
    EXPECT_EQ (d.session.edits.UndoDepth (), 0u);

    ASSERT_TRUE (d.ClickFirstRow ());
    ASSERT_EQ (d.session.selection.Items ().size (), 1u);
    EXPECT_EQ (d.session.selection.Items ()[0].entity, line);
    EXPECT_EQ (d.session.edits.Doc ().landscape, before.landscape) << "a row click only selects";
    EXPECT_EQ (d.session.edits.UndoDepth (), 0u);

    // An edit recompiles on the next frame.
    ASSERT_TRUE (d.session.edits.Apply ([] (ed::Document& doc, std::string& e) {
        for (auto& op : doc.landscape->operations)
            op.priority = 2;
        return !e.size ();
    }));
    d.Frame ();
    EXPECT_NE (d.session.recipe.get (), compiled);
    EXPECT_EQ (d.session.recipe->revision, d.session.edits.Doc ().revision);
}

// #105 slice 2: the row's arrows move a region in the stack as one undo step; a refused move
// (the first row up) says why and records nothing; undo and reload restore the exact order.
TEST (HudVirtualEditorRecipe, RowArrowsMoveARegionAsOneUndoAndRefuseWithAReason105)
{
    ed::Document doc;
    std::string error;
    ASSERT_TRUE (geo::StartLandscape (doc, { 1, 2, 3, 4 }, error)) << error;
    ed::Id lineA = 0, lineB = 0;
    const auto a = Region (doc, 0, lineA);
    const auto b = Region (doc, 5, lineB);
    RecipeDrive d (std::move (doc));
    const auto order = [&] {
        std::vector<ed::Id> out;
        for (const auto& row : eh::CurrentRecipe (d.session).rows)
            if (row.stage == geo::RecipeStage::Region)
                out.push_back (row.object);
        return out;
    };
    ASSERT_EQ (order (), (std::vector<ed::Id> { a, b }));
    // #191 U5: the first row's up arrow is disabled, and its tooltip gives the refusal's reason.
    EXPECT_NE (eh::RecipeMoveRefusal (d.session, a, -1).find ("Already the first"), std::string::npos)
        << eh::RecipeMoveRefusal (d.session, a, -1);
    EXPECT_TRUE (eh::RecipeMoveRefusal (d.session, a, 1).empty ());
    // Press each control in reading order until one publishes an edit: the first row's up arrow
    // (disabled), then its down arrow. Below the title bar, whose collapse arrow would fold the panel.
    bool moved = false;
    for (float y = 30; y < 770 && !moved; y += 4)
        for (float x = 12; x < 120 && !moved; x += 6) {
            d.Frame ({ x, y });
            if (d.context->HoveredId == 0)
                continue;
            d.Frame ({ x, y }, true);
            d.Frame ({ x, y }, false);
            moved = d.session.edits.UndoDepth () > 0;
        }
    ASSERT_TRUE (moved) << d.session.message;
    EXPECT_EQ (d.session.edits.UndoDepth (), 1u);
    EXPECT_EQ (order (), (std::vector<ed::Id> { b, a }));
    std::string text;
    ASSERT_TRUE (geo::WriteLandscape (d.session.edits.Doc (), geo::LandscapeStorage::Virtual, text, error));
    ASSERT_TRUE (d.session.edits.Undo ());
    EXPECT_EQ (order (), (std::vector<ed::Id> { a, b }));
    ASSERT_TRUE (eh::ReloadLandscape (d.session, text)) << d.session.message;
    EXPECT_EQ (order (), (std::vector<ed::Id> { b, a }));
}

// #105 step 4, as #191 Decided 4 moves it: the Quantities section no longer offers Build terrain --
// Build mesh with nothing selected and the state chip do. No control in the recipe asks for a build
// or says anything; the build itself, asked without a terrain, still says why and records nothing.
TEST (HudVirtualEditorRecipe, QuantitiesNoLongerOfferBuildAndABuildRefusesWithAReason191)
{
    ed::Document doc;
    std::string error;
    ASSERT_TRUE (geo::StartLandscape (doc, { 1, 2, 3, 4 }, error)) << error;
    ed::Id line = ed::kNoId;
    Region (doc, 0, line);
    RecipeDrive d (std::move (doc));
    const auto before = d.session.edits.Doc ();
    for (float y = 770; y > 30; y -= 4)
        for (float x = 12; x < 160; x += 6) {
            d.Frame ({ x, y });
            if (d.context->HoveredId == 0)
                continue;
            d.Frame ({ x, y }, true);
            d.Frame ({ x, y }, false);
            ASSERT_TRUE (d.session.message.empty ()) << "at " << x << ", " << y << ": " << d.session.message;
        }
    EXPECT_FALSE (eh::RequestTerrainBuild (d.session));
#ifdef TAPIOCA_GEOMETRY_RUST
    EXPECT_NE (d.session.message.find ("before building"), std::string::npos) << d.session.message;
#else
    EXPECT_EQ (d.session.message, "Terrain design requires the Rust geometry build.");
#endif
    EXPECT_EQ (d.session.edits.UndoDepth (), 0u);
    EXPECT_EQ (d.session.edits.Doc ().landscape, before.landscape);
    EXPECT_FALSE (d.session.terrainDesign) << "Build asks; it starts nothing without a terrain";
}

// #220: "Footprint on" is refused, with a reason, where a bypass could never build -- a region root
// that reads its line directly -- and allowed on a Boolean result; turning a step back on is never
// refused.
TEST (HudVirtualEditorRecipe, BypassIsRefusedWithAReasonWhereItCouldNeverBuild220)
{
    ed::Document doc;
    std::string error;
    ASSERT_TRUE (geo::StartLandscape (doc, { 1, 2, 3, 4 }, error)) << error;
    ed::Id lineA = 0, lineB = 0;
    const auto a = Region (doc, 0, lineA);
    const auto b = Region (doc, 5, lineB);
    const auto rootOf = [&] (const ed::Document& d, ed::Id id) {
        return std::find_if (d.landscape->objects.begin (), d.landscape->objects.end (),
                             [&] (const geo::HardscapeObject& o) { return o.id == id; })
            ->footprintOperation;
    };
    // A plain region's root selects its boundary line directly; the Footprint steps list offers its
    // switch (the recipe row hides it: it would not change the footprint).
    const auto aRoot = rootOf (doc, a);
    const auto& op = *std::find_if (doc.landscape->operations.begin (), doc.landscape->operations.end (),
                                    [&] (const geo::LandscapeOperation& o) { return o.id == aRoot; });
    const bool readsLine = std::none_of (doc.landscape->operations.begin (), doc.landscape->operations.end (),
                                         [&] (const geo::LandscapeOperation& o) { return o.id == op.inputs.front (); });
    geo::DomainCommand command;
    command.kind = geo::OperationKind::Difference;
    command.owner = b;
    command.subject.input = rootOf (doc, b);
    command.operand.input = aRoot;
    const auto applied = geo::ApplyDomainCommand (doc, command);
    ASSERT_TRUE (applied.ok) << applied.error;
    RecipeDrive d (std::move (doc));
    ASSERT_TRUE (readsLine);
    const auto reason = eh::RecipeBypassRefusal (d.session, aRoot);
    EXPECT_NE (reason.find ("reads its line directly"), std::string::npos) << reason;
    EXPECT_TRUE (eh::RecipeBypassRefusal (d.session, applied.operationId).empty ())
        << "a Boolean result's bypass builds";
    ASSERT_TRUE (d.session.edits.Apply ([&] (ed::Document& c, std::string& e) {
        return geo::SetLandscapeOperationEnabled (c, applied.operationId, false, e);
    }));
    EXPECT_TRUE (eh::RecipeBypassRefusal (d.session, applied.operationId).empty ()) << "turning it back on";
    d.Frame (); // draws the disabled or enabled switch without editing anything
    EXPECT_EQ (d.session.edits.UndoDepth (), 1u);
}

// #105 live request (2026-11-13): drag a region row onto another to move it there. One undo step,
// the arrows' validation; the rows between shift one place.
TEST (HudVirtualEditorRecipe, DraggingARowOntoAnotherMovesItThereAsOneUndo105)
{
    ed::Document doc;
    std::string error;
    ASSERT_TRUE (geo::StartLandscape (doc, { 1, 2, 3, 4 }, error)) << error;
    ed::Id lineA = 0, lineB = 0, lineC = 0;
    const auto a = Region (doc, 0, lineA);
    const auto b = Region (doc, 5, lineB);
    const auto c = Region (doc, 8, lineC);
    RecipeDrive d (std::move (doc));
    const auto order = [&] {
        std::vector<ed::Id> out;
        for (const auto& row : eh::CurrentRecipe (d.session).rows)
            if (row.stage == geo::RecipeStage::Region)
                out.push_back (row.object);
        return out;
    };
    ASSERT_EQ (order (), (std::vector<ed::Id> { a, b, c }));
    // The rows' selectables span the panel; nothing else is hoverable this far right.
    std::vector<float> rows;
    ImGuiID last = 0;
    for (float y = 30; y < 770; y += 2) {
        d.Frame ({ 450, y });
        const ImGuiID id = d.context->HoveredId;
        if (id != 0 && id != last)
            rows.push_back (y);
        last = id;
    }
    ASSERT_GE (rows.size (), 3u);
    const ImVec2 from { 450, rows[0] + 2 }, onto { 450, rows[2] + 2 };
    d.Frame (from);
    d.Frame (from, true);
    for (int i = 1; i <= 8; ++i)
        d.Frame ({ from.x, from.y + (onto.y - from.y) * float (i) / 8.0f }, true);
    d.Frame (onto, true);
    d.Frame (onto, false);
    d.Frame (onto, false);
    EXPECT_EQ (order (), (std::vector<ed::Id> { b, c, a })) << d.session.message;
    EXPECT_EQ (d.session.edits.UndoDepth (), 1u);
    ASSERT_TRUE (d.session.edits.Undo ());
    EXPECT_EQ (order (), (std::vector<ed::Id> { a, b, c }));
    // A refused drop changes nothing and says why.
    EXPECT_FALSE (eh::DropRecipeRow (d.session, a, ed::Id (9999)));
    EXPECT_NE (d.session.message.find ("Drop it on a region"), std::string::npos) << d.session.message;
    EXPECT_EQ (order (), (std::vector<ed::Id> { a, b, c }));
}
