#include "ArchViz/FloorProgramme.hpp"
#include "ArchViz/HudFloorProgramme.hpp"
#include <gtest/gtest.h>
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
