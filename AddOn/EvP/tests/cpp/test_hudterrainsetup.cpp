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

struct PanelGui {
    ImGuiContext* previous = ImGui::GetCurrentContext ();
    ImGuiContext* context = ImGui::CreateContext ();
    PanelGui ()
    {
        auto& io = ImGui::GetIO ();
        io.IniFilename = nullptr;
        io.DisplaySize = { 900, 1400 };
        io.DeltaTime = 1.0f / 60;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    }
    ~PanelGui ()
    {
        ImGui::DestroyContext (context);
        ImGui::SetCurrentContext (previous);
    }
    std::string Frame (eh::Session& session, ImVec2 pointer = { 850, 1350 }, bool down = false, bool landscape = false,
                       int region = 0)
    {
        auto& io = ImGui::GetIO ();
        io.AddMousePosEvent (pointer.x, pointer.y);
        io.AddMouseButtonEvent (0, down);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 700, 1300 });
        ImGui::Begin ("terrain setup", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer ();
        if (region)
            eh::RegionPanel (session, region == 2);
        else if (landscape)
            eh::LandscapePanel (session);
        else
            eh::TerrainDesignPanel (session);
        const std::string text = context->LogBuffer.c_str ();
        ImGui::LogFinish ();
        ImGui::End ();
        ImGui::Render ();
        return text;
    }
    bool ClickSectionControl (eh::Session& session, const char* label)
    {
        Frame (session);
        auto* window = ImGui::FindWindowByName ("terrain setup");
        const auto section = ImHashStr ("Terrain cross-section", 0, window->ID);
        const auto target = ImHashStr (label, 0, section);
        for (float y = 24; y < 1250; y += 4) {
            const ImVec2 point { 40, y };
            Frame (session, point);
            if (context->HoveredId != target)
                continue;
            Frame (session, point, true);
            Frame (session, point, false);
            return true;
        }
        return false;
    }
};

std::string PanelText (eh::Session& session)
{
    PanelGui gui;
    return gui.Frame (session);
}
} // namespace

TEST (HudTerrainSetup, LandscapeSourceRoleControlsDoNotShareImGuiIds)
{
    eh::Session session (EmptyLandscape ());
    PanelGui gui;
    gui.Frame (session, { 850, 1350 }, false, true);
    // Exercise the actual submitted widgets: ImGui counts duplicate IDs against
    // the previous frame's hovered item, including a disabled selection control.
    size_t hovered = 0;
    for (float y = 24; y < 1250; y += 8)
        for (float x : { 30.0f, 130.0f, 240.0f }) {
            const ImVec2 point { x, y };
            gui.Frame (session, point, false, true);
            if (!gui.context->HoveredId)
                continue;
            ++hovered;
            gui.Frame (session, point, false, true);
            EXPECT_LE (gui.context->HoveredIdPreviousFrameItemCount, 1) << "at " << x << ", " << y;
        }
    EXPECT_GT (hovered, 0u);
}

TEST (HudTerrainSetup, RegionSurfacePanelNamesContourDirectionAndHidesForReferenceSelection)
{
    auto doc = EmptyLandscape ();
    const auto region = edit::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 } }, true);
    const auto reference = edit::AddPolyline (doc, { { 20, 0, 0 }, { 30, 0, 0 } }, false);
    std::string error;
    ASSERT_TRUE (lg::SetLandscapeSourceRole (doc, region, lg::SourceRole::RegionBoundary, error));
    ASSERT_TRUE (lg::SetLandscapeSourceRole (doc, reference, lg::SourceRole::ReferenceLine, error));
    const auto object = doc.landscape->objects[0].id;
    auto surface = *doc.landscape->objects[0].surface;
    surface.elevation = lg::RegionElevationMode::Constant;
    surface.slope = lg::RegionSlopeMode::ContourLine;
    lg::MaterialSpec material;
    material.fillGuid = "00112233-4455-6677-8899-AABBCCDDEEFF";
    ASSERT_TRUE (lg::SetRegionSurface (doc, object, surface, material, error));
    eh::Session session (doc);
    session.selection.Set ({ { region, edit::Part::Entity, 0 } });
    PanelGui gui;
    const auto text = gui.Frame (session, { 850, 1350 }, false, false, 1);
    EXPECT_NE (text.find ("Fall side"), std::string::npos);
    EXPECT_NE (text.find ("perpendicular"), std::string::npos);
    EXPECT_NE (text.find ("not applied yet"), std::string::npos);
    EXPECT_EQ (session.landscapeObject, object);
    const auto materialText = gui.Frame (session, { 850, 1350 }, false, false, 2);
    EXPECT_NE (materialText.find ("#26"), std::string::npos);
    EXPECT_NE (materialText.find ("Material preset metadata"), std::string::npos);
    EXPECT_NE (materialText.find (material.fillGuid), std::string::npos) << "A stored GUID is not 'not assigned'.";
    session.selection.Set ({ { reference, edit::Part::Entity, 0 } });
    const auto refText = gui.Frame (session, { 850, 1350 }, false, false, 1);
    EXPECT_EQ (refText.find ("Elevation"), std::string::npos);
    const auto refMaterial = gui.Frame (session, { 850, 1350 }, false, false, 2);
    EXPECT_EQ (refMaterial.find ("Material preset metadata"), std::string::npos);
    session.selection.Clear ();
    const auto cleared = gui.Frame (session, { 850, 1350 }, false, false, 1);
    EXPECT_EQ (cleared.find ("Elevation"), std::string::npos)
        << "A remembered boolean owner is not a property-panel selection.";
    EXPECT_EQ (session.edits.UndoDepth (), 0u);
}

TEST (HudTerrainSetup, RegionSlopeCanTakeTwoSelectedVerticesInOneUndoableClick)
{
    auto doc = EmptyLandscape ();
    const auto line = edit::AddPolyline (doc, { { 1, 2, 10 }, { 11, 2, 10.5 }, { 11, 12, 10 } }, true);
    std::string error;
    ASSERT_TRUE (lg::SetLandscapeSourceRole (doc, line, lg::SourceRole::RegionBoundary, error));
    const auto object = doc.landscape->objects[0].id;
    auto surface = *doc.landscape->objects[0].surface;
    surface.elevation = lg::RegionElevationMode::Constant;
    surface.slope = lg::RegionSlopeMode::TwoPoints;
    ASSERT_TRUE (lg::SetRegionSurface (doc, object, surface, {}, error));
    eh::Session session (doc);
    session.selection.Set ({ { line, edit::Part::Vertex, doc.entities[0].vertices[0].id },
                             { line, edit::Part::Vertex, doc.entities[0].vertices[1].id } });
    PanelGui gui;
    gui.Frame (session, { 850, 1350 }, false, false, 1);
    const auto* window = ImGui::FindWindowByName ("terrain setup");
    const auto scope = ImHashStr ("region-surface", 0, window->ID);
    const auto owner = ImHashStr (std::to_string (object).c_str (), 0, scope);
    const auto target = ImHashStr ("Use two selected vertices", 0, owner);
    bool clicked = false;
    for (float y = 24; y < 1250; y += 4) {
        const ImVec2 pointer { 40, y };
        gui.Frame (session, pointer, false, false, 1);
        if (gui.context->HoveredId != target)
            continue;
        gui.Frame (session, pointer, true, false, 1);
        gui.Frame (session, pointer, false, false, 1);
        clicked = true;
        break;
    }
    ASSERT_TRUE (clicked);
    const auto& after = *session.edits.Doc ().landscape->objects[0].surface;
    EXPECT_EQ (after.first, doc.entities[0].vertices[0].p);
    EXPECT_EQ (after.second, doc.entities[0].vertices[1].p);
    EXPECT_EQ (session.edits.Doc ().entities, doc.entities);
    EXPECT_EQ (session.edits.UndoDepth (), 1u);
    ASSERT_TRUE (session.edits.Undo ());
    EXPECT_EQ (session.edits.Doc ().landscape, doc.landscape);
}

TEST (HudTerrainSetup, FreshLandscapeOffersCreationInsteadOfAnEmptySelectorAndDisabledDeadEnd)
{
    eh::Session session (EmptyLandscape ());
    const auto text = PanelText (session);
    EXPECT_NE (text.find ("Draw design path"), std::string::npos) << text;
    EXPECT_NE (text.find ("Draw property line"), std::string::npos) << text;
    EXPECT_NE (text.find ("topography boundary"), std::string::npos) << text;
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

TEST (HudTerrainSetup, LiveExistingPathsResolveWithoutDrawingAndExplicitChoiceIsPreserved)
{
    auto doc = EmptyLandscape ();
    edit::AddPath (doc, { { 0, 30, 10 }, { 20, 30, 10 } }, 2); // Unbound, not a design source.
    const auto first = AddDesignPath (doc);
    eh::Session sole (doc);
    EXPECT_EQ (eh::TerrainDesignPaths (sole), (std::vector<edit::Id> { first }));
    EXPECT_EQ (eh::ResolveTerrainDesignPath (sole), first);
    const auto second = AddDesignPath (doc, 10);
    eh::Session session (doc);
    EXPECT_EQ (eh::ResolveTerrainDesignPath (session), first); // Existing networks need no throwaway new path.
    EXPECT_NE (PanelText (session).find ("Edit design path"), std::string::npos);
    EXPECT_TRUE (session.drawing.empty ());
    EXPECT_EQ (session.edits.UndoDepth (), 0U);
    session.terrainPath = 0; // A selection made before the initial resolution wins over the fallback.
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
    for (int mutation = 0; mutation < 11; ++mutation) {
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
            case 7:
                value.taper = true;
                value.endLeft = -1;
                break;
            case 8:
                value.taper = true;
                value.endRight = std::numeric_limits<double>::quiet_NaN ();
                break;
            case 9:
                value.taper = true;
                value.endLeft = value.endRight = 0;
                break;
            case 10:
                value.taper = true;
                value.endLeft = std::numeric_limits<double>::infinity ();
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
    for (const char* label : { "Left width", "Right width", "Taper widths", "Crossfall", "Shoulder", "Cut slope",
                               "Fill slope", "Rounding", "Reload section values" })
        EXPECT_NE (text.find (label), std::string::npos) << label << "\n" << text;
    EXPECT_EQ (session.edits.UndoDepth (), 0U);
}

TEST (HudTerrainSetup, TaperCheckboxSeedsCurrentWidthsAndResetsEndsInOneUndoableEdit)
{
    auto doc = EmptyLandscape ();
    const auto path = AddDesignPath (doc);
    const auto range = edit::FindPath (doc, path)->sections.front ().id;
    std::string error;
    ASSERT_TRUE (edit::SetPathWidth (doc, path, range, 2.5, 1.5, error)) << error;
    eh::Session session (doc);
    PanelGui gui;
    ASSERT_TRUE (gui.ClickSectionControl (session, "Taper widths"));
    auto section = edit::FindPath (session.edits.Doc (), path)->sections.front ();
    EXPECT_TRUE (section.taper);
    EXPECT_EQ (section.endLeft, 2.5);
    EXPECT_EQ (section.endRight, 1.5);
    EXPECT_EQ (session.edits.UndoDepth (), 1U);
    const auto text = gui.Frame (session);
    EXPECT_NE (text.find ("Left end width"), std::string::npos) << text;
    EXPECT_NE (text.find ("Right end width"), std::string::npos) << text;
    session.terrainSectionDraft->value.endLeft = 4;
    session.terrainSectionDraft->value.endRight = .5;
    ASSERT_TRUE (eh::ApplyTerrainSectionEdit (session)) << session.message;
    ASSERT_TRUE (gui.ClickSectionControl (session, "Taper widths"));
    section = edit::FindPath (session.edits.Doc (), path)->sections.front ();
    EXPECT_FALSE (section.taper);
    EXPECT_EQ (section.endLeft, section.widthLeft);
    EXPECT_EQ (section.endRight, section.widthRight);
    EXPECT_EQ (session.edits.UndoDepth (), 3U);
    EXPECT_EQ (gui.Frame (session).find ("Left end width"), std::string::npos);
    ASSERT_TRUE (session.edits.Undo ());
    section = edit::FindPath (session.edits.Doc (), path)->sections.front ();
    EXPECT_TRUE (section.taper);
    EXPECT_EQ (section.endLeft, 4);
    EXPECT_EQ (section.endRight, .5);
    ASSERT_TRUE (session.edits.Redo ());
    EXPECT_FALSE (edit::FindPath (session.edits.Doc (), path)->sections.front ().taper);
}

TEST (HudTerrainSetup, TaperEndWidthsDragLiveAndPersistWithoutChangingTheStartWidths)
{
    auto doc = EmptyLandscape ();
    const auto path = AddDesignPath (doc);
    const auto range = edit::FindPath (doc, path)->sections.front ().id;
    std::string error;
    ASSERT_TRUE (edit::SetPathTaper (doc, path, range, true, 1, 1, error)) << error;
    const auto before = edit::FindPath (doc, path)->sections.front ();
    eh::Session session (doc);
    ASSERT_TRUE (eh::BeginTerrainSectionEdit (session, range));
    ASSERT_TRUE (eh::BeginTerrainSectionDrag (session));
    for (const double width : { 1.5, 2.5, 3.5 }) {
        session.terrainSectionDraft->value.endLeft = width;
        session.terrainSectionDraft->value.endRight = .5;
        ASSERT_TRUE (eh::MoveTerrainSectionDrag (session)) << session.message;
        const auto& section = edit::FindPath (session.edits.Doc (), path)->sections.front ();
        EXPECT_EQ (section.endLeft, width);
        EXPECT_EQ (section.endRight, .5);
        EXPECT_EQ (section.widthLeft, before.widthLeft);
        EXPECT_EQ (section.widthRight, before.widthRight);
        EXPECT_EQ (session.edits.UndoDepth (), 0U);
    }
    eh::EndTerrainSectionDrag (session);
    EXPECT_EQ (session.edits.UndoDepth (), 1U);
    const auto expected = edit::FindPath (session.edits.Doc (), path)->sections.front ();
    std::string text;
    ASSERT_TRUE (lg::WriteLandscape (session.edits.Doc (), lg::LandscapeStorage::Virtual, text, error)) << error;
    edit::Document loaded;
    ASSERT_TRUE (lg::ReadLandscape (text, loaded, error)) << error;
    EXPECT_EQ (edit::FindPath (loaded, path)->sections.front (), expected);
    ASSERT_TRUE (session.edits.Undo ());
    EXPECT_EQ (edit::FindPath (session.edits.Doc (), path)->sections.front (), before);
    EXPECT_FALSE (eh::ApplyTerrainSectionEdit (session));
    EXPECT_NE (session.message.find ("TerrainSectionStale:"), std::string::npos);
    ASSERT_TRUE (session.edits.Redo ());
    EXPECT_EQ (edit::FindPath (session.edits.Doc (), path)->sections.front (), expected);
}
