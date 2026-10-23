#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditPathEdit.hpp"
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

namespace {
namespace eh = geomsrv::archviz::edithud;
namespace edit = geomsrv::edit;
namespace lg = tapioca::geometry;

edit::Document EmptyLandscape ()
{
    edit::Document doc;
    std::string error;
    EXPECT_TRUE (lg::StartLandscape (doc, { 1 }, error)) << error;
    return doc;
}

edit::Id AddDesignPath (edit::Document& doc, double y = 0)
{
    const auto path = edit::AddPath (doc, { { 0, y, 10 }, { 20, y, 10 } }, 2);
    lg::LandscapeSource source;
    source.pathId = path;
    source.role = lg::SourceRole::PathCenterline;
    edit::Id id = 0;
    std::string error;
    EXPECT_TRUE (lg::AddLandscapeSource (doc, source, id, error)) << error;
    return path;
}

std::string PanelText (eh::Session& session)
{
    auto* previous = ImGui::GetCurrentContext ();
    auto* context = ImGui::CreateContext ();
    const auto cleanup = [previous] (ImGuiContext* value) {
        ImGui::DestroyContext (value);
        ImGui::SetCurrentContext (previous);
    };
    std::unique_ptr<ImGuiContext, decltype (cleanup)> held (context, cleanup);
    auto& io = ImGui::GetIO ();
    io.IniFilename = nullptr;
    io.DisplaySize = { 900, 1400 };
    io.DeltaTime = 1.0f / 60;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    ImGui::NewFrame ();
    ImGui::SetNextWindowSize ({ 700, 1300 });
    ImGui::Begin ("terrain setup", nullptr, ImGuiWindowFlags_NoSavedSettings);
    ImGui::LogToBuffer ();
    eh::TerrainDesignPanel (session);
    const std::string text = context->LogBuffer.c_str ();
    ImGui::LogFinish ();
    ImGui::End ();
    ImGui::Render ();
    return text;
}
} // namespace

TEST (HudTerrainSetup, FreshLandscapeOffersCreationInsteadOfAnEmptySelectorAndDisabledDeadEnd)
{
    eh::Session session (EmptyLandscape ());
    const auto text = PanelText (session);
    EXPECT_NE (text.find ("Draw design path"), std::string::npos) << text;
    EXPECT_NE (text.find ("Draw property line"), std::string::npos) << text;
    EXPECT_EQ (text.find ("Choose a design path"), std::string::npos) << text;
    EXPECT_EQ (text.find ("Capture selected Mesh"), std::string::npos) << text;
    EXPECT_TRUE (session.edits.Doc ().paths.empty ());
    EXPECT_EQ (session.edits.UndoDepth (), 0U);
}

TEST (HudTerrainSetup, DrawActionsUseTheExistingToolsAndCompletedPathBecomesTheChoice)
{
    eh::Session session (EmptyLandscape ());
    ASSERT_TRUE (eh::DrawTerrainDesignPath (session)) << session.message;
    EXPECT_EQ (session.tool, eh::Tool::Path);
    EXPECT_EQ (session.landscapeMode, eh::LandscapeMode::DrawSource);
    EXPECT_EQ (session.edits.UndoDepth (), 0U);
    session.drawing = { { 0, 0, 10 }, { 20, 0, 10 } };
    eh::detail::FinishDrawing (session);
    ASSERT_EQ (session.edits.Doc ().paths.size (), 1U);
    const auto path = session.edits.Doc ().paths.front ().id;
    EXPECT_EQ (session.terrainPath, path);
    EXPECT_EQ (eh::TerrainDesignPaths (session), (std::vector<edit::Id> { path }));
    EXPECT_EQ (session.edits.UndoDepth (), 1U);
    ASSERT_TRUE (eh::EditTerrainDesignPath (session)) << session.message;
    EXPECT_EQ (session.tool, eh::Tool::Select);
    EXPECT_EQ (session.profilePath, path);
    EXPECT_EQ (session.edits.UndoDepth (), 1U); // Opening controls is not an authored edit.
    ASSERT_TRUE (eh::DrawTerrainPropertyLine (session)) << session.message;
    EXPECT_EQ (session.tool, eh::Tool::Polyline);
    EXPECT_EQ (session.landscapeRole, lg::SourceRole::PropertyLine);
    session.drawing = { { -5, -5, 0 }, { 25, -5, 0 }, { 25, 5, 0 }, { -5, 5, 0 }, { -5, -5, 0 } };
    eh::detail::FinishDrawing (session);
    EXPECT_NE (session.edits.Doc ().landscape->propertyLineSourceId, 0U);
    EXPECT_EQ (session.edits.UndoDepth (), 2U);
    ASSERT_TRUE (session.edits.Undo ());
    EXPECT_EQ (session.edits.Doc ().landscape->propertyLineSourceId, 0U);
    ASSERT_TRUE (session.edits.Redo ());
    EXPECT_NE (session.edits.Doc ().landscape->propertyLineSourceId, 0U);
}

TEST (HudTerrainSetup, OnlyLiveAuthoredPathsAreListedAndInitialChoiceFollowsSelectionOrTheOnlyPath)
{
    auto doc = EmptyLandscape ();
    edit::AddPath (doc, { { 0, 30, 10 }, { 20, 30, 10 } }, 2); // Unbound, not a design source.
    const auto first = AddDesignPath (doc);
    eh::Session sole (doc);
    EXPECT_EQ (eh::TerrainDesignPaths (sole), (std::vector<edit::Id> { first }));
    EXPECT_EQ (eh::ResolveTerrainDesignPath (sole), first);
    const auto second = AddDesignPath (doc, 10);
    eh::Session session (doc);
    EXPECT_EQ (eh::ResolveTerrainDesignPath (session), 0U); // Ambiguous: never just pick the first item.
    session.selection.Set ({ { second, edit::Part::Entity, 0 } });
    EXPECT_EQ (eh::ResolveTerrainDesignPath (session), second);
    EXPECT_EQ (session.edits.UndoDepth (), 0U);
    ASSERT_TRUE (eh::ChooseTerrainDesignPath (session, first));
    EXPECT_EQ (eh::ResolveTerrainDesignPath (session), first); // Explicit choice survives unrelated selection.
    session.terrainPath = doc.nextId;
    EXPECT_EQ (eh::ResolveTerrainDesignPath (session), 0U); // A stale choice is not retargeted.
    ASSERT_TRUE (eh::ChooseTerrainDesignPath (session, second));
    ASSERT_TRUE (session.edits.Apply ([=] (edit::Document& value, std::string&) {
        for (auto& source : value.landscape->sources)
            if (source.pathId == second)
                source.deleted = true;
        return true;
    }));
    EXPECT_EQ (eh::TerrainDesignPaths (session), (std::vector<edit::Id> { first }));
    EXPECT_EQ (eh::ResolveTerrainDesignPath (session), 0U);
    EXPECT_FALSE (eh::ChooseTerrainDesignPath (session, second));
}

TEST (HudTerrainSetup, PathEditingIsAvailableBeforeAPropertyLineAndSetupRefusesActiveInput)
{
    auto doc = EmptyLandscape ();
    const auto path = AddDesignPath (doc);
    eh::Session session (doc);
    const auto text = PanelText (session);
    EXPECT_NE (text.find ("Path " + std::to_string (path)), std::string::npos) << text;
    EXPECT_NE (text.find ("Edit design path"), std::string::npos) << text;
    EXPECT_NE (text.find ("Draw property line"), std::string::npos) << text;
    EXPECT_EQ (text.find ("Capture selected Mesh"), std::string::npos) << text;
    ASSERT_TRUE (eh::EditTerrainDesignPath (session));
    EXPECT_EQ (session.selection.Items (), (std::vector<edit::Ref> { { path, edit::Part::Entity, 0 } }));
    EXPECT_EQ (session.edits.UndoDepth (), 0U);
    ASSERT_TRUE (session.edits.Begin ());
    EXPECT_FALSE (eh::DrawTerrainDesignPath (session));
    EXPECT_FALSE (eh::DrawTerrainPropertyLine (session));
    EXPECT_FALSE (eh::ChooseTerrainDesignPath (session, path));
    session.edits.Cancel ();
    session.drawing = { { 0, 0, 10 } };
    EXPECT_FALSE (eh::DrawTerrainPropertyLine (session));
    EXPECT_EQ (session.drawing.size (), 1U);
}
