#include "Geometry/SurfaceCriteria.hpp"
#include <gtest/gtest.h>
namespace sc = geomsrv::surfacecriteria;

TEST (SurfaceCriteria, GraphisoftGroupedTypeOrsAndLayerUseCorrectParentheses)
{
    std::vector<sc::Rule> rules { { 1, sc::Field::ElementType, sc::Condition::Is, "Wall", true, 1, 0 },
                                  { 2, sc::Field::ElementType, sc::Condition::Is, "Slab", true, 0, 0 },
                                  { 3, sc::Field::ElementType, sc::Condition::Is, "Column", false, 0, 1 },
                                  { 4, sc::Field::Layer, sc::Condition::Is, "Exterior", false, 0, 0 } };
    EXPECT_TRUE (sc::Validate (rules).empty ());
    EXPECT_TRUE (sc::Matches (rules, { "a", "Wall", "Exterior", "Ground", "W-1", true }, "Stucco", "Side"));
    EXPECT_TRUE (sc::Matches (rules, { "b", "Slab", "Exterior", "Ground", "S-1", true }, "Stucco", "Top"));
    EXPECT_FALSE (sc::Matches (rules, { "c", "Wall", "Interior", "Ground", "W-2", true }, "Stucco", "Side"));
    EXPECT_FALSE (sc::Matches (rules, { "d", "Beam", "Exterior", "Ground", "B-1", true }, "Stucco", "Bottom"));
    rules[0].open = 0;
    rules[2].close = 0;
    EXPECT_TRUE (sc::Matches (rules, { "c", "Wall", "Interior", "Ground", "W-2", true }, "Stucco", "Side"));
}

TEST (SurfaceCriteria, MissingFactsNeverSatisfyNegativePredicatesAndInvalidGroupsNeverApply)
{
    std::vector<sc::Rule> rules { { 1, sc::Field::Layer, sc::Condition::IsNot, "Interior" } };
    EXPECT_FALSE (sc::Matches (rules, { "subpart", "Beam" }, "Stucco", "Bottom"));
    rules[0].open = 1;
    EXPECT_FALSE (sc::Validate (rules).empty ());
    rules[0].open = 0;
    rules[0].close = 1;
    EXPECT_FALSE (sc::Validate (rules).empty ());
    EXPECT_FALSE (sc::Matches (rules, { "a", "Wall", "Exterior", "", "", true }, "Stucco", "Side"));
}

TEST (SurfaceCriteria, FreeValuesSupportIdSurfaceAndDirectionPredicates)
{
    const sc::Element element { "a", "Beam", "Cladding", "First", "COVER-021", true };
    std::vector<sc::Rule> rules { { 1, sc::Field::ElementId, sc::Condition::StartsWith, "cover-" },
                                  { 2, sc::Field::Surface, sc::Condition::Contains, "stucco" },
                                  { 3, sc::Field::Facing, sc::Condition::IsNot, "Top" } };
    EXPECT_TRUE (sc::Matches (rules, element, "Stucco - Red Rough", "Bottom"));
    EXPECT_FALSE (sc::Matches (rules, element, "Stucco - Red Rough", "Top"));
    EXPECT_FALSE (sc::Matches (rules, element, "Brick", "Side"));
    EXPECT_TRUE (sc::Matches ({}, element, "Brick", "Side"));
}
