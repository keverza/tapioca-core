#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditPathEdit.hpp"
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <cmath>
#include <numbers>

namespace eh = geomsrv::archviz::edithud;
namespace edit = geomsrv::edit;
namespace lg = tapioca::geometry;
namespace {

edit::Document TwoRangeDocument ()
{
    using Unit = lg::TerrainSlope::Unit;
    edit::Document doc;
    std::string error;
    EXPECT_TRUE (lg::StartLandscape (doc, { 1 }, error)) << error;
    const auto path = edit::AddPath (doc, { { 0, 0, 10 }, { 20, 0, 11 } }, 2);
    const auto other = edit::AddPath (doc, { { 0, 10, 10 }, { 20, 10, 11 } }, 2);
    EXPECT_TRUE (edit::SplitPathSection (doc, path, 10, error)) << error;
    EXPECT_TRUE (edit::EditPath (
        doc, path,
        [] (auto& value, auto&) {
            auto& first = value.sections[0];
            first.widthLeft = 1;
            first.widthRight = 2;
            first.crossfall = { { Unit::Percent, 2, 0 }, true };
            first.fall = edit::Fall::Crown;
            first.cutSlope = { Unit::Percent, 50, 0 };
            first.fillSlope = { Unit::RiseRun, 1, 3 };
            first.shoulder = .25;
            first.rounding = 1;
            auto& second = value.sections[1];
            second.widthLeft = 3;
            second.widthRight = 4;
            second.taper = true;
            second.endLeft = 3.5;
            second.endRight = 4.5;
            second.crossfall = { { Unit::Height, .03, 1 }, false };
            second.fall = edit::Fall::Right;
            second.cutSlope = { Unit::Height, 2, 4 };
            second.fillSlope = { Unit::Percent, 40, 0 };
            second.shoulder = .5;
            second.rounding = 2;
            return true;
        },
        error))
        << error;
    lg::LandscapeSource source;
    source.pathId = path;
    source.role = lg::SourceRole::PathCenterline;
    lg::Id id = 0;
    EXPECT_TRUE (lg::AddLandscapeSource (doc, source, id, error)) << error;
    (void) other;
    return doc;
}

struct MixedGui {
    ImGuiContext* previous = ImGui::GetCurrentContext ();
    ImGuiContext* context = ImGui::CreateContext ();
    MixedGui ()
    {
        auto& io = ImGui::GetIO ();
        io.IniFilename = nullptr;
        io.DisplaySize = { 900, 1600 };
        io.DeltaTime = 1.0f / 60;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    }
    ~MixedGui ()
    {
        ImGui::DestroyContext (context);
        ImGui::SetCurrentContext (previous);
    }
    std::string Frame (eh::Session& session, ImVec2 pointer = { 850, 1550 }, bool down = false)
    {
        auto& io = ImGui::GetIO ();
        io.AddMousePosEvent (pointer.x, pointer.y);
        io.AddMouseButtonEvent (0, down);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 700, 1500 });
        ImGui::Begin ("mixed sections", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer ();
        eh::TerrainDesignPanel (session);
        const std::string text = context->LogBuffer.c_str ();
        ImGui::LogFinish ();
        ImGui::End ();
        ImGui::Render ();
        return text;
    }
    ImVec2 Locate (eh::Session& session, ImGuiID target)
    {
        for (float y = 24; y < 1450; y += 2) {
            Frame (session, { 60, y });
            if (context->HoveredId == target)
                return { 60, y + 2 };
        }
        return { -1, -1 };
    }
    ImGuiID Section () const
    {
        return ImHashStr ("Terrain cross-section", 0, ImGui::FindWindowByName ("mixed sections")->ID);
    }
};

void ChooseUnit (MixedGui& gui, eh::Session& session, const char* field, int index)
{
    gui.Frame (session);
    const auto combo = gui.Locate (session, ImHashStr (field, 0, gui.Section ()));
    ASSERT_GT (combo.y, 0);
    gui.Frame (session, combo, true);
    gui.Frame (session, combo, false);
    gui.Frame (session, combo, false);
    gui.Frame (session, combo, false);
    auto* popup = ImGui::FindWindowByName ("##Combo_00");
    ASSERT_NE (popup, nullptr);
    const char* names[] { "Percent (%)", "Angle (degrees)", "Rise:run", "Height / run (m)" };
    const auto target = ImHashStr (names[index], 0, ImHashData (&index, sizeof (index), popup->ID));
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
}

} // namespace

TEST (HudMixedPathSections, MixedDisplayPreservesEveryFieldUntilAnActualEdit)
{
    auto doc = TwoRangeDocument ();
    const auto before = doc;
    eh::Session session (doc);
    const auto& path = doc.paths[0];
    ASSERT_TRUE (
        eh::BeginTerrainSectionEdit (session, std::vector<edit::Id> { path.sections[0].id, path.sections[1].id }));
    const auto mixed = session.terrainSectionDraft->mixed;
    for (const auto field : { eh::SectionField::WidthLeft, eh::SectionField::WidthRight, eh::SectionField::Taper,
                              eh::SectionField::Crossfall, eh::SectionField::CrossfallUnit, eh::SectionField::Fall,
                              eh::SectionField::Cut, eh::SectionField::CutUnit, eh::SectionField::FillUnit,
                              eh::SectionField::Shoulder, eh::SectionField::Rounding })
        EXPECT_EQ (mixed & eh::SectionFieldBit (field), 0u) << int (field);
    EXPECT_EQ (mixed & eh::SectionFieldBit (eh::SectionField::EndLeft), 0u);
    EXPECT_EQ (mixed & eh::SectionFieldBit (eh::SectionField::EndRight), 0u);
    EXPECT_EQ (mixed & eh::SectionFieldBit (eh::SectionField::Fill), 0u);
    EXPECT_EQ (session.edits.Doc ().paths, doc.paths);
    EXPECT_EQ (session.edits.UndoDepth (), 0u);
    MixedGui gui;
    const auto text = gui.Frame (session);
    EXPECT_NE (text.find ("{ mixed } Left width"), std::string::npos) << text;
    EXPECT_NE (text.find ("Crossfall unit mixed"), std::string::npos) << text;
    EXPECT_NE (text.find ("{ mixed } Falls to"), std::string::npos) << text;
    EXPECT_NE (text.find ("{ mixed } Rounding"), std::string::npos) << text;
    EXPECT_EQ (session.edits.Doc ().paths, doc.paths);
}

TEST (HudMixedPathSections, UnitChoiceConvertsEachRangeFromItsOwnAuthoredSlope)
{
    using Unit = lg::TerrainSlope::Unit;
    auto doc = TwoRangeDocument ();
    const auto before = doc.paths[0].sections;
    const auto other = doc.paths[1];
    eh::Session session (doc);
    const auto& path = doc.paths[0];
    ASSERT_TRUE (
        eh::BeginTerrainSectionEdit (session, std::vector<edit::Id> { path.sections[0].id, path.sections[1].id }));
    MixedGui gui;
    ChooseUnit (gui, session, "Crossfall unit", 3);
    const auto& ranges = session.edits.Doc ().paths[0].sections;
    ASSERT_EQ (ranges.size (), 2u);
    EXPECT_EQ (ranges[0].crossfall.slope.unit, Unit::Height);
    EXPECT_EQ (ranges[1].crossfall.slope.unit, Unit::Height);
    EXPECT_NEAR (ranges[0].crossfall.slope.a, .02, 1e-12);
    EXPECT_NEAR (ranges[1].crossfall.slope.a, .03, 1e-12);
    EXPECT_EQ (ranges[0].crossfall.slope.b, 1);
    EXPECT_EQ (ranges[1].crossfall.slope.b, 1);
    EXPECT_TRUE (ranges[0].crossfall.negative);
    EXPECT_FALSE (ranges[1].crossfall.negative);
    for (size_t i = 0; i < 2; ++i) {
        auto unchanged = ranges[i];
        unchanged.crossfall.slope = before[i].crossfall.slope;
        EXPECT_EQ (unchanged, before[i]);
    }
    EXPECT_EQ (session.edits.Doc ().paths[1], other);
    EXPECT_EQ (session.edits.UndoDepth (), 1u);
    const auto converted = session.edits.Doc ().paths[0].sections;
    ASSERT_TRUE (session.edits.Undo ());
    EXPECT_EQ (session.edits.Doc ().paths, doc.paths);
    ASSERT_TRUE (session.edits.Redo ());
    EXPECT_EQ (session.edits.Doc ().paths[0].sections, converted);
    std::string json, error;
    ASSERT_TRUE (lg::WriteLandscape (session.edits.Doc (), lg::LandscapeStorage::Virtual, json, error)) << error;
    edit::Document loaded;
    ASSERT_TRUE (lg::ReadLandscape (json, loaded, error)) << error;
    EXPECT_EQ (loaded.paths, session.edits.Doc ().paths);
}

TEST (HudMixedPathSections, ScalarAndFallEditsApplyOnlyTheirFieldWithOneUndo)
{
    auto doc = TwoRangeDocument ();
    const auto before = doc.paths[0].sections;
    eh::Session session (doc);
    const auto& path = doc.paths[0];
    ASSERT_TRUE (
        eh::BeginTerrainSectionEdit (session, std::vector<edit::Id> { path.sections[0].id, path.sections[1].id }));
    session.terrainSectionDraft->value.widthLeft = 2.5;
    session.terrainSectionDraft->mixed |= eh::SectionFieldBit (eh::SectionField::WidthLeft);
    ASSERT_TRUE (eh::ApplyTerrainSectionEdit (session)) << session.message;
    const auto widths = session.edits.Doc ().paths[0].sections;
    for (size_t i = 0; i < 2; ++i) {
        EXPECT_EQ (widths[i].widthLeft, 2.5);
        auto unchanged = widths[i];
        unchanged.widthLeft = before[i].widthLeft;
        EXPECT_EQ (unchanged, before[i]);
    }
    EXPECT_EQ (session.edits.UndoDepth (), 1u);
    const auto widened = session.edits.Doc ().paths[0].sections;
    ASSERT_TRUE (session.terrainSectionDraft);
    session.terrainSectionDraft->value.fall = edit::Fall::Left;
    session.terrainSectionDraft->mixed |= eh::SectionFieldBit (eh::SectionField::Fall);
    ASSERT_TRUE (eh::ApplyTerrainSectionEdit (session)) << session.message;
    const auto& falls = session.edits.Doc ().paths[0].sections;
    for (size_t i = 0; i < 2; ++i) {
        EXPECT_EQ (falls[i].fall, edit::Fall::Left);
        auto unchanged = falls[i];
        unchanged.fall = before[i].fall;
        EXPECT_EQ (unchanged, widths[i]);
    }
    EXPECT_EQ (session.edits.UndoDepth (), 2u);
    ASSERT_TRUE (session.edits.Undo ());
    EXPECT_EQ (session.edits.Doc ().paths[0].sections, widened);
    ASSERT_TRUE (session.edits.Undo ());
    EXPECT_EQ (session.edits.Doc ().paths, doc.paths);
}

TEST (HudMixedPathSections, SliderGestureRestoresUntouchedMixedFieldsAndUsesOneUndo)
{
    auto doc = TwoRangeDocument ();
    const auto before = doc.paths[0].sections;
    eh::Session session (doc);
    const auto& path = doc.paths[0];
    ASSERT_TRUE (
        eh::BeginTerrainSectionEdit (session, std::vector<edit::Id> { path.sections[0].id, path.sections[1].id }));
    ASSERT_TRUE (eh::BeginTerrainSectionDrag (session));
    session.terrainSectionDraft->value.widthRight = 2.25;
    session.terrainSectionDraft->mixed |= eh::SectionFieldBit (eh::SectionField::WidthRight);
    ASSERT_TRUE (eh::MoveTerrainSectionDrag (session)) << session.message;
    const auto& moved = session.edits.Doc ().paths[0].sections;
    for (size_t i = 0; i < 2; ++i) {
        EXPECT_EQ (moved[i].widthRight, 2.25);
        auto unchanged = moved[i];
        unchanged.widthRight = before[i].widthRight;
        EXPECT_EQ (unchanged, before[i]);
    }
    EXPECT_EQ (session.edits.UndoDepth (), 0u);
    eh::EndTerrainSectionDrag (session);
    EXPECT_EQ (session.edits.UndoDepth (), 1u);
    ASSERT_TRUE (session.edits.Undo ());
    EXPECT_EQ (session.edits.Doc ().paths, doc.paths);
}

TEST (HudMixedPathSections, MixedTaperChoicePreservesEachRangesEndWidths)
{
    auto doc = TwoRangeDocument ();
    const auto before = doc.paths[0].sections;
    eh::Session session (doc);
    const auto& path = doc.paths[0];
    ASSERT_TRUE (
        eh::BeginTerrainSectionEdit (session, std::vector<edit::Id> { path.sections[0].id, path.sections[1].id }));
    session.terrainSectionDraft->value.taper = true;
    session.terrainSectionDraft->mixed |= eh::SectionFieldBit (eh::SectionField::Taper);
    ASSERT_TRUE (eh::ApplyTerrainSectionEdit (session)) << session.message;
    const auto& ranges = session.edits.Doc ().paths[0].sections;
    for (size_t i = 0; i < 2; ++i) {
        EXPECT_TRUE (ranges[i].taper);
        EXPECT_EQ (ranges[i].endLeft, before[i].endLeft);
        EXPECT_EQ (ranges[i].endRight, before[i].endRight);
        auto unchanged = ranges[i];
        unchanged.taper = before[i].taper;
        EXPECT_EQ (unchanged, before[i]);
    }
    EXPECT_EQ (session.edits.UndoDepth (), 1u);
}

TEST (HudMixedPathSections, DirectUnitOperationConvertsEverySelectedRange)
{
    using Unit = lg::TerrainSlope::Unit;
    auto doc = TwoRangeDocument ();
    const auto before = doc.paths[0].sections;
    eh::Session session (doc);
    const auto& path = doc.paths[0];
    ASSERT_TRUE (
        eh::BeginTerrainSectionEdit (session, std::vector<edit::Id> { path.sections[0].id, path.sections[1].id }));
    session.terrainSectionDraft->value.cutSlope.unit = Unit::RiseRun;
    session.terrainSectionDraft->mixed |= eh::SectionFieldBit (eh::SectionField::CutUnit);
    EXPECT_EQ (session.terrainSectionDraft->mixed & eh::SectionFieldBit (eh::SectionField::CutUnit),
               eh::SectionFieldBit (eh::SectionField::CutUnit));
    EXPECT_EQ (session.terrainSectionDraft->sections.size (), 2u);
    ASSERT_TRUE (eh::ApplyTerrainSectionEdit (session)) << session.message;
    const auto& ranges = session.edits.Doc ().paths[0].sections;
    EXPECT_NEAR (ranges[0].cutSlope.a, .5, 1e-12);
    EXPECT_NEAR (ranges[1].cutSlope.a, .5, 1e-12);
    EXPECT_EQ (ranges[0].cutSlope.b, 1);
    EXPECT_EQ (ranges[1].cutSlope.b, 1);
    for (size_t i = 0; i < 2; ++i) {
        auto unchanged = ranges[i];
        unchanged.cutSlope = before[i].cutSlope;
        EXPECT_EQ (unchanged, before[i]);
    }
}

TEST (HudMixedPathSections, DifferentCutUnitsAlsoConvertPerRangeWithoutFlatteningValues)
{
    using Unit = lg::TerrainSlope::Unit;
    auto doc = TwoRangeDocument ();
    const auto before = doc.paths[0].sections;
    eh::Session session (doc);
    const auto& path = doc.paths[0];
    ASSERT_TRUE (
        eh::BeginTerrainSectionEdit (session, std::vector<edit::Id> { path.sections[0].id, path.sections[1].id }));
    lg::TerrainSlope converted;
    std::string conversionError;
    ASSERT_TRUE (lg::ConvertTerrainSlope (before[1].cutSlope, Unit::RiseRun, 1, converted, conversionError))
        << conversionError;
    EXPECT_NEAR (converted.a, .5, 1e-12);
    EXPECT_EQ (converted.b, 1);
    MixedGui gui;
    ChooseUnit (gui, session, "Cut slope unit", 2);
    EXPECT_EQ (session.message, "Cross-section applied. Terrain preview follows the document revision.");
    EXPECT_EQ (session.terrainSectionDraft->value.cutSlope.unit, Unit::RiseRun);
    EXPECT_NE (session.terrainSectionDraft->mixed & eh::SectionFieldBit (eh::SectionField::CutUnit), 0u);
    const auto& ranges = session.edits.Doc ().paths[0].sections;
    EXPECT_EQ (ranges[0].cutSlope.unit, Unit::RiseRun);
    EXPECT_EQ (ranges[1].cutSlope.unit, Unit::RiseRun);
    EXPECT_NEAR (ranges[0].cutSlope.a, .5, 1e-12);
    EXPECT_NEAR (ranges[1].cutSlope.a, .5, 1e-12);
    EXPECT_EQ (ranges[0].cutSlope.b, 1);
    EXPECT_EQ (ranges[1].cutSlope.b, 1);
    for (size_t i = 0; i < 2; ++i) {
        auto unchanged = ranges[i];
        unchanged.cutSlope = before[i].cutSlope;
        EXPECT_EQ (unchanged, before[i]);
    }
    EXPECT_EQ (session.edits.UndoDepth (), 1u);
}

TEST (HudMixedPathSections, StaleSelectionAndInvalidCommonValueRefuseAtomically)
{
    auto doc = TwoRangeDocument ();
    const auto& path = doc.paths[0];
    eh::Session session (doc);
    const std::vector<edit::Id> selected { path.sections[0].id, path.sections[1].id };
    ASSERT_TRUE (eh::BeginTerrainSectionEdit (session, selected));
    session.terrainSectionDraft->value.widthLeft = -1;
    session.terrainSectionDraft->mixed |= eh::SectionFieldBit (eh::SectionField::WidthLeft);
    EXPECT_FALSE (eh::ApplyTerrainSectionEdit (session));
    EXPECT_EQ (session.edits.Doc ().paths, doc.paths);
    EXPECT_EQ (session.edits.UndoDepth (), 0u);
    ASSERT_TRUE (eh::BeginTerrainSectionEdit (session, selected));
    ASSERT_TRUE (session.edits.Apply ([&] (edit::Document& changed, std::string& error) {
        return edit::DeletePathSection (changed, path.id, path.sections[1].id, error);
    }));
    EXPECT_FALSE (eh::ApplyTerrainSectionEdit (session));
    EXPECT_EQ (session.edits.Doc ().paths.size (), 2u);
    EXPECT_EQ (session.edits.Doc ().paths[0].sections.size (), 1u);
    EXPECT_EQ (session.edits.UndoDepth (), 1u);
    EXPECT_FALSE (
        eh::BeginTerrainSectionEdit (session, std::vector<edit::Id> { path.sections[1].id, path.sections[0].id }));
}
