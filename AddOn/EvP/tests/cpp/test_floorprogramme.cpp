#include "ArchViz/FloorProgramme.hpp"
#include "ArchViz/HudFloorProgramme.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include <gtest/gtest.h>
#include <imgui_internal.h>
#include <array>
#include <memory>
#include <cmath>
#include <limits>

namespace fp = geomsrv::archviz::floorprogramme;
namespace hp = geomsrv::archviz::hudprogramme;

// Expected values come from the private generator reading the same brief (its oracle role):
// the target is the middle of the range, the frontage 2.4 m + 1.2 m per room, counts by
// largest remainder and the mix cost of its S6/S7 stages.
TEST (FloorProgramme, DefaultIsTheUserBriefWithPythonTargetsFrontagesAndNames)
{
    const auto programme = fp::Default ();
    ASSERT_TRUE (fp::Valid (programme)) << fp::Problem (programme);
    ASSERT_EQ (programme.types.size (), 6u);
    const double targets[] = { 33, 42.5, 60, 70, 77.5, 85 };
    const double frontages[] = { 4.2, 4.8, 6.0, 6.0, 7.2, 8.4 };
    const char* names[] = { "1.5R", "2R", "3R 55-65", "3R 65-75", "4R", "5R" };
    for (size_t i = 0; i < 6; ++i) {
        EXPECT_DOUBLE_EQ (fp::Target (programme.types[i]), targets[i]);
        EXPECT_NEAR (fp::Frontage (programme.types[i]), frontages[i], 1e-12);
        EXPECT_EQ (fp::Name (programme, i), names[i]);
    }
    EXPECT_NEAR (fp::MeanArea (programme), 60.15, 1e-9);
}

TEST (FloorProgramme, BriefParsesLikeThePrivateGeneratorAndRoundTrips)
{
    fp::Programme programme;
    std::string error;
    ASSERT_TRUE (fp::Parse ("About 150 apartments from which:\n5% 1,5 room 30-36m2\n30% 2room 40-45m2\n"
                            "20% 3 room 55\xE2\x80\x93"
                            "65m2\n20% 3 room 65-75m2\n20% 4 room 75-80m2\n5% 5 room 80-90m2",
                            programme, error))
        << error;
    EXPECT_EQ (programme, fp::Default ());
    fp::Programme again;
    ASSERT_TRUE (fp::Parse (fp::Brief (programme), again, error)) << error;
    EXPECT_EQ (again, programme);
    // Shares that do not add to 100% are scaled, as the generator's loader does.
    ASSERT_TRUE (fp::Parse ("10% 2 room 40-45m2; 30% 3 room 60-70m2", programme, error));
    EXPECT_DOUBLE_EQ (programme.types[0].share, 0.25);
    EXPECT_DOUBLE_EQ (programme.types[1].share, 0.75);
    EXPECT_FALSE (fp::Parse ("two rooms please", programme, error));
    EXPECT_FALSE (error.empty ());
    fp::UnitType row;
    ASSERT_TRUE (fp::ParseType ("12% 4 room 80-75m2", row, error));
    EXPECT_DOUBLE_EQ (row.share, 0.12);
    EXPECT_DOUBLE_EQ (row.minM2, 75);
    EXPECT_FALSE (fp::ParseType ("120% 4 room 75-80m2", row, error));
}

TEST (FloorProgramme, EditsKeepSharesAtOneHundredPercent)
{
    auto programme = fp::Default ();
    const auto sum = [&] {
        double total = 0;
        for (const auto& type : programme.types)
            total += type.share;
        return total;
    };
    ASSERT_TRUE (fp::SetShare (programme, 1, 0.5));
    EXPECT_NEAR (sum (), 1, 1e-12);
    EXPECT_DOUBLE_EQ (programme.types[1].share, 0.5);
    EXPECT_NEAR (programme.types[0].share / programme.types[5].share, 1, 1e-12) << "Others scale in proportion";
    const size_t added = fp::AddType (programme);
    ASSERT_LT (added, fp::kMaxTypes);
    EXPECT_DOUBLE_EQ (programme.types[added].share, 0);
    EXPECT_TRUE (fp::Valid (programme)) << fp::Problem (programme);
    ASSERT_TRUE (fp::SetShare (programme, added, 0.1));
    EXPECT_NEAR (sum (), 1, 1e-12);
    ASSERT_TRUE (fp::RemoveType (programme, 0));
    EXPECT_NEAR (sum (), 1, 1e-12);
    EXPECT_TRUE (fp::Valid (programme));
    EXPECT_FALSE (fp::SetRange (programme, 0, 50, 40)) << "A range must rise";
    EXPECT_TRUE (fp::SetRange (programme, 0, 38, 44));
    EXPECT_TRUE (fp::SetRooms (programme, 0, 2.4));
    EXPECT_DOUBLE_EQ (programme.types[0].rooms, 2.5);
    EXPECT_FALSE (fp::SetShare (programme, 0, std::numeric_limits<double>::quiet_NaN ()));
    fp::Programme single { { { 2, 40, 45, 1 } } };
    EXPECT_FALSE (fp::RemoveType (single, 0)) << "The last type stays";
    EXPECT_FALSE (fp::SetShare (single, 0, 0.3)) << "One type always holds 100%";
    fp::Programme broken = fp::Default ();
    broken.types[2].share += 0.1;
    EXPECT_NE (fp::Problem (broken).find ("110%"), std::string::npos) << fp::Problem (broken);
}

TEST (FloorProgramme, CountsMixCostAndRetypeMatchThePrivateGenerator)
{
    const auto programme = fp::Default ();
    EXPECT_EQ (fp::Counts (programme, 17), (std::vector<int> { 1, 5, 4, 3, 3, 1 }));
    EXPECT_EQ (fp::Counts (programme, 20), (std::vector<int> { 1, 6, 4, 4, 4, 1 }));
    EXPECT_EQ (fp::Counts (programme, 23), (std::vector<int> { 1, 7, 5, 5, 4, 1 }));
    EXPECT_EQ (fp::Counts (programme, 9), (std::vector<int> { 0, 3, 2, 2, 2, 0 }));
    EXPECT_NEAR (fp::MixCost (programme, { 1, 5, 4, 3, 3, 1 }), 1.215, 1e-9);
    EXPECT_NEAR (fp::MixCost (programme, { 0, 6, 4, 4, 4, 2 }), 3.8, 1e-9);
    EXPECT_NEAR (fp::MixCost (programme, { 3, 3, 3, 3, 3, 3 }), 22.26, 1e-9);
    EXPECT_EQ (fp::Retype (programme, 2, 64), 2u) << "Within its range a type is kept";
    EXPECT_EQ (fp::Retype (programme, 2, 72), 3u);
    EXPECT_EQ (fp::Retype (programme, 0, 50), 1u) << "No range fits: the nearest target (2R)";
    EXPECT_EQ (fp::Nearest (programme, 86), 5u);
    EXPECT_NE (fp::Colour (5), fp::Colour (4));
    EXPECT_NE (fp::Key (programme), fp::Key (fp::Programme { { { 2, 40, 45, 1 } } }));
}

TEST (FloorProgramme, PromptAnswersApplyOnlyToTheProgrammeTheyWereAskedAbout)
{
    auto programme = fp::Default ();
    hp::TextEdit row { programme, 1, fp::Brief ({ { programme.types[1] } }) };
    std::string error;
    ASSERT_TRUE (hp::Answer (programme, row, "40% 2 room 41-46m2", error)) << error;
    EXPECT_DOUBLE_EQ (programme.types[1].minM2, 41);
    EXPECT_NEAR (programme.types[1].share, 0.4, 1e-12);
    EXPECT_TRUE (fp::Valid (programme));
    EXPECT_FALSE (hp::Answer (programme, row, "30% 2 room 40-45m2", error)) << "Stale prompt";
    hp::TextEdit brief { programme, -1, fp::Brief (programme) };
    ASSERT_TRUE (hp::Answer (programme, brief, "50% 2 room 40-45m2; 50% 3 room 60-70m2", error)) << error;
    EXPECT_EQ (programme.types.size (), 2u);
    hp::TextEdit bad { programme, -1, {} };
    EXPECT_FALSE (hp::Answer (programme, bad, "nothing", error));
    EXPECT_EQ (programme.types.size (), 2u);
}

TEST (FloorProgramme, StoredTextRoundTripsExactlyAndRefusesForeignOrBrokenText)
{
    auto programme = fp::Default ();
    ASSERT_TRUE (fp::SetShare (programme, 2, 1.0 / 3.0));
    fp::Programme read;
    std::string error;
    ASSERT_TRUE (fp::FromText (fp::ToText (programme), read, error)) << error;
    EXPECT_EQ (read, programme) << "Exact, unlike the rounded brief";
    EXPECT_FALSE (fp::FromText ("30% 2 room 40-45m2", read, error));
    EXPECT_FALSE (fp::FromText ("tapioca.programme 1\n2 40 45\n", read, error));
    EXPECT_FALSE (fp::FromText ("tapioca.programme 1\n2 40 45 0.5\n", read, error)) << "Shares must total 100%";
    EXPECT_EQ (read, programme);
}

TEST (FloorProgramme, TableReadbackValidatesBeforeNormalisingAndDoesNotMutateOnFailure)
{
    auto values = fp::Default ();
    values.types[0].share = 0.10;
    fp::Programme answer;
    std::string error;
    ASSERT_TRUE (hp::PrepareTable (fp::Default (), answer, error));
    EXPECT_EQ (answer, fp::Default ()) << "Untouched Apply preserves stored precision";
    ASSERT_TRUE (hp::PrepareTable (values, answer, error));
    EXPECT_TRUE (fp::Valid (answer));
    EXPECT_NEAR (answer.types[0].share, 0.10 / 1.05, 1e-12);
    const auto valid = answer;
    values.types[1].rooms = 2.25;
    EXPECT_FALSE (hp::PrepareTable (values, answer, error));
    EXPECT_EQ (answer, valid);
    values = fp::Default ();
    values.types[0].minM2 = values.types[0].maxM2;
    EXPECT_FALSE (hp::PrepareTable (values, answer, error));
    values = fp::Default ();
    values.types[0].share = 1.1;
    EXPECT_FALSE (hp::PrepareTable (values, answer, error));
    values.types[0].share = std::numeric_limits<double>::quiet_NaN ();
    EXPECT_FALSE (hp::PrepareTable (values, answer, error));
    values = fp::Default ();
    for (auto& type : values.types)
        type.share = 0;
    EXPECT_FALSE (hp::PrepareTable (values, answer, error));
    EXPECT_EQ (answer, valid);
}

TEST (FloorProgramme, TableAnswerIsAtomicStaleSafeAndQueuesOnlyChangedSaves)
{
    namespace hud = geomsrv::archviz::overlayhud;
    auto state = hud::NewState ();
    const hp::TableEdit edit { state->massingProgramme, 1, hp::Field::Minimum };
    std::string error;
    EXPECT_TRUE (hud::AnswerProgrammeTable (*state, edit, edit.before, error));
    fp::Programme saved;
    EXPECT_FALSE (hud::TakeProgrammeSave (*state, saved)) << "No-op Apply adds no Undo step";
    auto answer = edit.before;
    answer.types[1].minM2 = 41;
    answer.types[2].rooms = 3.5;
    ASSERT_TRUE (hud::AnswerProgrammeTable (*state, edit, answer, error));
    EXPECT_EQ (state->massingProgramme, answer);
    ASSERT_TRUE (hud::TakeProgrammeSave (*state, saved));
    EXPECT_EQ (saved, answer);
    EXPECT_FALSE (hud::TakeProgrammeSave (*state, saved));
    EXPECT_FALSE (hud::AnswerProgrammeTable (*state, edit, edit.before, error));
    EXPECT_EQ (state->massingProgramme, answer);
    const hp::TableEdit fresh { answer };
    auto invalid = answer;
    invalid.types[1].maxM2 = 0;
    EXPECT_FALSE (hud::AnswerProgrammeTable (*state, fresh, invalid, error));
    EXPECT_EQ (state->massingProgramme, answer);
}

TEST (HudProgramme, DoubleClickEachNumericCellRequestsTheNativeTableAndFitsANarrowPanel)
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
    io.DisplaySize = { 800, 1200 };
    io.DeltaTime = 1.0f / 60;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    auto programme = fp::Default ();
    const auto saved = programme;
    std::vector<hp::TableEdit> edits;
    std::array<ImVec2, 4> cells;
    const auto frame = [&] (ImVec2 mouse, bool down) {
        io.MousePos = mouse;
        io.MouseDown[0] = down;
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 240, 1100 });
        ImGui::Begin ("programme", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
        ImGui::SetNextItemOpen (true, ImGuiCond_Always);
        hp::Draw (programme, saved, edits);
        const auto* table = context->Tables.GetByKey (ImGui::GetID ("##massing.programme"));
        if (table) {
            const float rowHeight = table->RowPosY2 - table->RowPosY1;
            const float y = table->RowPosY1 - float (programme.types.size () - 1) * rowHeight + rowHeight / 2;
            for (size_t field = 0; field < cells.size (); ++field) {
                const auto& column = table->Columns[int (field) + 1];
                cells[field] = { (column.WorkMinX + column.WorkMaxX) / 2, y };
            }
        }
        EXPECT_LE (ImGui::GetCurrentWindow ()->DC.CursorMaxPos.x, ImGui::GetCurrentWindow ()->WorkRect.Max.x + 1);
        ImGui::End ();
        ImGui::Render ();
    };
    frame ({ 600, 600 }, false);
    frame ({ 600, 600 }, false);
    for (size_t field = 0; field < cells.size (); ++field) {
        const ImVec2 at = cells[field];
        frame (at, false);
        frame (at, true);
        frame (at, false);
        frame (at, true);
        frame (at, false);
        ASSERT_EQ (edits.size (), field + 1);
        EXPECT_EQ (edits.back ().type, 0u);
        EXPECT_EQ (edits.back ().field, hp::Field (field));
        EXPECT_EQ (edits.back ().before, saved);
    }
    EXPECT_EQ (programme, saved) << "Double-click queues UI only, not a project write";
}
