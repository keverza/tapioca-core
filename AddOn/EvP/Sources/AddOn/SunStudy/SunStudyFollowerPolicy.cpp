#include "SunStudy/SunStudyFollowerPolicy.hpp"

#if defined(TAPIOCA_GEOMETRY_RUST)
#include "SunStudy/SunStudyRustFollower.hpp"
// Forward to the Rust follower; `inert` is the answer of a build without one.
#define EVP_FOLLOW(call, inert) (rust_->call)
#else
namespace evp::sunstudy::rust {
class RustSunStudyFollower {};
} // namespace evp::sunstudy::rust
#define EVP_FOLLOW(call, inert) (inert)
#if defined(_MSC_VER)
#pragma warning(disable : 4100) // the inert policy ignores its arguments
#endif
#endif

namespace evp::sunstudy {

SunStudyFollowerPolicy::SunStudyFollowerPolicy ()
{
#if defined(TAPIOCA_GEOMETRY_RUST)
    rust_ = std::make_unique<rust::RustSunStudyFollower> ();
#endif
}

SunStudyFollowerPolicy::~SunStudyFollowerPolicy () = default;

void SunStudyFollowerPolicy::Adopt (const std::string& studyId, const SunStudyDependencySignature& signature,
                                    int64_t nowMs)
{
    EVP_FOLLOW (Adopt (studyId, signature, nowMs), void ());
}

bool SunStudyFollowerPolicy::Observe (const SunStudyDependencySignature& world, int64_t nowMs)
{
    return EVP_FOLLOW (Observe (world, nowMs), false);
}

bool SunStudyFollowerPolicy::ShouldStart (int64_t nowMs)
{
    return EVP_FOLLOW (ShouldStart (nowMs), false);
}

int64_t SunStudyFollowerPolicy::MillisecondsUntilStart (int64_t nowMs)
{
    return EVP_FOLLOW (MillisecondsUntilStart (nowMs), int64_t { -1 });
}

uint64_t SunStudyFollowerPolicy::NoteStarted (const SunStudyDependencySignature& signature, int64_t nowMs)
{
    return EVP_FOLLOW (NoteStarted (signature, nowMs), uint64_t { 0 });
}

bool SunStudyFollowerPolicy::CanPublishResult (uint64_t generation, const SunStudyDependencySignature& signature)
{
    return EVP_FOLLOW (CanPublishResult (generation, signature), false);
}

bool SunStudyFollowerPolicy::NoteCompleted (uint64_t generation, const std::string& studyId,
                                            const SunStudyDependencySignature& signature, int64_t nowMs)
{
    return EVP_FOLLOW (NoteCompleted (generation, studyId, signature, nowMs), false);
}

void SunStudyFollowerPolicy::NoteFailed (uint64_t generation, int64_t nowMs)
{
    EVP_FOLLOW (NoteFailed (generation, nowMs), void ());
}

void SunStudyFollowerPolicy::Clear ()
{
    EVP_FOLLOW (Clear (), void ());
}

void SunStudyFollowerPolicy::RetireReplacement (int64_t nowMs)
{
    const auto id = StudyId ();
    if (id.empty ())
        return;
    const auto study = StudySignature (), world = WorldSignature ();
    Clear (); // advance identity even when an edit was undone during the run
    Adopt (id, study, nowMs);
    Observe (world, nowMs);
}

void SunStudyFollowerPolicy::SetDebounceMs (int64_t ms)
{
    EVP_FOLLOW (SetDebounceMs (ms), void ());
}

SunStudyFollowState SunStudyFollowerPolicy::State () const
{
    return EVP_FOLLOW (State (), SunStudyFollowState::NoStudy);
}

SunStudyDirtyReason SunStudyFollowerPolicy::DirtyReason () const
{
    return EVP_FOLLOW (DirtyReason (), SunStudyDirtyReason::None);
}

bool SunStudyFollowerPolicy::HasRenderableCache () const
{
    return EVP_FOLLOW (HasRenderableCache (), false);
}

bool SunStudyFollowerPolicy::IsFullyCurrent () const
{
    return EVP_FOLLOW (IsFullyCurrent (), false);
}

bool SunStudyFollowerPolicy::HasDirtyRegions () const
{
    return EVP_FOLLOW (HasDirtyRegions (), false);
}

bool SunStudyFollowerPolicy::IsComputing () const
{
    return EVP_FOLLOW (IsComputing (), false);
}

bool SunStudyFollowerPolicy::IsDirty () const
{
    return EVP_FOLLOW (IsDirty (), false);
}

std::string SunStudyFollowerPolicy::StudyId () const
{
    return EVP_FOLLOW (StudyId (), std::string ());
}

SunStudyDependencySignature SunStudyFollowerPolicy::StudySignature () const
{
    return EVP_FOLLOW (StudySignature (), SunStudyDependencySignature ());
}

SunStudyDependencySignature SunStudyFollowerPolicy::WorldSignature () const
{
    return EVP_FOLLOW (WorldSignature (), SunStudyDependencySignature ());
}

uint64_t SunStudyFollowerPolicy::Generation () const
{
    return EVP_FOLLOW (Generation (), uint64_t { 0 });
}

uint64_t SunStudyFollowerPolicy::Reruns () const
{
    return EVP_FOLLOW (Reruns (), uint64_t { 0 });
}

uint64_t SunStudyFollowerPolicy::DiscardedResults () const
{
    return EVP_FOLLOW (DiscardedResults (), uint64_t { 0 });
}

int64_t SunStudyFollowerPolicy::DebounceMs () const
{
    return EVP_FOLLOW (DebounceMs (), kSunStudyDebounceMs);
}

std::string SunStudyFollowerPolicy::Describe () const
{
    return EVP_FOLLOW (Describe (), std::string ("this build has no sun-study core"));
}

} // namespace evp::sunstudy

#undef EVP_FOLLOW
