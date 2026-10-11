#ifndef EVP_NATIVECOMMANDS_POLYGONPICKSTATE_HPP
#define EVP_NATIVECOMMANDS_POLYGONPICKSTATE_HPP

// SDK-FREE single-flight state machine for Tapioca.PickPolygon.
//
// DELIBERATELY FREE OF ACAPI, GSRoot AND Win32. The rules below are exactly the
// ones a live Archicad session cannot be asked to reproduce on demand -- a cancel
// that arrives before the posted pick starts, a request superseded by a new one,
// a callback that fires AFTER the document closed, a stale model snapshot at any
// phase, a hover normal from a DIFFERENT component -- so they live where
// tests/cpp can pin them (test_polygonpickstate.cpp). SurfaceInspectorCommands.cpp
// owns the ACAPI around it.
//
// WHY A STATE MACHINE AND NOT A FLAG. The command dispatcher runs poll/cancel on
// the command worker while the pick itself runs on the main thread inside
// ACAPI_UserInput_GetPoint. That is two threads over one record, so the record is
// mutex-guarded and the mutex is NEVER held across the SDK input or the SDK
// callback -- only around the small copies below.
//
// PHYSICAL SINGLE FLIGHT. A request that is merely Pending (queued, not yet in
// the SDK) and a request whose GetPoint call is physically in flight are BOTH
// busy: Begin() refuses either rather than superseding it. `inputInFlight`
// deliberately survives Reset(), so a project close cannot let a queued start
// open a new pick on top of one the modal loop is still running.

#include <cstdint>
#include <mutex>
#include <string>

namespace geomsrv {
namespace polygonpick {

enum class Status { Idle, Pending, Running, Done, Cancelled, Failed };

// Why a pick should abandon. `BeginInput` performs the atomic check; ShouldAbort
// is the same test re-run after GetPoint returns.
enum class Abort { None, Superseded, LifecycleChanged, CancelRequested };

// The snapshot/model identity a pick is valid for. Captured once on the worker at
// start and re-observed before input, after input and at poll; any drift fails the
// pick. ACAPI-FREE: the caller reads MeshStore, this type only compares.
struct WorldIdentity {
    uint64_t captureStamp = 0;
    bool haveSnapshot = false;
    uint64_t snapshotId = 0;
};

inline bool operator== (const WorldIdentity& a, const WorldIdentity& b)
{
    return a.captureStamp == b.captureStamp && a.haveSnapshot == b.haveSnapshot && a.snapshotId == b.snapshotId;
}
inline bool operator!= (const WorldIdentity& a, const WorldIdentity& b)
{
    return !(a == b);
}

// The live "component under cursor" sighting, as a detached COPY of what
// API_Get3DComponentType reported. The SDK's struct is only valid for the
// duration of the callback, so it is never retained.
struct Sighting {
    bool valid = false;
    std::string guid;
    int dbElemIdx = 0;
    int bodyIdx = 0;
    unsigned long ipgon = 0;
    double normal[3] = { 0.0, 0.0, 0.0 };
};

// A committed pick. `found == false` is a real answer: the pick landed where no
// element was under the cursor (background). It is NOT used for "an element was
// hit but it has no 3D polygon" -- that is a failure, so a 2D hit can never be
// reported as a found face.
struct FacePick {
    bool found = false;
    std::string guid;
    int dbElemIdx = 0;
    int bodyIdx = 0;
    unsigned long ipgon = 0;
    double pos[3] = { 0.0, 0.0, 0.0 };

    int neigID = 0;
    int neigInIndex = 0;
    int neigFlags = 0;
    int neigElemPart = 0;
    unsigned long neigElemPartIndex = 0;

    // Set ONLY when the callback's own identity matched this pick exactly (guid +
    // dbElemIdx + bodyIdx + ipgon) AND the normal was finite and non-zero.
    bool haveNormal = false;
    double normal[3] = { 0.0, 0.0, 0.0 };
};

// Identity of one adopted request.
struct Token {
    uint64_t serial = 0;
    uint64_t generation = 0;
    std::string requestId;
    WorldIdentity expected;
};

// Begin()'s answer. BusyRequest = a Pending/queued request exists; BusyInput = a
// GetPoint call is physically in flight. Both are refusals, never a supersede.
enum class BeginStatus { Accepted, BusyRequest, BusyInput };

// BeginInput()'s answer. Only Started permits GetPoint to run.
enum class InputStart { Started, NotOwned, Cancelled, StaleModel, Superseded, LifecycleChanged };

// A read of the request. A terminal status is returned by Poll exactly once.
struct Outcome {
    Status status = Status::Idle;
    bool cancelRequested = false;
    bool running = false;

    bool callbackSawComponent = false;
    bool callbackMatchedPick = false;

    bool haveSdkCode = false;
    long sdkCode = 0;
    std::string reason;

    FacePick pick;

    uint64_t sessionGeneration = 0;
    WorldIdentity expected;
};

// Single-flight: at most one request is active; a new Begin() replaces a terminal
// one but refuses a pending/running one.
//
// THREADS. Begin/RequestCancel/Poll/Peek/Reset run on the command worker;
// ShouldAbort/BeginInput/EndInput/OnComponent/Complete/Fail/Cancelled run on the
// main thread. All share one mutex; none of them calls ACAPI.
class PolygonPickState {
  public:
    // Adopt a new request. Refuses (without touching the active one) when a
    // request is pending or an SDK input is physically in flight. `expected` is
    // the world identity captured at start.
    BeginStatus Begin (const std::string& newRequestId, const WorldIdentity& expected, Token& outToken);

    // Ask the active request to stop, bound to its exact token/generation. Ignored
    // for a terminal or unknown request. On success `outToken` is the live token
    // the caller posts ACAPI_UserInput_CancelUserInput for.
    bool RequestCancel (const std::string& id, Token& outToken);

    // Atomic mark-and-validate before the SDK input. On Started it sets Running and
    // raises the physical in-flight flag; only Started permits GetPoint.
    InputStart BeginInput (const Token& token, const WorldIdentity& observed);

    // Lower the physical in-flight flag. Safe to call for a token whose request was
    // reset; never clears another token's input.
    void EndInput (const Token& token);

    // The re-check after GetPoint returned (or any late abort test).
    Abort ShouldAbort (const Token& token) const;

    // Copy one callback sighting into the detached record. Ignored unless the
    // exact token still owns a running, in-flight input -- so a callback after
    // Reset (or after the input ended) cannot mutate the record.
    void OnComponent (const Token& token, const Sighting& sighting);

    // The SDK input returned a point. Fails the pick when the world identity
    // drifted; otherwise applies the identity-matched, finite normal.
    void Complete (const Token& token, FacePick pick, const WorldIdentity& observed);

    // The SDK input failed (`sdkCode` != 0) or a non-SDK rule refused the pick.
    void Fail (const Token& token, long sdkCode, const std::string& why);

    // The pick was cancelled. `sdkCode` carries APIERR_CANCEL for a real SDK
    // cancellation, or 0 for a logical one (queued cancel / supersede).
    void Cancelled (const Token& token, long sdkCode, const std::string& why);

    // Consume the active request's outcome. A Done outcome becomes Failed when the
    // world identity drifted since start; a terminal status is returned once and
    // then the request is forgotten.
    Outcome Poll (const std::string& id, const WorldIdentity& observed);

    // Same as Poll but never consumes. `cancel` uses it to echo the live state.
    Outcome Peek (const std::string& id) const;

    // Forget the request and bump the lifecycle generation. Does NOT lower the
    // physical in-flight flag: a Reset during an SDK input must still keep Begin
    // refusing until that input actually returns.
    void Reset ();

    uint64_t Generation () const;
    bool IsActive (const Token& token) const;
    bool IsRunning (const Token& token) const; // owns a running, in-flight input
    bool InputInFlight () const;

  private:
    Outcome BuildOutcomeLocked () const;
    bool OwnsLocked (const Token& token) const;

    mutable std::mutex mtx;
    uint64_t generation = 1;
    uint64_t serial = 0;

    std::string requestId;
    Status status = Status::Idle;
    bool cancelRequested = false;
    uint64_t requestGeneration = 0;
    WorldIdentity expected;

    // Physical single flight -- survives Reset().
    bool inputInFlight = false;
    uint64_t inputSerial = 0;

    bool callbackSawComponent = false;
    Sighting lastSighting;

    bool haveSdkCode = false;
    long sdkCode = 0;
    std::string reason;

    FacePick pick;
    bool callbackMatchedPick = false;
};

} // namespace polygonpick
} // namespace geomsrv

#endif
