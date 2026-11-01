#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditPathEdit.hpp"
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <cmath>

#ifdef TAPIOCA_GEOMETRY_RUST
namespace eh = geomsrv::archviz::edithud;
namespace edit = geomsrv::edit;
namespace lg = tapioca::geometry;
namespace {
struct PathGui {
    ImGuiContext* previous = ImGui::GetCurrentContext ();
    ImGuiContext* context = ImGui::CreateContext ();
    PathGui ()
    {
        auto& io = ImGui::GetIO ();
        io.IniFilename = nullptr;
        io.DisplaySize = { 900, 1400 };
        io.DeltaTime = 1.0f / 60;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    }
    ~PathGui ()
    {
        ImGui::DestroyContext (context);
        ImGui::SetCurrentContext (previous);
    }
    void Frame (eh::Session& session, ImVec2 pointer = { 850, 1350 }, bool down = false)
    {
        auto& io = ImGui::GetIO ();
        io.AddMousePosEvent (pointer.x, pointer.y);
        io.AddMouseButtonEvent (0, down);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 700, 1300 });
        ImGui::Begin ("path slope", nullptr, ImGuiWindowFlags_NoSavedSettings);
        eh::TerrainDesignPanel (session);
        ImGui::End ();
        ImGui::Render ();
    }
    ImVec2 Locate (eh::Session& session, ImGuiID target)
    {
        for (float y = 24; y < 1250; y += 2) {
            Frame (session, { 60, y });
            if (context->HoveredId == target)
                return { 60, y + 2 };
        }
        return { -1, -1 };
    }
};
} // namespace

namespace {
void CheckPathSlopeFields (bool fill)
{
    using Unit = lg::TerrainSlope::Unit;
    const auto member = fill ? &edit::Section::fillSlope : &edit::Section::cutSlope;
    for (int unitIndex : { 1, 3 }) {
        edit::Document doc;
        std::string error;
        ASSERT_TRUE (lg::StartLandscape (doc, { 1 }, error));
        const auto path = edit::AddPath (doc, { { 0, 0, 10 }, { 20, 0, 11 } }, 2);
        const auto other = edit::AddPath (doc, { { 0, 10, 10 }, { 20, 10, 11 } }, 2);
        lg::LandscapeSource source;
        source.pathId = path;
        source.role = lg::SourceRole::PathCenterline;
        lg::Id id = 0;
        ASSERT_TRUE (lg::AddLandscapeSource (doc, source, id, error));
        ASSERT_TRUE (edit::EditPath (
            doc, path,
            [fill] (auto& value, auto&) {
                if (fill)
                    value.sections[0].cutSlope = { Unit::RiseRun, 1.5, 3 };
                else
                    value.sections[0].fillSlope = { Unit::Height, 2.1, 5 };
                value.sections[0].crossfall = edit::PercentCrossfall (3);
                value.sections[0].fall = edit::Fall::Right;
                value.sections[0].shoulder = 0.25;
                return true;
            },
            error))
            << error;
        const auto original = edit::FindPath (doc, path)->sections[0];
        eh::Session session (doc);
        ASSERT_TRUE (eh::ChooseTerrainDesignPath (session, path));
        PathGui gui;
        gui.Frame (session);
        const auto* window = ImGui::FindWindowByName ("path slope");
        const auto section = ImHashStr ("Terrain cross-section", 0, window->ID);
        const auto tree = gui.Locate (session, section);
        ASSERT_GT (tree.y, 0);
        gui.Frame (session, tree, true);
        gui.Frame (session, tree, false);
        const auto combo = gui.Locate (session, ImHashStr (fill ? "Fill slope unit" : "Cut slope unit", 0, section));
        ASSERT_GT (combo.y, 0);
        gui.Frame (session, combo, true);
        gui.Frame (session, combo, false);
        gui.Frame (session, combo, false);
        auto* popup = ImGui::FindWindowByName ("##Combo_00");
        ASSERT_NE (popup, nullptr);
        const char* choiceLabel = unitIndex == 1 ? "Angle (degrees)" : "Height / run (m)";
        const auto target = ImHashStr (choiceLabel, 0, ImHashData (&unitIndex, sizeof (unitIndex), popup->ID));
        ImVec2 choice { -1, -1 };
        for (float y = popup->Pos.y + 4; y < popup->Pos.y + popup->Size.y; y += 2) {
            const ImVec2 pointer { popup->Pos.x + 30, y };
            gui.Frame (session, pointer);
            if (gui.context->HoveredId == target) {
                choice = pointer;
                break;
            }
        }
        ASSERT_GT (choice.y, 0);
        gui.Frame (session, choice, true);
        gui.Frame (session, choice, false);
        const auto switched = edit::FindPath (session.edits.Doc (), path)->sections[0];
        const auto& switchedSlope = switched.*member;
        const double grade = fill ? 0.33 : 0.5;
        EXPECT_EQ (switchedSlope.unit, unitIndex == 1 ? Unit::Angle : Unit::Height);
        EXPECT_NEAR (switchedSlope.a, unitIndex == 1 ? std::atan (grade) : grade, 1e-12);
        EXPECT_EQ (switchedSlope.b, unitIndex == 1 ? 0 : 1);
        EXPECT_EQ (session.edits.UndoDepth (), 1u);
        auto unchanged = switched;
        unchanged.*member = original.*member;
        EXPECT_EQ (unchanged, original);
        const char* sliderLabel = fill ? (unitIndex == 1 ? "Fill slope (degrees)" : "Fill height (m)")
                                       : (unitIndex == 1 ? "Cut slope (degrees)" : "Cut height (m)");
        const auto slider = gui.Locate (session, ImHashStr (sliderLabel, 0, section));
        ASSERT_GT (slider.y, 0);
        gui.Frame (session, slider, true);
        ASSERT_TRUE (session.edits.InGesture ());
        EXPECT_DOUBLE_EQ (session.terrainSectionSliderMax, unitIndex == 1 ? 90 : 10);
        for (int move = 0; move < 6; ++move) {
            gui.Frame (session, { unitIndex == 1 ? 240.0f : 400.0f, slider.y }, true);
            EXPECT_EQ (edit::FindPath (session.edits.Doc (), path)->sections[0].*member,
                       session.terrainSectionDraft->value.*member)
                << "apply after degree-to-radian writeback";
            EXPECT_DOUBLE_EQ (session.terrainSectionSliderMax, unitIndex == 1 ? 90 : 10);
            EXPECT_EQ (session.edits.UndoDepth (), 1u);
        }
        gui.Frame (session, { 400, slider.y }, false);
        EXPECT_FALSE (session.edits.InGesture ());
        EXPECT_EQ (session.terrainSectionSliderId, 0u);
        EXPECT_EQ (session.edits.UndoDepth (), 2u);
        const auto edited = edit::FindPath (session.edits.Doc (), path)->sections[0];
        EXPECT_NE ((edited.*member).a, switchedSlope.a);
        if (unitIndex == 3)
            EXPECT_LE ((edited.*member).a, 10);
        unchanged = edited;
        unchanged.*member = original.*member;
        EXPECT_EQ (unchanged, original);
        EXPECT_EQ (*edit::FindPath (session.edits.Doc (), other), *edit::FindPath (doc, other));
        std::string json;
        ASSERT_TRUE (lg::WriteLandscape (session.edits.Doc (), lg::LandscapeStorage::Virtual, json, error)) << error;
        edit::Document loaded;
        ASSERT_TRUE (lg::ReadLandscape (json, loaded, error)) << error;
        EXPECT_EQ (loaded.paths, session.edits.Doc ().paths);
        ASSERT_TRUE (session.edits.Undo ());
        EXPECT_EQ (edit::FindPath (session.edits.Doc (), path)->sections[0], switched);
        ASSERT_TRUE (session.edits.Undo ());
        EXPECT_EQ (session.edits.Doc ().paths, doc.paths);
    }
}
} // namespace

TEST (HudTerrainSetup, PathCrossfallUnitsAndActualSliderKeepSignFallHistoryAndIndependentFaces)
{
    using Unit = lg::TerrainSlope::Unit;
    for (int unitIndex : { 1, 3 }) {
        edit::Document doc;
        std::string error;
        ASSERT_TRUE (lg::StartLandscape (doc, { 1 }, error));
        const auto path = edit::AddPath (doc, { { 0, 0, 10 }, { 20, 0, 11 } }, 2);
        const auto other = edit::AddPath (doc, { { 0, 10, 10 }, { 20, 10, 11 } }, 2);
        lg::LandscapeSource source;
        source.pathId = path;
        source.role = lg::SourceRole::PathCenterline;
        lg::Id id = 0;
        ASSERT_TRUE (lg::AddLandscapeSource (doc, source, id, error));
        ASSERT_TRUE (edit::EditPath (
            doc, path,
            [] (auto& value, auto&) {
                value.sections[0].crossfall = { { Unit::Percent, 3, 0 }, true };
                value.sections[0].fall = edit::Fall::Crown;
                value.sections[0].cutSlope = { Unit::RiseRun, 1.5, 3 };
                value.sections[0].fillSlope = { Unit::Height, 2.1, 5 };
                value.sections[0].shoulder = .25;
                return true;
            },
            error))
            << error;
        const auto original = edit::FindPath (doc, path)->sections[0];
        eh::Session session (doc);
        ASSERT_TRUE (eh::ChooseTerrainDesignPath (session, path));
        PathGui gui;
        gui.Frame (session);
        const auto* window = ImGui::FindWindowByName ("path slope");
        const auto section = ImHashStr ("Terrain cross-section", 0, window->ID);
        const auto tree = gui.Locate (session, section);
        ASSERT_GT (tree.y, 0);
        gui.Frame (session, tree, true);
        gui.Frame (session, tree, false);
        const auto combo = gui.Locate (session, ImHashStr ("Crossfall unit", 0, section));
        ASSERT_GT (combo.y, 0);
        gui.Frame (session, combo, true);
        gui.Frame (session, combo, false);
        gui.Frame (session, combo, false);
        auto* popup = ImGui::FindWindowByName ("##Combo_00");
        ASSERT_NE (popup, nullptr);
        const char* choiceLabel = unitIndex == 1 ? "Angle (degrees)" : "Height / run (m)";
        const auto target = ImHashStr (choiceLabel, 0, ImHashData (&unitIndex, sizeof (unitIndex), popup->ID));
        ImVec2 choice { -1, -1 };
        for (float y = popup->Pos.y + 4; y < popup->Pos.y + popup->Size.y; y += 2) {
            const ImVec2 pointer { popup->Pos.x + 30, y };
            gui.Frame (session, pointer);
            if (gui.context->HoveredId == target) {
                choice = pointer;
                break;
            }
        }
        ASSERT_GT (choice.y, 0);
        gui.Frame (session, choice, true);
        gui.Frame (session, choice, false);
        const auto switched = edit::FindPath (session.edits.Doc (), path)->sections[0];
        EXPECT_EQ (switched.crossfall.slope.unit, unitIndex == 1 ? Unit::Angle : Unit::Height);
        EXPECT_NEAR (switched.crossfall.slope.a, unitIndex == 1 ? std::atan (.03) : .03, 1e-12);
        EXPECT_EQ (switched.crossfall.slope.b, unitIndex == 1 ? 0 : 1);
        EXPECT_TRUE (switched.crossfall.negative);
        EXPECT_EQ (switched.fall, edit::Fall::Crown);
        EXPECT_EQ (session.edits.UndoDepth (), 1u);
        auto unchanged = switched;
        unchanged.crossfall = original.crossfall;
        EXPECT_EQ (unchanged, original);
        const char* sliderLabel = unitIndex == 1 ? "Crossfall (degrees)" : "Crossfall height (m)";
        const auto slider = gui.Locate (session, ImHashStr (sliderLabel, 0, section));
        ASSERT_GT (slider.y, 0);
        gui.Frame (session, slider, true);
        ASSERT_TRUE (session.edits.InGesture ());
        EXPECT_DOUBLE_EQ (session.terrainSectionSliderMax, unitIndex == 1 ? 90 : 10);
        const float dragX = unitIndex == 1 ? 240.0f : 55.0f;
        for (int move = 0; move < 6; ++move) {
            gui.Frame (session, { dragX, slider.y }, true);
            EXPECT_EQ (edit::FindPath (session.edits.Doc (), path)->sections[0].crossfall.slope,
                       session.terrainSectionDraft->value.crossfall.slope);
            EXPECT_TRUE (edit::FindPath (session.edits.Doc (), path)->sections[0].crossfall.negative);
            EXPECT_LT (std::abs (edit::PathCrossfallPercent (session.terrainSectionDraft->value.crossfall)), 100);
            EXPECT_EQ (edit::FindPath (session.edits.Doc (), path)->sections[0].fall, edit::Fall::Crown);
            EXPECT_DOUBLE_EQ (session.terrainSectionSliderMax, unitIndex == 1 ? 90 : 10);
            EXPECT_EQ (session.edits.UndoDepth (), 1u);
        }
        gui.Frame (session, { 400, slider.y }, false);
        EXPECT_FALSE (session.edits.InGesture ());
        EXPECT_EQ (session.terrainSectionSliderId, 0u);
        EXPECT_EQ (session.edits.UndoDepth (), 2u);
        const auto edited = edit::FindPath (session.edits.Doc (), path)->sections[0];
        EXPECT_NE (edited.crossfall.slope.a, switched.crossfall.slope.a);
        if (unitIndex == 3)
            EXPECT_LE (edited.crossfall.slope.a, 10);
        unchanged = edited;
        unchanged.crossfall = original.crossfall;
        EXPECT_EQ (unchanged, original);
        EXPECT_EQ (*edit::FindPath (session.edits.Doc (), other), *edit::FindPath (doc, other));
        std::string json;
        ASSERT_TRUE (lg::WriteLandscape (session.edits.Doc (), lg::LandscapeStorage::Virtual, json, error)) << error;
        edit::Document loaded;
        ASSERT_TRUE (lg::ReadLandscape (json, loaded, error)) << error;
        EXPECT_EQ (loaded.paths, session.edits.Doc ().paths);
        ASSERT_TRUE (session.edits.Undo ());
        EXPECT_EQ (edit::FindPath (session.edits.Doc (), path)->sections[0], switched);
        ASSERT_TRUE (session.edits.Undo ());
        EXPECT_EQ (session.edits.Doc ().paths, doc.paths);
    }
}

TEST (HudTerrainSetup, PathCutUnitsAndActualAngleOrHeightSliderKeepIndependentHistoryAndStableBounds)
{
    CheckPathSlopeFields (false);
}

TEST (HudTerrainSetup, PathFillUnitsAndActualAngleOrHeightSliderKeepTypedCutHistoryAndStableBounds)
{
    CheckPathSlopeFields (true);
}
#endif
