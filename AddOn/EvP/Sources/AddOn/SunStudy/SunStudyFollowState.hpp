#ifndef EVP_SUNSTUDY_SUNSTUDYFOLLOWSTATE_HPP
#define EVP_SUNSTUDY_SUNSTUDYFOLLOWSTATE_HPP

// SunStudy/SunStudyFollowState -- the follower's vocabulary: the signature a
// study's result is valid for, why it went stale, the state it is in and the
// quiet period before a replacement starts. The policy that moves between the
// states is the follower's own.

#include <cstdint>

namespace evp::sunstudy {

// The inputs a study's result is valid for. Two studies with equal signatures
// must produce equal numbers; anything that breaks that belongs in here.
struct SunStudyDependencySignature {
    // SunStudyGeometryKey of the geometry that was sampled. ⚠️ THE WHOLE
    // MODEL, NOT A COUNT OF ELEMENTS: moving one wall changes what every other
    // surface receives, so there is no such thing as a partially stale study
    // (see `Stale` below). ⚠️ AND NOT THE SNAPSHOT ID: a material-only edit
    // makes a new snapshot with the same key, and must not re-run the study
    // (SunStudyResultKey.hpp, #97).
    uint64_t geometry = 0;
    // The day's sun: date, timestep, horizon filter, north, latitude, longitude.
    // ⚠️ A CONTENT HASH, NOT A COUNTER -- SunSeries::Version() already is one,
    // and for the same reason: five callers can change the day and a forgotten
    // bump serves a stale study that looks correct.
    uint64_t sun = 0;
    // Grid spacing, sample mode, jitter, normal offset -- what was measured and
    // where, as opposed to when.
    uint64_t sampling = 0;
    // The snapshot id, for the log only. ⚠️ NOT COMPARED: two snapshots with
    // one key are the same study. LAST, so `{ geometry, sun, sampling }`
    // brace initialisers keep their meaning.
    uint64_t snapshot = 0;

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

} // namespace evp::sunstudy

#endif
