#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditPathEdit.hpp"
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <limits>

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

TEST (HudTerrainSetup, SectionFieldsApplyTogetherInOneHistoryEntryAndPersistInTheExistingPathCodec)
{
    auto doc = EmptyLandscape ();
    const auto path = AddDesignPath (doc);
    const auto before = edit::FindPath (doc, path)->sections.front ();
    eh::Session session (doc);
    ASSERT_TRUE (eh::BeginTerrainSectionEdit (session, before.id));
    auto& value = session.terrainSectionDraft->value;
    value.widthLeft = 2;
    value.widthRight = 3;
    value.crossfall = 3;
    value.fall = edit::Fall::Right;
    value.shoulder = .25;
    value.cutSlope = 60;
    value.fillSlope = 40;
    const auto expected = value;
    EXPECT_EQ (edit::FindPath (session.edits.Doc (), path)->sections.front (), before);
    EXPECT_EQ (session.edits.UndoDepth (), 0U);
    ASSERT_TRUE (eh::ApplyTerrainSectionEdit (session)) << session.message;
    EXPECT_EQ (edit::FindPath (session.edits.Doc (), path)->sections.front (), expected);
    EXPECT_EQ (session.edits.UndoDepth (), 1U);
    EXPECT_FALSE (eh::ApplyTerrainSectionEdit (session)); // No-op is not an extra history entry.
    EXPECT_EQ (session.edits.UndoDepth (), 1U);
    std::string text, error;
    ASSERT_TRUE (lg::WriteLandscape (session.edits.Doc (), lg::LandscapeStorage::Virtual, text, error)) << error;
    edit::Document loaded;
    ASSERT_TRUE (lg::ReadLandscape (text, loaded, error)) << error;
    EXPECT_EQ (edit::FindPath (loaded, path)->sections.front (), expected);
    ASSERT_TRUE (session.edits.Undo ());
    EXPECT_EQ (edit::FindPath (session.edits.Doc (), path)->sections.front (), before);
    EXPECT_FALSE (eh::ApplyTerrainSectionEdit (session)); // A draft from before Undo is stale.
    EXPECT_NE (session.message.find ("TerrainSectionStale:"), std::string::npos);
    ASSERT_TRUE (session.edits.Redo ());
    EXPECT_EQ (edit::FindPath (session.edits.Doc (), path)->sections.front (), expected);
}

TEST (HudTerrainSetup, ASliderDragAppliesLiveAndItsReleaseIsOneUndoStep)
{
    auto doc = EmptyLandscape ();
    const auto path = AddDesignPath (doc);
    const auto before = edit::FindPath (doc, path)->sections.front ();
    eh::Session session (doc);
    ASSERT_TRUE (eh::BeginTerrainSectionEdit (session, before.id));
    ASSERT_TRUE (eh::BeginTerrainSectionDrag (session)) << session.message;
    EXPECT_FALSE (eh::BeginTerrainSectionDrag (session)) << "one held slider at a time";
    auto& value = session.terrainSectionDraft->value;
    for (const double width : { 1.5, 2.5, 3.5 }) {
        value.widthLeft = width;
        ASSERT_TRUE (eh::MoveTerrainSectionDrag (session)) << session.message;
        // Live: the document follows the slider while it is held, and no undo entry yet.
        EXPECT_EQ (edit::FindPath (session.edits.Doc (), path)->sections.front ().widthLeft, width);
        EXPECT_EQ (session.edits.UndoDepth (), 0U);
        EXPECT_TRUE (session.edits.InGesture ());
    }
    eh::EndTerrainSectionDrag (session);
    EXPECT_FALSE (session.edits.InGesture ());
    EXPECT_FALSE (session.terrainSectionDrag);
    EXPECT_EQ (session.edits.UndoDepth (), 1U);
    EXPECT_EQ (session.terrainSectionDraft->revision, session.edits.Doc ().revision) << "the draft stays current";
    ASSERT_TRUE (session.edits.Undo ());
    EXPECT_EQ (edit::FindPath (session.edits.Doc (), path)->sections.front (), before) << "one step back to the start";
}

TEST (HudTerrainSetup, InvalidOrIdentityChangingSectionDraftsRefuseAtomicallyAndRemainCorrectable)
{
    auto doc = EmptyLandscape ();
    const auto path = AddDesignPath (doc);
    const auto before = edit::FindPath (doc, path)->sections.front ();
    eh::Session session (doc);
    for (int mutation = 0; mutation < 7; ++mutation) {
        ASSERT_TRUE (eh::BeginTerrainSectionEdit (session, before.id));
        auto& value = session.terrainSectionDraft->value;
        switch (mutation) {
            case 0:
                value.widthLeft = -1;
                break;
            case 1:
                value.cutSlope = 0;
                break;
            case 2:
                value.fillSlope = std::numeric_limits<double>::infinity ();
                break;
            case 3:
                value.crossfall = std::numeric_limits<double>::quiet_NaN ();
                break;
            case 4:
                ++value.id;
                break;
            case 5:
                value.station += 1;
                break;
            case 6:
                value.fall = static_cast<edit::Fall> (255);
                break;
        }
        EXPECT_FALSE (eh::ApplyTerrainSectionEdit (session)) << mutation;
        EXPECT_EQ (edit::FindPath (session.edits.Doc (), path)->sections.front (), before);
        EXPECT_EQ (session.edits.Doc ().revision, doc.revision);
        EXPECT_EQ (session.edits.UndoDepth (), 0U);
    }
    ASSERT_TRUE (eh::BeginTerrainSectionEdit (session, before.id));
    session.terrainSectionDraft->value.cutSlope = 65;
    ASSERT_TRUE (eh::ApplyTerrainSectionEdit (session)) << session.message;
    EXPECT_EQ (edit::FindPath (session.edits.Doc (), path)->sections.front ().cutSlope, 65);
}

TEST (HudTerrainSetup, DraftsCannotCrossPathsOverwriteNewerEditsOrApplyDuringInput)
{
    auto doc = EmptyLandscape ();
    const auto first = AddDesignPath (doc);
    const auto second = AddDesignPath (doc, 10);
    const auto section = edit::FindPath (doc, first)->sections.front ().id;
    eh::Session session (doc);
    ASSERT_TRUE (eh::ChooseTerrainDesignPath (session, first));
    ASSERT_TRUE (eh::BeginTerrainSectionEdit (session, section));
    session.terrainSectionDraft->value.cutSlope = 65;
    ASSERT_TRUE (session.edits.Begin ());
    EXPECT_FALSE (eh::ApplyTerrainSectionEdit (session));
    session.edits.Cancel ();
    ASSERT_TRUE (session.edits.Apply ([=] (edit::Document& value, std::string& error) {
        return edit::SetPathWidth (value, first, section, 4, 4, error);
    }));
    EXPECT_FALSE (eh::ApplyTerrainSectionEdit (session));
    EXPECT_NE (session.message.find ("TerrainSectionStale:"), std::string::npos);
    EXPECT_EQ (edit::FindPath (session.edits.Doc (), first)->sections.front ().widthLeft, 4);
    ASSERT_TRUE (eh::BeginTerrainSectionEdit (session, section)); // Explicit reload reads the new values.
    EXPECT_EQ (session.terrainSectionDraft->value.widthLeft, 4);
    ASSERT_TRUE (eh::ChooseTerrainDesignPath (session, second));
    EXPECT_FALSE (session.terrainSectionDraft);
    EXPECT_FALSE (eh::BeginTerrainSectionEdit (session, section));
    EXPECT_EQ (session.edits.UndoDepth (), 1U);
}

TEST (HudTerrainSetup, PanelRendersExplicitSectionParametersInsteadOfOnlyCaptureButtons)
{
    auto doc = EmptyLandscape ();
    AddDesignPath (doc);
    eh::Session session (doc);
    const auto text = PanelText (session);
    for (const char* label : { "Left width", "Right width", "Crossfall", "Shoulder", "Cut slope", "Fill slope",
                               "Rounding", "Reload section values" })
        EXPECT_NE (text.find (label), std::string::npos) << label << "\n" << text;
    EXPECT_EQ (session.edits.UndoDepth (), 0U);
}
