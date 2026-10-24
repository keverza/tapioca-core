#include "SunStudy/SunStudyFollowerSelection.hpp"
#include "SunStudy/SunStudyCoreSelection.hpp"

#if defined(TAPIOCA_GEOMETRY_RUST)
#include "SunStudy/SunStudyRustFollower.hpp"
// Forward to whichever follower runs; the Rust one only exists with the archive.
#define EVP_FOLLOW(call) (rust_ != nullptr ? rust_->call : cpp_.call)
#else
namespace evp::sunstudy::rust {
class RustSunStudyFollower {};
} // namespace evp::sunstudy::rust
#define EVP_FOLLOW(call) (cpp_.call)
#endif

namespace evp::sunstudy {

SelectableSunStudyFollower::SelectableSunStudyFollower ()
{
#if defined(TAPIOCA_GEOMETRY_RUST)
    if (SelectedSunStudyCore () == SunStudyCoreKind::Rust)
        rust_ = std::make_unique<rust::RustSunStudyFollower> ();
#endif
}

SelectableSunStudyFollower::~SelectableSunStudyFollower () = default;

void SelectableSunStudyFollower::Adopt (const std::string& studyId, const SunStudyDependencySignature& signature,
                                        int64_t nowMs)
{
    EVP_FOLLOW (Adopt (studyId, signature, nowMs));
}

bool SelectableSunStudyFollower::Observe (const SunStudyDependencySignature& world, int64_t nowMs)
{
    return EVP_FOLLOW (Observe (world, nowMs));
}

bool SelectableSunStudyFollower::ShouldStart (int64_t nowMs)
{
    return EVP_FOLLOW (ShouldStart (nowMs));
}

int64_t SelectableSunStudyFollower::MillisecondsUntilStart (int64_t nowMs)
{
    return EVP_FOLLOW (MillisecondsUntilStart (nowMs));
}

uint64_t SelectableSunStudyFollower::NoteStarted (const SunStudyDependencySignature& signature, int64_t nowMs)
{
    return EVP_FOLLOW (NoteStarted (signature, nowMs));
}

bool SelectableSunStudyFollower::CanPublishResult (uint64_t generation, const SunStudyDependencySignature& signature)
{
    return EVP_FOLLOW (CanPublishResult (generation, signature));
}

bool SelectableSunStudyFollower::NoteCompleted (uint64_t generation, const std::string& studyId,
                                                const SunStudyDependencySignature& signature, int64_t nowMs)
{
    return EVP_FOLLOW (NoteCompleted (generation, studyId, signature, nowMs));
}

void SelectableSunStudyFollower::NoteFailed (uint64_t generation, int64_t nowMs)
{
    EVP_FOLLOW (NoteFailed (generation, nowMs));
}

void SelectableSunStudyFollower::Clear ()
{
    EVP_FOLLOW (Clear ());
}

void SelectableSunStudyFollower::SetDebounceMs (int64_t ms)
{
    EVP_FOLLOW (SetDebounceMs (ms));
}

SunStudyFollowState SelectableSunStudyFollower::State () const
{
    return EVP_FOLLOW (State ());
}

SunStudyDirtyReason SelectableSunStudyFollower::DirtyReason () const
{
    return EVP_FOLLOW (DirtyReason ());
}

bool SelectableSunStudyFollower::HasRenderableCache () const
{
    return EVP_FOLLOW (HasRenderableCache ());
}

bool SelectableSunStudyFollower::IsFullyCurrent () const
{
    return EVP_FOLLOW (IsFullyCurrent ());
}

bool SelectableSunStudyFollower::HasDirtyRegions () const
{
    return EVP_FOLLOW (HasDirtyRegions ());
}

bool SelectableSunStudyFollower::IsComputing () const
{
    return EVP_FOLLOW (IsComputing ());
}

bool SelectableSunStudyFollower::IsDirty () const
{
    return EVP_FOLLOW (IsDirty ());
}

std::string SelectableSunStudyFollower::StudyId () const
{
    return EVP_FOLLOW (StudyId ());
}

SunStudyDependencySignature SelectableSunStudyFollower::StudySignature () const
{
    return EVP_FOLLOW (StudySignature ());
}

SunStudyDependencySignature SelectableSunStudyFollower::WorldSignature () const
{
    return EVP_FOLLOW (WorldSignature ());
}

uint64_t SelectableSunStudyFollower::Generation () const
{
    return EVP_FOLLOW (Generation ());
}

uint64_t SelectableSunStudyFollower::Reruns () const
{
    return EVP_FOLLOW (Reruns ());
}

uint64_t SelectableSunStudyFollower::DiscardedResults () const
{
    return EVP_FOLLOW (DiscardedResults ());
}

int64_t SelectableSunStudyFollower::DebounceMs () const
{
    return EVP_FOLLOW (DebounceMs ());
}

std::string SelectableSunStudyFollower::Describe () const
{
    return EVP_FOLLOW (Describe ());
}

} // namespace evp::sunstudy

#undef EVP_FOLLOW
