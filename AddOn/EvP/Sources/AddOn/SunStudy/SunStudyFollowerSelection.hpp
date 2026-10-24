#ifndef EVP_SUNSTUDY_SUNSTUDYFOLLOWERSELECTION_HPP
#define EVP_SUNSTUDY_SUNSTUDYFOLLOWERSELECTION_HPP

// SunStudy/SunStudyFollowerSelection -- the follower the driver runs: the C++
// SunStudyFollower or, under TAPIOCA_SUNSTUDY_CORE=rust, the Rust policy (P5).
// Chosen once, when constructed, so one driver never mixes the two. Same
// method surface as SunStudyFollower; one thread at a time.

#include "SunStudy/SunStudyFollower.hpp"

#include <memory>
#include <string>

namespace evp::sunstudy {
namespace rust {
class RustSunStudyFollower;
}

class SelectableSunStudyFollower final {
  public:
    SelectableSunStudyFollower ();
    ~SelectableSunStudyFollower ();
    SelectableSunStudyFollower (const SelectableSunStudyFollower&) = delete;
    SelectableSunStudyFollower& operator= (const SelectableSunStudyFollower&) = delete;

    bool UsesRust () const
    {
        return rust_ != nullptr;
    }

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
    SunStudyFollower cpp_;
    std::unique_ptr<rust::RustSunStudyFollower> rust_;
};

} // namespace evp::sunstudy

#endif
