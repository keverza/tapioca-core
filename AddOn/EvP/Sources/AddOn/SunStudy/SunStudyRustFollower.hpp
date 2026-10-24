#ifndef EVP_SUNSTUDY_SUNSTUDYRUSTFOLLOWER_HPP
#define EVP_SUNSTUDY_SUNSTUDYRUSTFOLLOWER_HPP

// SunStudy/SunStudyRustFollower -- SunStudyFollower with its decisions made
// by the Rust follower (P4). Same method surface as the C++ class, so the
// driver swaps an implementation rather than a vocabulary. One thread at a
// time, as the C++ follower. An ABI failure (which only a corrupted handle can
// cause) leaves the last state and is reported through LastError.

#include "SunStudy/SunStudyFollower.hpp"
#include "SunStudy/SunStudyRustFfi.h"

#include <cstdint>
#include <string>

namespace evp::sunstudy::rust {

class RustSunStudyFollower final {
  public:
    RustSunStudyFollower ();
    ~RustSunStudyFollower ();
    RustSunStudyFollower (const RustSunStudyFollower&) = delete;
    RustSunStudyFollower& operator= (const RustSunStudyFollower&) = delete;

    void Adopt (const std::string& studyId, const SunStudyDependencySignature& signature, int64_t nowMs);
    bool Observe (const SunStudyDependencySignature& world, int64_t nowMs);
    bool ShouldStart (int64_t nowMs);
    int64_t MillisecondsUntilStart (int64_t nowMs);
    uint64_t NoteStarted (const SunStudyDependencySignature& signature, int64_t nowMs);
    bool CanPublishResult (uint64_t generation, const SunStudyDependencySignature& signature);
    bool NoteCompleted (uint64_t generation, const std::string& studyId, const SunStudyDependencySignature& signature,
                        int64_t nowMs);
    void NoteFailed (uint64_t generation, int64_t nowMs);
    void Clear ();
    void SetDebounceMs (int64_t ms);

    SunStudyFollowState State () const
    {
        return static_cast<SunStudyFollowState> (last_.state);
    }
    SunStudyDirtyReason DirtyReason () const
    {
        return static_cast<SunStudyDirtyReason> (last_.dirtyReason);
    }
    bool HasRenderableCache () const
    {
        return State () != SunStudyFollowState::NoStudy;
    }
    bool IsFullyCurrent () const
    {
        return State () == SunStudyFollowState::Current;
    }
    bool HasDirtyRegions () const
    {
        const SunStudyFollowState s = State ();
        return s == SunStudyFollowState::DirtyVisible || s == SunStudyFollowState::Starting ||
               s == SunStudyFollowState::UpdatingVisible;
    }
    bool IsComputing () const
    {
        return State () == SunStudyFollowState::UpdatingVisible;
    }
    bool IsDirty () const
    {
        return HasDirtyRegions () || State () == SunStudyFollowState::Failed;
    }
    SunStudyDependencySignature StudySignature () const;
    SunStudyDependencySignature WorldSignature () const;
    uint64_t Generation () const
    {
        return last_.counters[0];
    }
    uint64_t Reruns () const
    {
        return last_.counters[1];
    }
    uint64_t DiscardedResults () const
    {
        return last_.counters[2];
    }
    int64_t DebounceMs () const
    {
        return last_.debounceMs;
    }
    std::string StudyId () const;
    std::string Describe () const;
    const std::string& LastError () const
    {
        return error_;
    }

  private:
    int64_t Apply (uint8_t kind, const SunStudyDependencySignature& signature, int64_t nowMs, uint64_t generation = 0,
                   const std::string* studyId = nullptr, int64_t debounceMs = 0);
    std::string Text (uint8_t which) const;

    TapiocaSunStudyFollower* handle_ = nullptr;
    TapiocaFollowerSnapshot last_ {};
    mutable std::string error_;
};

} // namespace evp::sunstudy::rust

#endif
