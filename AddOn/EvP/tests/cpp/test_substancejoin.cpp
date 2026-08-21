// ArchViz/SubstanceJoin — the many-to-many join from building materials to the
// surfaces the renderer actually draws.
//
// ⚠️ WHAT THIS FILE IS DEFENDING. The join is the ONLY place where a correct
// per-material verdict can still become a wrong per-surface one, and every way
// it does that is silent: the model renders, in plausible materials, and the
// timber elements wearing a shared white paint come out as concrete. So the
// tests below are mostly about REFUSAL -- the cases where the right answer is
// "no substance" and the tempting answer is a majority vote.

#include "ArchViz/SubstanceJoin.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace geomsrv::archviz;

namespace {

SurfaceSubstanceObservation Obs (int32_t surface, Substance substance, double weight)
{
    SurfaceSubstanceObservation o;
    o.surface = surface;
    o.substance = substance;
    o.weight = weight;
    return o;
}

}   // namespace

TEST (SubstanceJoin, EmptyInEmptyOut)
{
    EXPECT_TRUE (JoinSurfaceSubstances ({}).empty ());
}

TEST (SubstanceJoin, OneSubstanceWins)
{
    const auto joined = JoinSurfaceSubstances ({
        Obs (7, Substance::Concrete, 40.0),
        Obs (7, Substance::Concrete, 60.0),
    });
    ASSERT_EQ (joined.count (7), 1u);
    EXPECT_EQ (joined.at (7).substance, Substance::Concrete);
    EXPECT_FLOAT_EQ (joined.at (7).confidence, 1.0f);
}

// ⚠️ THE CENTRAL CASE. A white paint on concrete walls and on timber studs is
// the ordinary situation in every real project, and a majority vote would render
// every stud as concrete. The join must refuse.
TEST (SubstanceJoin, SharedSurfaceIsRefusedRatherThanGuessed)
{
    const auto joined = JoinSurfaceSubstances ({
        Obs (3, Substance::Concrete, 70.0),
        Obs (3, Substance::Wood, 30.0),
    });
    ASSERT_EQ (joined.count (3), 1u);
    EXPECT_EQ (joined.at (3).substance, Substance::Unknown);
    EXPECT_FLOAT_EQ (joined.at (3).confidence, 0.0f);
}

// A stray sliver body must not be able to veto a surface. This is the other
// side of the previous test and the reason the bar is 0.90 rather than 1.0.
TEST (SubstanceJoin, NoiseDoesNotVeto)
{
    const auto joined = JoinSurfaceSubstances ({
        Obs (3, Substance::Concrete, 97.0),
        Obs (3, Substance::Wood, 3.0),
    });
    EXPECT_EQ (joined.at (3).substance, Substance::Concrete);
    EXPECT_NEAR (joined.at (3).confidence, 0.97f, 1e-5f);
}

TEST (SubstanceJoin, ExactlyAtTheBarIsAccepted)
{
    const auto joined = JoinSurfaceSubstances ({
        Obs (1, Substance::Metal, 90.0),
        Obs (1, Substance::Unknown, 10.0),
    });
    EXPECT_EQ (joined.at (1).substance, Substance::Metal);
}

// ⚠️ THE REFUSAL ONE LEVEL DOWN MUST SURVIVE THIS ONE. BuildingMaterialSignal
// declines to name a material whenever its two signals disagree; if `Unknown`
// were simply skipped here, a surface sitting mostly on materials that were
// refused would be named confidently by whatever minority did classify.
TEST (SubstanceJoin, UnknownCountsInTheDenominator)
{
    const auto joined = JoinSurfaceSubstances ({
        Obs (5, Substance::Concrete, 20.0),
        Obs (5, Substance::Unknown, 80.0),
    });
    ASSERT_EQ (joined.count (5), 1u);
    EXPECT_EQ (joined.at (5).substance, Substance::Unknown);
}

// "Asked and refused" and "never seen" are different facts, and the renderer
// treats them identically -- but the probe does not, so the distinction is kept.
TEST (SubstanceJoin, RefusedSurfaceIsPresentUnseenSurfaceIsNot)
{
    const auto joined = JoinSurfaceSubstances ({ Obs (9, Substance::Unknown, 5.0) });
    EXPECT_EQ (joined.count (9), 1u);
    EXPECT_EQ (joined.count (10), 0u);
}

// ⚠️ WEIGHT, NOT BODY COUNT. Twenty curtain-wall mullions and one floor slab is
// the shape of a real element list, and counting bodies rather than size would
// let the mullions decide what the slab's surface is made of.
TEST (SubstanceJoin, SizeOutweighsCount)
{
    std::vector<SurfaceSubstanceObservation> observations;
    for (int i = 0; i < 20; ++i)
        observations.push_back (Obs (4, Substance::Metal, 2.0));    // 40 total
    observations.push_back (Obs (4, Substance::Concrete, 960.0));
    const auto joined = JoinSurfaceSubstances (observations);
    EXPECT_EQ (joined.at (4).substance, Substance::Concrete);
}

// A zero-weight body carries no evidence and must not be able to dilute a
// surface below the bar -- a refusal caused by nothing at all.
TEST (SubstanceJoin, ZeroWeightObservationsAreIgnored)
{
    const auto joined = JoinSurfaceSubstances ({
        Obs (2, Substance::Wood, 10.0),
        Obs (2, Substance::Unknown, 0.0),
        Obs (2, Substance::Concrete, -5.0),
    });
    EXPECT_EQ (joined.at (2).substance, Substance::Wood);
    EXPECT_FLOAT_EQ (joined.at (2).confidence, 1.0f);
}

TEST (SubstanceJoin, SurfacesAreIndependent)
{
    const auto joined = JoinSurfaceSubstances ({
        Obs (1, Substance::Concrete, 10.0),
        Obs (2, Substance::Wood, 10.0),
        Obs (3, Substance::Concrete, 5.0),
        Obs (3, Substance::Wood, 5.0),
    });
    EXPECT_EQ (joined.at (1).substance, Substance::Concrete);
    EXPECT_EQ (joined.at (2).substance, Substance::Wood);
    EXPECT_EQ (joined.at (3).substance, Substance::Unknown);
}
