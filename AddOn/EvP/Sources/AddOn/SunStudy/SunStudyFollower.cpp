#include "SunStudy/SunStudyFollower.hpp"

namespace evp::sunstudy {

namespace {

// Which field moved. ⚠️ GEOMETRY IS REPORTED FIRST WHEN SEVERAL MOVED AT ONCE,
// because it is the one that invalidates the most and the one a reader should
// chase: an Archicad edit raises the geometry snapshot AND, through the same
// notification, often the environment with it.
SunStudyDirtyReason WhatChanged (const SunStudyDependencySignature& was, const SunStudyDependencySignature& now)
{
    if (was.geometry != now.geometry)
        return SunStudyDirtyReason::Geometry;
    if (was.sun != now.sun)
        return SunStudyDirtyReason::Sun;
    if (was.sampling != now.sampling)
        return SunStudyDirtyReason::Sampling;
    return SunStudyDirtyReason::None;
}

const char* ReasonText (SunStudyDirtyReason reason)
{
    switch (reason) {
        case SunStudyDirtyReason::Geometry:
            return "geometry";
        case SunStudyDirtyReason::Sun:
            return "sun";
        case SunStudyDirtyReason::Sampling:
            return "sampling";
        case SunStudyDirtyReason::None:
            break;
    }
    return "none";
}

} // namespace

void SunStudyFollower::Adopt (const std::string& studyId, const SunStudyDependencySignature& signature, int64_t nowMs)
{
    studyId_ = studyId;
    studySignature_ = signature;
    // ⚠️ ADOPTING SETS THE WORLD TOO, and it has to: a study is adopted at the
    // moment it was computed, so the world it was computed against IS the world
    // as far as anyone knows. Leaving the previous world in place would make the
    // very next Observe report the study stale against a signature it already
    // matches.
    worldSignature_ = signature;
    haveWorld_ = true;
    state_ = SunStudyFollowState::Current;
    dirtyReason_ = SunStudyDirtyReason::None;
    dirtySinceMs_ = nowMs;
    // Nothing is in flight any more, and a stale target left behind would be
    // compared against by the next Observe.
    haveRun_ = false;
}

void SunStudyFollower::GoDirty (SunStudyDirtyReason reason, int64_t nowMs)
{
    // ⚠️ THE GENERATION MOVES ON EVERY TRANSITION INTO DIRTY, INCLUDING ONE THAT
    // ARRIVES MID-COMPUTATION. That is what makes an in-flight result
    // unacceptable the instant the world moves, rather than when it eventually
    // returns -- and it is why the check on completion is against a number
    // rather than against elapsed time.
    ++generation_;
    state_ = SunStudyFollowState::DirtyVisible;
    // Whatever was running is superseded by the bump above; its target must not
    // outlive it as the thing observations are judged against.
    haveRun_ = false;
    dirtyReason_ = reason;
    // ⚠️ AND THE QUIET PERIOD RESTARTS. A change arriving while a replacement is
    // already computing must not start the next one 300 ms after the FIRST edit
    // of the burst.
    dirtySinceMs_ = nowMs;
}

bool SunStudyFollower::Observe (const SunStudyDependencySignature& world, int64_t nowMs)
{
    const bool first = !haveWorld_;
    const SunStudyDependencySignature previous = worldSignature_;
    worldSignature_ = world;
    haveWorld_ = true;

    if (state_ == SunStudyFollowState::NoStudy)
        return false; // nothing to invalidate; the world is just being recorded

    // ⚠️ AGAINST THE STUDY'S SIGNATURE, NOT THE LAST OBSERVED ONE. A world that
    // changes and changes back -- an element moved and undone -- is NOT stale,
    // and comparing against the previous observation would report it as stale
    // forever after. The study's own signature is the only thing its numbers are
    // valid for.
    // ⚠️ A RUN IN FLIGHT IS MEASURED AGAINST WHAT IT TARGETS, NOT AGAINST THE
    // STUDY IT IS REPLACING. This is the second half of the polling problem and
    // it ran away live exactly as the first half deadlocked.
    //
    // While a replacement computes, `studySignature_` still describes the OLD
    // study -- of course it differs from the world; that difference is why the
    // run was started. Comparing against it made every tick report the model
    // stale again, bump the generation, and supersede the run that was fixing
    // it: 100 studies started in 62 seconds, none ever advanced one slice, and
    // the log said `partly stale ... snapshot 2 != snapshot 1` a hundred times
    // without the numbers ever moving. The overlay stayed up showing the
    // pre-edit analysis, so nothing on screen said anything was wrong.
    //
    // Against the RUN's signature, an unchanged world is agreement and the run
    // proceeds; a world that moves again still supersedes it, which is the
    // property that must not be lost.
    const bool running =
        state_ == SunStudyFollowState::Starting || state_ == SunStudyFollowState::UpdatingVisible;
    const SunStudyDependencySignature& against = (running && haveRun_) ? runSignature_ : studySignature_;
    const SunStudyDirtyReason reason = WhatChanged (against, world);
    if (reason == SunStudyDirtyReason::None) {
        // Back in agreement. ⚠️ A STUDY THAT WAS DIRTY AND IS NOW MATCHED AGAIN
        // BECOMES CURRENT, which is what makes an undone edit free -- but only
        // while nothing has replaced it: once a replacement is Starting or
        // Computing, letting the old one quietly become current again would race
        // the run that is about to finish.
        if (state_ == SunStudyFollowState::DirtyVisible) {
            state_ = SunStudyFollowState::Current;
            dirtyReason_ = SunStudyDirtyReason::None;
            return true;
        }
        return false;
    }

    (void) first;

    // Already dirty for the same reason: do not spend a generation on it, and
    // restart the quiet period ONLY IF THE WORLD ACTUALLY MOVED AGAIN.
    //
    // ⚠️ THE `changed` TEST IS LOAD-BEARING AND ITS ABSENCE DEADLOCKED THE
    // FOLLOWER LIVE. The driver POLLS -- it calls Observe every tick whether
    // anything happened or not, because one path that always asks is cheaper to
    // guarantee than a special case that sometimes does not. An unconditional
    // `dirtySinceMs_ = nowMs` here therefore restarted the 300 ms countdown five
    // times a second for ever, ShouldStart never once became true, and the study
    // sat in DirtyVisible indefinitely: the overlay stayed up, correctly, showing
    // stale values that were never replaced.
    //
    // ⚠️ AND THE OFFLINE TESTS COULD NOT SEE IT, because they only called
    // Observe when the signature had changed -- which is the one case where the
    // two behaviours agree. `RepeatedObservationsOfTheSameWorldDoNotPostponeTheStart`
    // now polls the way the driver does.
    if (state_ == SunStudyFollowState::DirtyVisible && dirtyReason_ == reason) {
        const bool worldMovedAgain = previous != world;
        if (worldMovedAgain)
            dirtySinceMs_ = nowMs;
        return false;
    }

    GoDirty (reason, nowMs);
    return true;
}

bool SunStudyFollower::ShouldStart (int64_t nowMs) const
{
    if (state_ != SunStudyFollowState::DirtyVisible)
        return false;
    return (nowMs - dirtySinceMs_) >= debounceMs_;
}

int64_t SunStudyFollower::MillisecondsUntilStart (int64_t nowMs) const
{
    if (state_ != SunStudyFollowState::DirtyVisible)
        return -1;
    const int64_t elapsed = nowMs - dirtySinceMs_;
    return elapsed >= debounceMs_ ? 0 : debounceMs_ - elapsed;
}

uint64_t SunStudyFollower::NoteStarted (const SunStudyDependencySignature& signature, int64_t nowMs)
{
    (void) nowMs;
    state_ = SunStudyFollowState::UpdatingVisible;
    // The run is for THIS world; remember what it will have to match.
    worldSignature_ = signature;
    haveWorld_ = true;
    // ⚠️ AND WHAT EVERY OBSERVATION UNTIL IT FINISHES IS JUDGED AGAINST. See
    // the runaway note in Observe: without this the next tick supersedes it.
    runSignature_ = signature;
    haveRun_ = true;
    ++reruns_;
    return generation_;
}

bool SunStudyFollower::NoteCompleted (uint64_t generation, const std::string& studyId,
                                      const SunStudyDependencySignature& signature, int64_t nowMs)
{
    // ⚠️ TWO CHECKS, AND BOTH ARE LOAD-BEARING. The generation catches a result
    // whose world moved while it ran. The signature catches a result that was
    // started against the wrong inputs in the first place -- a caller passing
    // stale parameters -- which no amount of generation counting would notice.
    if (generation != generation_) {
        ++discarded_;
        return false;
    }
    if (haveWorld_ && signature != worldSignature_) {
        ++discarded_;
        // ⚠️ AND IT GOES BACK TO DIRTY RATHER THAN TO FAILED. Nothing is broken;
        // the world simply moved, and the next quiet period must start another
        // run or the overlay never returns.
        GoDirty (WhatChanged (signature, worldSignature_), nowMs);
        return false;
    }

    Adopt (studyId, signature, nowMs);
    return true;
}

void SunStudyFollower::NoteFailed (uint64_t generation, int64_t nowMs)
{
    if (generation != generation_) {
        // A failure from a superseded run says nothing about the current one.
        ++discarded_;
        return;
    }
    (void) nowMs;
    state_ = SunStudyFollowState::Failed;
}

void SunStudyFollower::Clear ()
{
    state_ = SunStudyFollowState::NoStudy;
    dirtyReason_ = SunStudyDirtyReason::None;
    studyId_.clear ();
    studySignature_ = SunStudyDependencySignature {};
    worldSignature_ = SunStudyDependencySignature {};
    runSignature_ = SunStudyDependencySignature {};
    haveWorld_ = false;
    haveRun_ = false;
    // ⚠️ THE GENERATION DOES NOT GO BACK TO ZERO. A run started before the clear
    // is still out there, and a counter that restarts would eventually hand it a
    // number it matches.
    ++generation_;
    dirtySinceMs_ = 0;
}

std::string SunStudyFollower::Describe () const
{
    switch (state_) {
        case SunStudyFollowState::NoStudy:
            return "no sun study is being followed";
        case SunStudyFollowState::Current:
            return "sun study '" + studyId_ + "' is current for snapshot " + std::to_string (studySignature_.geometry);
        case SunStudyFollowState::DirtyVisible:
            return std::string ("sun study partly stale (") + ReasonText (dirtyReason_) + "): scene snapshot " +
                   std::to_string (worldSignature_.geometry) + " != study snapshot " +
                   std::to_string (studySignature_.geometry);
        case SunStudyFollowState::Starting:
            return "a replacement sun study is starting";
        case SunStudyFollowState::UpdatingVisible:
            return "a replacement sun study is resolving its timesteps; the cache stays on screen";
        case SunStudyFollowState::Failed:
            return "the last sun study attempt failed";
    }
    return "unknown";
}

} // namespace evp::sunstudy
