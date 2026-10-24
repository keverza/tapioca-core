#include "SunStudy/SunStudyRustFollower.hpp"
#include "SunStudy/SunStudyRustDetail.hpp"

namespace evp::sunstudy::rust {

using namespace detail;

static_assert (sizeof (void*) != 8 || sizeof (TapiocaFollowerEvent) == 72);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaFollowerSnapshot) == 96);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaFollowerTextOutput) == 32);

namespace {

enum Kind : uint8_t {
    kAdopt = 0,
    kObserve = 1,
    kShouldStart = 2,
    kUntilStart = 3,
    kNoteStarted = 4,
    kCanPublish = 5,
    kNoteCompleted = 6,
    kNoteFailed = 7,
    kClear = 8,
    kSetDebounce = 9,
    kRead = 10,
};

SunStudyDependencySignature SignatureOf (const uint64_t (&fields)[3])
{
    SunStudyDependencySignature signature;
    signature.geometry = fields[0];
    signature.sun = fields[1];
    signature.sampling = fields[2];
    return signature;
}

} // namespace

RustSunStudyFollower::RustSunStudyFollower ()
{
    if (Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_follower_create (&handle_, m, n); }, error_))
        Apply (kRead, {}, 0);
}

RustSunStudyFollower::~RustSunStudyFollower ()
{
    std::string ignored;
    Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_follower_destroy (handle_, m, n); }, ignored);
}

int64_t RustSunStudyFollower::Apply (uint8_t kind, const SunStudyDependencySignature& signature, int64_t nowMs,
                                     uint64_t generation, const std::string* studyId, int64_t debounceMs)
{
    TapiocaFollowerEvent event {};
    event.kind = kind;
    event.signature[0] = signature.geometry;
    event.signature[1] = signature.sun;
    event.signature[2] = signature.sampling;
    event.nowMs = nowMs;
    event.generation = generation;
    event.debounceMs = debounceMs;
    if (studyId != nullptr)
        event.studyId = { studyId->data (), studyId->size () };
    TapiocaFollowerSnapshot out {};
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_follower_apply (handle_, &event, &out, m, n); },
                 error_))
        return 0;
    last_ = out;
    return out.answer;
}

void RustSunStudyFollower::Adopt (const std::string& studyId, const SunStudyDependencySignature& signature,
                                  int64_t nowMs)
{
    Apply (kAdopt, signature, nowMs, 0, &studyId);
}

bool RustSunStudyFollower::Observe (const SunStudyDependencySignature& world, int64_t nowMs)
{
    return Apply (kObserve, world, nowMs) != 0;
}

bool RustSunStudyFollower::ShouldStart (int64_t nowMs)
{
    return Apply (kShouldStart, {}, nowMs) != 0;
}

int64_t RustSunStudyFollower::MillisecondsUntilStart (int64_t nowMs)
{
    return Apply (kUntilStart, {}, nowMs);
}

uint64_t RustSunStudyFollower::NoteStarted (const SunStudyDependencySignature& signature, int64_t nowMs)
{
    return static_cast<uint64_t> (Apply (kNoteStarted, signature, nowMs));
}

bool RustSunStudyFollower::CanPublishResult (uint64_t generation, const SunStudyDependencySignature& signature)
{
    return Apply (kCanPublish, signature, 0, generation) != 0;
}

bool RustSunStudyFollower::NoteCompleted (uint64_t generation, const std::string& studyId,
                                          const SunStudyDependencySignature& signature, int64_t nowMs)
{
    return Apply (kNoteCompleted, signature, nowMs, generation, &studyId) != 0;
}

void RustSunStudyFollower::NoteFailed (uint64_t generation, int64_t nowMs)
{
    Apply (kNoteFailed, {}, nowMs, generation);
}

void RustSunStudyFollower::Clear ()
{
    Apply (kClear, {}, 0);
}

void RustSunStudyFollower::SetDebounceMs (int64_t ms)
{
    Apply (kSetDebounce, {}, 0, 0, nullptr, ms);
}

SunStudyDependencySignature RustSunStudyFollower::StudySignature () const
{
    return SignatureOf (last_.study);
}

SunStudyDependencySignature RustSunStudyFollower::WorldSignature () const
{
    return SignatureOf (last_.world);
}

std::string RustSunStudyFollower::Text (uint8_t which) const
{
    TapiocaFollowerTextOutput output {};
    output.which = which;
    size_t length = 0;
    output.length = &length;
    const auto read = [&] (char* m, size_t n) { return tapioca_sunstudy_follower_text (handle_, &output, m, n); };
    if (!Invoke (read, error_)) // shape
        return std::string ();
    std::string text (length, '\0');
    output.text = text.data ();
    output.capacity = text.size ();
    if (length != 0 && !Invoke (read, error_)) // fill
        return std::string ();
    return text;
}

std::string RustSunStudyFollower::StudyId () const
{
    return Text (0);
}

std::string RustSunStudyFollower::Describe () const
{
    return Text (1);
}

} // namespace evp::sunstudy::rust
