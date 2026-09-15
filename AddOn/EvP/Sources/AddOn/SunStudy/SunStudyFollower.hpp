#ifndef EVP_SUNSTUDY_SUNSTUDYFOLLOWER_HPP
#define EVP_SUNSTUDY_SUNSTUDYFOLLOWER_HPP

// SunStudy/SunStudyFollower — when a study is stale, when to recompute it, and
// which result is allowed to become visible.
//
// ⚠️ NO ACAPI, NO Diligent, NO CLOCK, NO STUDY. It is a state machine over
// events and a millisecond stamp the caller supplies. That is deliberate and it
// is the whole reason this file exists separately: every rule here is a rule
// about TIME and IDENTITY, and both are exactly what a live session cannot be
// asked to reproduce. A debounce that fires twice, or a result that arrives one
// snapshot late and is shown anyway, is a heat map of a building that is no
// longer there -- and it looks entirely correct.
//
// ---------------------------------------------------------------------------
// WHAT MAKES A STUDY STALE, AND WHAT MUST NOT
//
// ⚠️ THE RENDERER'S ENVIRONMENT NOTIFICATION IS NOT THE STUDY'S DEPENDENCY, and
// conflating them cost a session already. Archicad raises
// `isEnvironmentChanged` whenever the 3D window's projection moves, so a user
// who merely ORBITS produces a continuous stream of them; treating that as a
// reason to recompute would rebuild the study twice a second and would be the
// navigation bug all over again, one layer up.
//
// So the study carries a SIGNATURE of the things that actually change its
// numbers, and the notification is only a reason to RECOMPUTE that signature --
// never, on its own, a reason to act. Camera position, projection, viewport
// size, selection and material colour are absent from it by construction.

#include <cstdint>
#include <string>

namespace evp::sunstudy {

// The inputs a study's result is valid for. Two studies with equal signatures
// must produce equal numbers; anything that breaks that belongs in here.
struct SunStudyDependencySignature {
    // QueryEngine::SnapshotId() of the geometry that was sampled. ⚠️ THE
    // SNAPSHOT, NOT A COUNT OF ELEMENTS: moving one wall changes what every
    // other surface receives, so there is no such thing as a partially stale
    // study (see `Stale` below).
    uint64_t geometry = 0;
    // The day's sun: date, timestep, horizon filter, north, latitude, longitude.
    // ⚠️ A CONTENT HASH, NOT A COUNTER -- SunSeries::Version() already is one,
    // and for the same reason: five callers can change the day and a forgotten
    // bump serves a stale study that looks correct.
    uint64_t sun = 0;
    // Grid spacing, sample mode, jitter, normal offset -- what was measured and
    // where, as opposed to when.
    uint64_t sampling = 0;

    bool operator== (const SunStudyDependencySignature& other) const
    {
        return geometry == other.geometry && sun == other.sun && sampling == other.sampling;
    }
    bool operator!= (const SunStudyDependencySignature& other) const
    {
        return !(*this == other);
    }
};

// Why the visible study is no longer valid. ⚠️ REPORTED RATHER THAN COLLAPSED
// INTO A BOOLEAN: "stale because the model changed" and "stale because the day
// changed" send a reader to completely different places, and a diagnostic that
// can only say `drawing=false` sends them nowhere.
enum class SunStudyDirtyReason : uint8_t {
    None = 0,
    Geometry,
    Sun,
    Sampling,
};

enum class SunStudyFollowState : uint8_t {
    // Nothing has ever been adopted.
    NoStudy,
    // A study is adopted and its signature still matches the world.
    Current,
    // The world moved and part of the analysis is no longer valid -- BUT THE
    // CACHE IS STILL DRAWN.
    //
    // ⚠️ THIS USED TO HIDE THE OVERLAY AND THAT WAS WRONG. Blanking a whole
    // building's analysis because one wall moved throws away the ninety per cent
    // of it that is still exactly correct, and it does so at the moment the user
    // is looking -- so every edit costs them the picture they were reading. What
    // is stale is a REGION, and a region is what should say so. The cache stays
    // visible; the affected patches are marked stale and replaced as they
    // recompute.
    //
    // Hiding the overlay is now reserved for the four cases where there is
    // genuinely nothing to show: the user turned it off, the viewer closed, the
    // configuration went away, or the cache cannot be mapped to the scene at all.
    DirtyVisible,
    // The debounce has expired and a replacement has been asked for.
    Starting,
    // A replacement is running. The cache is STILL DRAWN, stale where affected.
    UpdatingVisible,
    // The last attempt failed. Holds until the world moves again or a caller
    // retries, so a broken study does not spin. The cache stays visible: a
    // failed refresh is a reason to distrust part of a picture, not to delete it.
    Failed,
};

// How long the world must hold still before a replacement is started.
//
// ⚠️ IT IS A QUIET PERIOD, NOT A RATE LIMIT, AND THE DIFFERENCE IS THE POINT.
// One Archicad user action produces several notifications, so a rate limit
// starts a study in the middle of an edit and then another when it finishes. The
// timer RESETS on every relevant change, so a burst of twenty produces exactly
// one run, 300 ms after the last of them.
//
// 300 ms against a 40-100 ms analysis puts a fresh overlay roughly 340-400 ms
// after the user stops -- fast enough to read as live, slow enough that dragging
// an element does not start a study per frame of the drag.
constexpr int64_t kSunStudyDebounceMs = 300;

class SunStudyFollower final {
  public:
    // A study has become the visible one. `signature` is what it was computed
    // against; `nowMs` is any monotonic millisecond stamp.
    void Adopt (const std::string& studyId, const SunStudyDependencySignature& signature, int64_t nowMs);

    // The world as it stands. Call it whenever anything MIGHT have changed --
    // this is the cheap, safe call, and it is the only one that decides.
    //
    // ⚠️ THE TRANSITION TO STALE IS IMMEDIATE AND THE RESTART IS NOT. The moment
    // the signature disagrees, the study stops being current and the overlay
    // must come off: a heat map drawn over geometry it did not measure is the
    // one output of this whole feature that is actively misleading, and waiting
    // 300 ms to stop showing it would leave it on screen for exactly as long as
    // a person needs to read it. The debounce governs only when the REPLACEMENT
    // starts.
    //
    // Returns true when this call changed the state.
    bool Observe (const SunStudyDependencySignature& world, int64_t nowMs);

    // True when the quiet period has expired and a replacement should start now.
    // ⚠️ A QUERY, NOT AN ACTION: the caller owns the bus and the main thread, and
    // this file owns neither.
    bool ShouldStart (int64_t nowMs) const;

    // How much of the quiet period is left, or -1 when nothing is waiting.
    // ⚠️ REPORTED RATHER THAN RE-DERIVED BY A CALLER. A diagnostic that probed
    // ShouldStart in a loop to find the answer would be reading the debounce
    // through a keyhole, and would go wrong the moment the rule gained a term.
    int64_t MillisecondsUntilStart (int64_t nowMs) const;

    // A replacement has been started for the world as of `signature`. Returns
    // the generation to carry with it and to check on completion.
    uint64_t NoteStarted (const SunStudyDependencySignature& signature, int64_t nowMs);

    // A run finished. ⚠️ THE GENERATION IS CHECKED, NOT THE TIMING, and this is
    // the rule that stops a believable wrong answer: a study started against
    // snapshot 41 may complete after the model has moved to 42, and it will
    // complete SUCCESSFULLY with a perfectly plausible result. Accepting it
    // paints the old building's sunlight onto the new one.
    //
    // Returns false when the result is stale and must be discarded.
    bool NoteCompleted (uint64_t generation, const std::string& studyId, const SunStudyDependencySignature& signature,
                        int64_t nowMs);

    // A run failed or was refused. Same generation rule: a failure from a
    // superseded run says nothing about the current one.
    void NoteFailed (uint64_t generation, int64_t nowMs);

    // Forget everything. The overlay goes with it.
    void Clear ();

    SunStudyFollowState State () const
    {
        return state_;
    }
    SunStudyDirtyReason DirtyReason () const
    {
        return dirtyReason_;
    }
    // ---- the four questions, and they are NOT the same question -------------
    //
    // ⚠️ THEY WERE ONE BOOLEAN AND THAT IS WHAT MADE THE OVERLAY VANISH ON EVERY
    // EDIT. "Is there anything to draw", "is all of it up to date", "is some of
    // it stale" and "is work in flight" have four different answers and drive
    // four different behaviours; collapsing them forced the renderer to treat
    // "partly stale" as "nothing to show".

    // Is there a cache the renderer should be drawing? True in every state
    // except NoStudy -- INCLUDING while dirty and while updating.
    bool HasRenderableCache () const
    {
        return state_ != SunStudyFollowState::NoStudy;
    }
    // Is every part of it valid for the world as it stands?
    bool IsFullyCurrent () const
    {
        return state_ == SunStudyFollowState::Current;
    }
    // Is some part of it stale? ⚠️ NOT a reason to stop drawing -- a reason to
    // draw those parts as stale and to start replacing them.
    bool HasDirtyRegions () const
    {
        return state_ == SunStudyFollowState::DirtyVisible || state_ == SunStudyFollowState::Starting ||
               state_ == SunStudyFollowState::UpdatingVisible;
    }
    bool IsComputing () const
    {
        return state_ == SunStudyFollowState::UpdatingVisible;
    }
    bool IsDirty () const
    {
        return HasDirtyRegions () || state_ == SunStudyFollowState::Failed;
    }
    const std::string& StudyId () const
    {
        return studyId_;
    }
    const SunStudyDependencySignature& StudySignature () const
    {
        return studySignature_;
    }
    const SunStudyDependencySignature& WorldSignature () const
    {
        return worldSignature_;
    }
    uint64_t Generation () const
    {
        return generation_;
    }
    uint64_t Reruns () const
    {
        return reruns_;
    }
    // Results thrown away because the world moved while they were computing.
    // ⚠️ REPORTED, because a study that is always superseded looks exactly like a
    // study that never runs, and the two need opposite fixes.
    uint64_t DiscardedResults () const
    {
        return discarded_;
    }
    int64_t DebounceMs () const
    {
        return debounceMs_;
    }
    void SetDebounceMs (int64_t ms)
    {
        debounceMs_ = ms > 0 ? ms : kSunStudyDebounceMs;
    }

    // A sentence a person can act on: which snapshot the study measured against
    // which the scene now holds. ⚠️ THE NUMBERS, NOT JUST THE VERDICT -- "stale"
    // alone cannot be told apart from "never started".
    std::string Describe () const;

  private:
    void GoDirty (SunStudyDirtyReason reason, int64_t nowMs);

    SunStudyFollowState state_ = SunStudyFollowState::NoStudy;
    SunStudyDirtyReason dirtyReason_ = SunStudyDirtyReason::None;
    std::string studyId_;
    SunStudyDependencySignature studySignature_;
    SunStudyDependencySignature worldSignature_;
    // What the run currently in flight is computing FOR.
    //
    // ⚠️ WITHOUT IT THE FOLLOWER SUPERSEDES ITS OWN REPLACEMENT. While a
    // replacement computes, the STUDY's signature is expected to differ from the
    // world -- that difference is the entire reason the run exists. An Observe
    // that compared the world against the study would therefore find it stale on
    // the very next tick, bump the generation, and throw away the run that was
    // fixing it, for ever. See the live runaway note in Observe.
    SunStudyDependencySignature runSignature_;
    bool haveRun_ = false;
    // The generation a result must carry to be accepted. Bumped on every
    // transition into Dirty, so anything already running is superseded.
    uint64_t generation_ = 0;
    uint64_t reruns_ = 0;
    uint64_t discarded_ = 0;
    int64_t dirtySinceMs_ = 0;
    int64_t debounceMs_ = kSunStudyDebounceMs;
    bool haveWorld_ = false;
};

} // namespace evp::sunstudy

#endif // EVP_SUNSTUDY_SUNSTUDYFOLLOWER_HPP
