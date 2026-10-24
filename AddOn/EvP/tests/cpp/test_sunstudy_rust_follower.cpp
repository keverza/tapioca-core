// Differential test: the Rust follower against SunStudyFollower, in lockstep.
// Both receive the same scripted events -- the driver's 200 ms tick, edits,
// bursts, undo, results that arrive late, failures, clears -- and every
// observable is compared after every event. A follower is only its
// transitions, so equal transitions under the same scripts is the exit
// criterion (handoff P4).

#include "SunStudy/SunStudyFollower.hpp"
#include "SunStudy/SunStudyCoreSelection.hpp"
#include "SunStudy/SunStudyFollowerSelection.hpp"
#include "SunStudy/SunStudyRustFollower.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using namespace evp::sunstudy;
namespace rs = evp::sunstudy::rust;

class Lcg {
  public:
    explicit Lcg (uint64_t seed) : state_ (seed)
    {
    }
    uint64_t Next ()
    {
        state_ = state_ * 6364136223846793005ull + 1442695040888963407ull;
        return state_ >> 33;
    }
    int Below (int n)
    {
        return static_cast<int> (Next () % static_cast<uint64_t> (n));
    }

  private:
    uint64_t state_;
};

SunStudyDependencySignature Sig (uint64_t geometry, uint64_t sun = 7, uint64_t sampling = 11)
{
    SunStudyDependencySignature s;
    s.geometry = geometry;
    s.sun = sun;
    s.sampling = sampling;
    return s;
}

void ExpectSame (SunStudyFollower& e, rs::RustSunStudyFollower& a, int64_t now, const std::string& what)
{
    ASSERT_TRUE (a.LastError ().empty ()) << a.LastError ();
    ASSERT_EQ (a.State (), e.State ()) << what;
    ASSERT_EQ (a.DirtyReason (), e.DirtyReason ()) << what;
    ASSERT_EQ (a.Generation (), e.Generation ()) << what;
    ASSERT_EQ (a.Reruns (), e.Reruns ()) << what;
    ASSERT_EQ (a.DiscardedResults (), e.DiscardedResults ()) << what;
    ASSERT_EQ (a.DebounceMs (), e.DebounceMs ()) << what;
    ASSERT_TRUE (a.StudySignature () == e.StudySignature ()) << what;
    ASSERT_TRUE (a.WorldSignature () == e.WorldSignature ()) << what;
    ASSERT_EQ (a.StudyId (), e.StudyId ()) << what;
    ASSERT_EQ (a.Describe (), e.Describe ()) << what;
    ASSERT_EQ (a.HasRenderableCache (), e.HasRenderableCache ()) << what;
    ASSERT_EQ (a.IsDirty (), e.IsDirty ()) << what;
    ASSERT_EQ (a.ShouldStart (now), e.ShouldStart (now)) << what;
    ASSERT_EQ (a.MillisecondsUntilStart (now), e.MillisecondsUntilStart (now)) << what;
}

} // namespace

TEST (SunStudyRustFollowerParity, ScriptedTicksMatchTransitionForTransition)
{
    Lcg lcg (0xf0110ull);
    size_t starts = 0, completions = 0, discards = 0;
    for (int script = 0; script < 400; ++script) {
        SunStudyFollower e;
        rs::RustSunStudyFollower a;
        uint64_t geometry = 41, sun = 7;
        uint64_t running = 0;
        bool haveRun = false;
        int64_t now = 1000;
        for (int event = 0; event < 80; ++event) {
            const std::string what = "script " + std::to_string (script) + " event " + std::to_string (event);
            now += 50 * (1 + lcg.Below (8)); // 50..400 ms: around the debounce
            const SunStudyDependencySignature world = Sig (geometry, sun);
            switch (lcg.Below (12)) {
                case 0: // an edit
                    ++geometry;
                    break;
                case 1: // undo
                    geometry = geometry > 41 ? geometry - 1 : geometry;
                    break;
                case 2: // the day moved
                    sun ^= 1;
                    break;
                case 3: { // a result arrives (possibly late, possibly for the wrong world)
                    const auto signature = lcg.Below (4) == 0 ? Sig (geometry + 1, sun) : world;
                    const uint64_t generation = haveRun && lcg.Below (3) != 0 ? running : running + 1;
                    const std::string id = "study-" + std::to_string (event);
                    const bool ea = e.NoteCompleted (generation, id, signature, now);
                    ASSERT_EQ (a.NoteCompleted (generation, id, signature, now), ea) << what;
                    completions += ea ? 1 : 0;
                    discards += ea ? 0 : 1;
                    break;
                }
                case 4: { // a failure
                    const uint64_t generation = lcg.Below (2) == 0 ? running : running + 1;
                    e.NoteFailed (generation, now);
                    a.NoteFailed (generation, now);
                    break;
                }
                case 5:
                    if (lcg.Below (8) == 0) {
                        e.Clear ();
                        a.Clear ();
                    }
                    break;
                case 6:
                    if (e.State () == SunStudyFollowState::NoStudy || lcg.Below (10) == 0) {
                        e.Adopt ("adopted", world, now);
                        a.Adopt ("adopted", world, now);
                    }
                    break;
                case 7: {
                    const int64_t debounce = lcg.Below (4) == 0 ? 0 : 100 * (1 + lcg.Below (5));
                    e.SetDebounceMs (debounce);
                    a.SetDebounceMs (debounce);
                    break;
                }
                default: // a driver tick
                    break;
            }
            // Every event is followed by the driver's own tick.
            const SunStudyDependencySignature tick = Sig (geometry, sun);
            ASSERT_EQ (a.Observe (tick, now), e.Observe (tick, now)) << what;
            if (e.State () == SunStudyFollowState::DirtyVisible && e.ShouldStart (now)) {
                ASSERT_TRUE (a.ShouldStart (now)) << what;
                running = e.NoteStarted (tick, now);
                ASSERT_EQ (a.NoteStarted (tick, now), running) << what;
                haveRun = true;
                ++starts;
            }
            ASSERT_EQ (a.CanPublishResult (running, tick), e.CanPublishResult (running, tick)) << what;
            ExpectSame (e, a, now, what);
            if (::testing::Test::HasFatalFailure ())
                return;
        }
    }
    // The corpus must reach every interesting branch, not idle in Current.
    EXPECT_GT (starts, 500u);
    EXPECT_GT (completions, 50u);
    EXPECT_GT (discards, 500u);
}

TEST (SunStudyRustFollowerParity, TheLiveRunawayAndDeadlockScriptsMatch)
{
    // The two scripts that failed live: an edit polled at the timer rate, and
    // a second edit while the replacement runs.
    SunStudyFollower e;
    rs::RustSunStudyFollower a;
    e.Adopt ("study-a", Sig (41), 1000);
    a.Adopt ("study-a", Sig (41), 1000);
    for (int64_t now = 1000; now <= 6000; now += 200) {
        const auto world = Sig (now < 3000 ? 42 : 43);
        ASSERT_EQ (a.Observe (world, now), e.Observe (world, now));
        if (e.State () == SunStudyFollowState::DirtyVisible && e.ShouldStart (now))
            ASSERT_EQ (a.NoteStarted (world, now), e.NoteStarted (world, now));
        ExpectSame (e, a, now, "tick " + std::to_string (now));
    }
    EXPECT_EQ (e.Reruns (), 2u);
}

TEST (SunStudyRustFollowerParity, TheDriversFollowerIsChosenOnceAndForwardsEveryCall)
{
    // The facade the driver holds: Rust under the flag, C++ otherwise, and
    // either way the same transitions as SunStudyFollower.
    for (const auto kind : { SunStudyCoreKind::Cpp, SunStudyCoreKind::Rust }) {
        OverrideSunStudyCore (kind);
        SelectableSunStudyFollower a;
        OverrideSunStudyCore (std::nullopt);
        EXPECT_EQ (a.UsesRust (), kind == SunStudyCoreKind::Rust && RustSunStudyCoreAvailable ());
        SunStudyFollower e;
        e.Adopt ("study-a", Sig (41), 1000);
        a.Adopt ("study-a", Sig (41), 1000);
        uint64_t generation = 0;
        for (int64_t now = 1000; now <= 6000; now += 200) {
            const auto world = Sig (now < 3000 ? 42 : 43);
            ASSERT_EQ (a.Observe (world, now), e.Observe (world, now));
            if (e.State () == SunStudyFollowState::DirtyVisible && e.ShouldStart (now)) {
                ASSERT_TRUE (a.ShouldStart (now));
                generation = e.NoteStarted (world, now);
                ASSERT_EQ (a.NoteStarted (world, now), generation);
            }
            ASSERT_EQ (a.State (), e.State ());
            ASSERT_EQ (a.Generation (), e.Generation ());
            ASSERT_EQ (a.MillisecondsUntilStart (now), e.MillisecondsUntilStart (now));
            ASSERT_EQ (a.Describe (), e.Describe ());
        }
        ASSERT_EQ (a.NoteCompleted (generation, "study-b", Sig (43), 6200),
                   e.NoteCompleted (generation, "study-b", Sig (43), 6200));
        EXPECT_EQ (a.StudyId (), e.StudyId ());
        EXPECT_TRUE (a.IsFullyCurrent ());
        EXPECT_EQ (a.Reruns (), e.Reruns ());
    }
}
