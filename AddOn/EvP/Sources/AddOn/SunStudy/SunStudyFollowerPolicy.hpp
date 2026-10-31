#ifndef EVP_SUNSTUDY_SUNSTUDYFOLLOWERPOLICY_HPP
#define EVP_SUNSTUDY_SUNSTUDYFOLLOWERPOLICY_HPP

// SunStudy/SunStudyFollowerPolicy -- the follower the driver runs: the Rust
// follower policy, behind a face the driver can hold in every build. Without
// the Rust archive there is no sun-study core, so no study is ever adopted:
// the policy stays NoStudy and every call is inert. One thread at a time.

#include "SunStudy/SunStudyFollowState.hpp"

#include <memory>
#include <string>

namespace evp::sunstudy {
namespace rust {
class RustSunStudyFollower;
}

class SunStudyFollowerPolicy final {
  public:
    SunStudyFollowerPolicy ();
    ~SunStudyFollowerPolicy ();
    SunStudyFollowerPolicy (const SunStudyFollowerPolicy&) = delete;
    SunStudyFollowerPolicy& operator= (const SunStudyFollowerPolicy&) = delete;

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

    SunStudyFollowState State () const;
    SunStudyDirtyReason DirtyReason () const;
    bool HasRenderableCache () const;
    bool IsFullyCurrent () const;
    bool HasDirtyRegions () const;
    bool IsComputing () const;
    bool IsDirty () const;
    std::string StudyId () const;
    SunStudyDependencySignature StudySignature () const;
    SunStudyDependencySignature WorldSignature () const;
    uint64_t Generation () const;
    uint64_t Reruns () const;
    uint64_t DiscardedResults () const;
    int64_t DebounceMs () const;
    std::string Describe () const;

  private:
    std::unique_ptr<rust::RustSunStudyFollower> rust_;
};

} // namespace evp::sunstudy

#endif
