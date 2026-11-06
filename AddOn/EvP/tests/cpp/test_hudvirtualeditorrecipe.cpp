// The Operations tab's recipe (HudVirtualEditorRecipe.hpp, #105 slice 1): drawn without editing
// anything, recompiled only when the document changes, and a row click selects that region's
// boundary line in the view.
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorRecipe.hpp"
#include "Edit/EditLandscape.hpp"
#include <gtest/gtest.h>
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
