#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/SurfaceInspectorCommands.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/CommandRegistration.hpp"
#include "NativeCommands/CommandUtils.hpp" // AttributeIndexToName
#include "NativeCommands/PolygonPickState.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "Geometry/MeshStore.hpp" // captureStamp + snapshot identity (ACAPI-free)
#include "Python/MainThreadGate.hpp"

#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace geomsrv {

namespace {

bool ReadElementId (const GS::ObjectState& record, GS::UniString& guid)
{
    GS::ObjectState elementId;
    return record.Get ("elementId", elementId) && elementId.Get ("guid", guid) && !guid.IsEmpty ();
}

void AddElementId (const GS::UniString& guid, GS::ObjectState& record)
{
    GS::ObjectState elementId;
    elementId.Add ("guid", guid);
    record.Add ("elementId", elementId);
}

std::string ToStl (const GS::UniString& text)
{
    return text.ToCStr (0, MaxUSize, CC_UTF8).Get ();
}

// One canonical element identifier, EITHER form: `original` is what the caller
// sent (kept verbatim for the response rows), `canonical` is the SDK's own
// uppercase braced spelling, and `guid` the parsed value. Canonicalising before
// dedup is what makes "{ab..}" and "AB.." the same element instead of two.
struct ElementRef {
    GS::UniString original;
    std::string canonical;
    API_Guid guid;
};

// Round-trips through APIGuidFromString -> APIGuidToString, exactly as the SDK
// reads and writes a guid. A malformed string (or the null guid) is refused
// BEFORE any SDK call, never silently treated as "not found".
bool CanonicalGuid (const GS::UniString& text, std::string& canonical, API_Guid& guidOut)
{
    if (text.IsEmpty ())
        return false;
    const API_Guid parsed = APIGuidFromString (text.ToCStr ().Get ());
    if (parsed == APINULLGuid)
        return false;
    canonical = ToStl (APIGuidToString (parsed));
    guidOut = parsed;
    return true;
}

// The single-flight picker record, the live callback's target, and the token the
// callback must match. Written/read on the main thread inside one GetPoint; only
// s_callbackTarget is atomic so the callback can load it safely.
polygonpick::PolygonPickState s_pickState;
std::atomic<polygonpick::PolygonPickState*> s_callbackTarget { nullptr };
polygonpick::Token s_callbackToken;

void SurfacePickComponentProc (const API_Get3DComponentType* info);

// RAII around the physical SDK input: it lowers s_pickState's in-flight flag even
// on an early return or a throw, which is the ONLY thing that lets Begin() accept
// a new request again.
class ScopedPickInput {
  public:
    ScopedPickInput (polygonpick::PolygonPickState& state, const polygonpick::Token& token) noexcept
        : state (state), token (token)
    {
    }
    ScopedPickInput (const ScopedPickInput&) = delete;
    ScopedPickInput& operator= (const ScopedPickInput&) = delete;
    ~ScopedPickInput ()
    {
        state.EndInput (token);
    }

  private:
    polygonpick::PolygonPickState& state;
    // RunPick's token outlives this guard. Borrowing avoids an allocating copy
    // after BeginInput has already marked the physical input in flight.
    const polygonpick::Token& token;
};

// The snapshot/model identity, read from the ACAPI-free stores. Safe on the
// worker (poll/start) as well as the main thread.
polygonpick::WorldIdentity ReadWorld ()
{
    polygonpick::WorldIdentity world;
    world.captureStamp = MeshStore::Get ().CaptureStamp ();
    const std::shared_ptr<const Snapshot> snapshot = MeshStore::Get ().Current ();
    world.haveSnapshot = snapshot != nullptr;
    world.snapshotId = world.haveSnapshot ? snapshot->id : 0;
    return world;
}

bool ProjectOpen ()
{
    API_ProjectInfo project = {};
    return ACAPI_ProjectOperation_Project (&project) == NoError;
}

// True only when the current front window is the 3D model window. `err` is the
// ACAPI code when the query itself failed, 0 when the window is simply not 3D.
bool FrontWindowIs3D (GSErrCode& err)
{
    API_WindowInfo info = {};
    err = ACAPI_Window_GetCurrentWindow (&info);
    if (err != NoError)
        return false;
    return info.typeID == APIWind_3DModelID;
}

// ===========================================================================
// Tapioca.GetExposedSurfaceQuantities -- A1 of HANDOFF-ExposedSurfaceInspector.
//
// ACAPI_Element_GetSurfaceQuantities is the native "exposed surface area" read,
// and it is the baseline the Stage A gate is measured against a surface schedule.
// Four contract points shape everything below:
//
//   * ALL THREE ARGUMENTS ARE POINTERS and must be non-null. An EMPTY
//     coverElemGuids is legal and means "no covers", never "the whole project".
//   * Records are per (element, surface material, building material), NOT one per
//     element, and come back "in the same order as elemGuids". They are grouped by
//     the CANONICAL elemGUID here -- never indexed positionally.
//   * A malformed/absent identifier is refused BEFORE the SDK call, and a target
//     that does not exist refuses the WHOLE call. That prevents the misleading
//     "the call succeeded, this row is just empty" a post-hoc per-row existence
//     check would produce.
//   * Identifiers are canonicalised then deduplicated for the SDK call, while the
//     response rows keep the caller's ORIGINAL guid strings in request order; the
//     cover echo is the exact deduplicated canonical list that was passed.
//
// Read. Gate, no undo scope.
// ===========================================================================
class GetExposedSurfaceQuantitiesCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "GetExposedSurfaceQuantities";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Array<GS::ObjectState> requestedElements;
        if (!params.Get ("elements", requestedElements) || requestedElements.IsEmpty ()) {
            return NativeCommandResult::Failure (EVP_FAIL ("need elements=[{elementId:{guid}}] (at least one target)",
                                                           "Tapioca.GetExposedSurfaceQuantities"));
        }
        GS::Array<GS::ObjectState> requestedCovers;
        params.Get ("coverElements", requestedCovers); // absent or empty == no covers

        GS::Array<ElementRef> targets;
        for (const GS::ObjectState& requested : requestedElements) {
            GS::UniString guid;
            if (!ReadElementId (requested, guid)) {
                return NativeCommandResult::Failure (
                    EVP_FAIL ("every element needs elementId.guid", "Tapioca.GetExposedSurfaceQuantities"));
            }
            ElementRef ref;
            ref.original = guid;
            if (!CanonicalGuid (guid, ref.canonical, ref.guid)) {
                return NativeCommandResult::Failure (EVP_FAIL (
                    GS::UniString::Printf ("elementId.guid \"%T\" is not a valid element identifier", guid.ToPrintf ()),
                    "Tapioca.GetExposedSurfaceQuantities"));
            }
            targets.Push (ref);
        }

        GS::Array<ElementRef> covers;
        for (const GS::ObjectState& requested : requestedCovers) {
            GS::UniString guid;
            if (!ReadElementId (requested, guid)) {
                return NativeCommandResult::Failure (
                    EVP_FAIL ("every coverElement needs elementId.guid", "Tapioca.GetExposedSurfaceQuantities"));
            }
            ElementRef ref;
            ref.original = guid;
            if (!CanonicalGuid (guid, ref.canonical, ref.guid)) {
                return NativeCommandResult::Failure (EVP_FAIL (
                    GS::UniString::Printf ("coverElement elementId.guid \"%T\" is not a valid element identifier",
                                           guid.ToPrintf ()),
                    "Tapioca.GetExposedSurfaceQuantities"));
            }
            covers.Push (ref);
        }

        // Deduplicate by CANONICAL guid for the call only; `targets`/`covers`
        // keep every input for the rows and the echo.
        std::unordered_set<std::string> seenTargets, seenCovers;
        GS::Array<API_Guid> targetGuids;
        for (const ElementRef& ref : targets)
            if (seenTargets.insert (ref.canonical).second)
                targetGuids.Push (ref.guid);

        GS::Array<API_Guid> coverGuids;
        GS::Array<std::string> coverCanonical; // the exact list handed to the SDK
        for (const ElementRef& ref : covers)
            if (seenCovers.insert (ref.canonical).second) {
                coverGuids.Push (ref.guid);
                coverCanonical.Push (ref.canonical);
            }

        // Validate existence before the SDK call: an element that is not in the
        // project refuses the whole call with the offending guids, rather than a
        // successful call with a quietly empty row.
        for (const API_Guid& guid : targetGuids) {
            API_Element element = {};
            element.header.guid = guid;
            const GSErrCode elementErr = ACAPI_Element_Get (&element);
            if (elementErr != NoError)
                return NativeCommandResult::Failure (
                    EVP_ACAPI_FAIL ("ACAPI_Element_Get", elementErr,
                                    GS::UniString::Printf ("quantity target %T", APIGuidToString (guid).ToPrintf ())));
        }
        for (const API_Guid& guid : coverGuids) {
            API_Element element = {};
            element.header.guid = guid;
            const GSErrCode elementErr = ACAPI_Element_Get (&element);
            if (elementErr != NoError)
                return NativeCommandResult::Failure (
                    EVP_ACAPI_FAIL ("ACAPI_Element_Get", elementErr,
                                    GS::UniString::Printf ("quantity cover %T", APIGuidToString (guid).ToPrintf ())));
        }

        GS::Array<API_ElemPartSurfaceQuantity> quantities;
        const GSErrCode err = ACAPI_Element_GetSurfaceQuantities (&targetGuids, &coverGuids, &quantities);
        if (err != NoError) {
            return NativeCommandResult::Failure (
                EVP_ACAPI_FAIL ("ACAPI_Element_GetSurfaceQuantities", err,
                                GS::UniString::Printf ("%u target(s), %u cover(s)", (unsigned) targetGuids.GetSize (),
                                                       (unsigned) coverGuids.GetSize ())));
        }

        // Group by CANONICAL guid -- the returned order is not a per-element map,
        // and the key must be the same spelling the lookup below uses.
        std::unordered_map<std::string, GS::Array<GS::ObjectState>> byCanonical;
        for (const API_ElemPartSurfaceQuantity& quantity : quantities) {
            GS::ObjectState record;
            // The SDK's own field names (materialIndex / buildMatIdx /
            // exposedSurface) are kept verbatim: the surface schedule and the
            // DevKit struct use exactly these. The unit is explicit.
            record.Add ("materialIndex", GS::UniString (quantity.materialIndex.ToUniString ()));
            record.Add ("materialName", AttributeIndexToName (API_MaterialID, quantity.materialIndex));
            record.Add ("buildMatIdx", GS::UniString (quantity.buildMatIdx.ToUniString ()));
            record.Add ("buildingMaterialName", AttributeIndexToName (API_BuildingMaterialID, quantity.buildMatIdx));
            record.Add ("exposedSurface", quantity.exposedSurface);
            record.Add ("unit", GS::UniString ("m2"));
            byCanonical[ToStl (APIGuidToString (quantity.elemGUID))].Push (record);
        }

        // One row per requested element, in request order, original guid preserved.
        GS::Array<GS::ObjectState> rows;
        for (const ElementRef& ref : targets) {
            GS::ObjectState row;
            AddElementId (ref.original, row);
            row.Add ("found", true);
            const auto found = byCanonical.find (ref.canonical);
            if (found != byCanonical.end ())
                row.Add ("quantities", found->second);
            else
                row.Add ("quantities", GS::Array<GS::ObjectState> ()); // zero-record row
            rows.Push (row);
        }

        // The exact deduplicated canonical cover list that was passed.
        GS::Array<GS::ObjectState> coverEcho;
        for (const std::string& canonical : coverCanonical) {
            GS::ObjectState cover;
            AddElementId (GS::UniString (canonical.c_str (), CC_UTF8), cover);
            coverEcho.Push (cover);
        }

        os.Add ("unit", GS::UniString ("m2"));
        os.Add ("count", (GS::Int32) rows.GetSize ());
        os.Add ("coverCount", (GS::Int32) coverEcho.GetSize ());
        os.Add ("coverElements", coverEcho);
        os.Add ("elements", rows);
        return os;
    }
};

// ===========================================================================
// Tapioca.PickPolygon -- A2. ONE poolable verb: action=start|poll|cancel.
//
// NeedsMainThread() IS FALSE FOR THE WHOLE COMMAND: a live ACAPI_UserInput_GetPoint
// holds the main thread for human time and may never run inside
// MainThreadGate::Invoke. `start` POSTs the pick and returns; `poll`/`cancel` read
// the mutex-guarded state on the worker; `cancel` also posts a token-bound
// ACAPI_UserInput_CancelUserInput. The dispatcher runs this inline (gate-free).
//
// The snapshot identity is read from MeshStore (ACAPI-free) at start on the
// worker, and re-observed before input, after input and at poll; drift fails the
// pick. The command itself calls no ACAPI outside the posted pick.
// ===========================================================================
class PickPolygonCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "PickPolygon";
    }

    bool NeedsMainThread () const override
    {
        return false;
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString action;
        if (!params.Get ("action", action)) {
            return NativeCommandResult::Failure (EVP_FAIL ("need action=start|poll|cancel", "Tapioca.PickPolygon"));
        }

        if (action == "start") {
            GS::UniString prompt;
            params.Get ("prompt", prompt); // the start schema forbids requestId: the server owns it

            const std::string id = GenerateRequestId ();
            polygonpick::Token token;
            const polygonpick::BeginStatus begin = s_pickState.Begin (id, ReadWorld (), token);
            if (begin == polygonpick::BeginStatus::BusyRequest)
                return NativeCommandResult::Failure (EVP_FAIL (
                    "a pick request is already pending; poll or cancel it first", "Tapioca.PickPolygon start"));
            if (begin == polygonpick::BeginStatus::BusyInput)
                return NativeCommandResult::Failure (
                    EVP_FAIL ("a pick is already in progress; wait for it to finish", "Tapioca.PickPolygon start"));

            if (!PostPick (token, ToStl (prompt))) {
                // Never leave the request Pending forever: a gate that will not
                // dispatch cannot ever complete it.
                s_pickState.Fail (token, 0, "the main-thread gate is not dispatching; the pick was not started");
                return NativeCommandResult::Failure (EVP_FAIL (
                    "the main-thread gate is not dispatching; the pick was not started", "Tapioca.PickPolygon start"));
            }

            archviz::ArchVizLog ("surface pick: start id=" + id + " generation=" + std::to_string (token.generation));
            GS::ObjectState os;
            os.Add ("state", GS::UniString ("pending"));
            os.Add ("requestId", GS::UniString (id.c_str (), CC_UTF8));
            return os;
        }

        if (action == "poll" || action == "cancel") {
            GS::UniString requestId;
            if (!params.Get ("requestId", requestId) || requestId.IsEmpty ()) {
                return NativeCommandResult::Failure (
                    EVP_FAIL ("poll/cancel need requestId (from the start response)", "Tapioca.PickPolygon"));
            }
            const std::string id = ToStl (requestId);

            polygonpick::Outcome outcome;
            if (action == "cancel") {
                polygonpick::Token token;
                if (s_pickState.RequestCancel (id, token))
                    PostCancelUserInput (token); // bound to the exact token/generation
                outcome = s_pickState.Peek (id); // cancel does not consume
            }
            else {
                outcome = s_pickState.Poll (id, ReadWorld ()); // poll consumes a terminal state
            }
            return BuildOutcomeResponse (id, outcome);
        }

        return NativeCommandResult::Failure (EVP_FAIL ("action must be start, poll or cancel", "Tapioca.PickPolygon"));
    }

  private:
    static std::string GenerateRequestId ()
    {
        static std::atomic<uint64_t> counter { 0 };
        return "pick-" + std::to_string (counter.fetch_add (1, std::memory_order_relaxed) + 1);
    }

    static bool PostPick (const polygonpick::Token& token, const std::string& prompt)
    {
        GS::UniString gateError;
        return evp::MainThreadGate::Get ().Post (
            [token, prompt] () {
                try {
                    RunPick (token, prompt);
                }
                catch (...) {
                    s_callbackTarget.store (nullptr, std::memory_order_release);
                    s_pickState.Fail (token, 0, "the native picker failed with an exception");
                }
            },
            gateError);
    }

    // Best-effort in-flight cancel, MAGIC-FREE about which pick it cancels: the
    // job carries the exact token/generation and does nothing unless that token
    // still owns a physically running input. It can therefore never cancel an
    // unrelated later pick.
    //
    // ⚠️ THE DOCUMENTED LIMITATION. The running pick blocks the main thread inside
    // its own modal input loop, and nothing else marshals meanwhile
    // (MainThreadGate.hpp measured 4/4), so this job is typically not delivered
    // until the user acts. The guarantee is the cancelRequested flag, consumed by
    // the post-input check.
    static void PostCancelUserInput (const polygonpick::Token& token)
    {
        GS::UniString gateError;
        evp::MainThreadGate::Get ().Post (
            [token] () {
                if (!s_pickState.IsRunning (token))
                    return;
                const GSErrCode e = ACAPI_UserInput_CancelUserInput ();
                archviz::ArchVizLog ("surface pick: CancelUserInput id=" + token.requestId +
                                     " err=" + std::to_string ((int) e));
            },
            gateError);
    }

    static void CopyPrompt (char (&dest)[256], const std::string& prompt)
    {
        dest[0] = '\0';
        if (prompt.empty ())
            return;
        const std::size_t count = prompt.size () < 255 ? prompt.size () : 255;
        for (std::size_t i = 0; i < count; ++i)
            dest[i] = prompt[i];
        dest[count] = '\0';
    }

    static GS::ObjectState BuildOutcomeResponse (const std::string& id, const polygonpick::Outcome& outcome)
    {
        GS::ObjectState os;
        os.Add ("state", GS::UniString (StateName (outcome.status)));
        os.Add ("requestId", GS::UniString (id.c_str (), CC_UTF8));

        if (outcome.status == polygonpick::Status::Idle)
            return os; // unknown / already-consumed request

        os.Add ("cancelRequested", outcome.cancelRequested);
        os.Add ("running", outcome.running);
        os.Add ("diagnostics", BuildDiagnostics (outcome));

        if (outcome.status == polygonpick::Status::Done)
            os.Add ("pick", BuildPick (outcome.pick));
        else if (outcome.status == polygonpick::Status::Failed || outcome.status == polygonpick::Status::Cancelled)
            os.Add ("failure", BuildFailure (outcome));
        return os;
    }

    static const char* StateName (polygonpick::Status status)
    {
        switch (status) {
            case polygonpick::Status::Pending:
            case polygonpick::Status::Running:
                return "pending";
            case polygonpick::Status::Done:
                return "done";
            case polygonpick::Status::Cancelled:
                return "cancelled";
            case polygonpick::Status::Failed:
                return "failed";
            case polygonpick::Status::Idle:
                break;
        }
        return "none";
    }

    static GS::ObjectState Point3 (double x, double y, double z)
    {
        GS::ObjectState point;
        point.Add ("x", x);
        point.Add ("y", y);
        point.Add ("z", z);
        return point;
    }

    static GS::ObjectState BuildPick (const polygonpick::FacePick& pick)
    {
        GS::ObjectState os;
        os.Add ("found", pick.found);
        os.Add ("guid", GS::UniString (pick.guid.c_str (), CC_UTF8));
        os.Add ("dbElemIdx", (GS::Int32) pick.dbElemIdx);
        os.Add ("bodyIdx", (GS::Int32) pick.bodyIdx);
        os.Add ("ipgon", (GS::Int64) pick.ipgon);
        os.Add ("pos", Point3 (pick.pos[0], pick.pos[1], pick.pos[2]));

        GS::ObjectState neig;
        neig.Add ("neigID", (GS::Int32) pick.neigID);
        neig.Add ("inIndex", (GS::Int32) pick.neigInIndex);
        neig.Add ("flags", (GS::Int32) pick.neigFlags);
        neig.Add ("elemPart", (GS::Int32) pick.neigElemPart);
        neig.Add ("elemPartIndex", (GS::Int64) pick.neigElemPartIndex);
        os.Add ("neig", neig);

        if (pick.haveNormal) {
            os.Add ("normal", Point3 (pick.normal[0], pick.normal[1], pick.normal[2]));
            os.Add ("normalSource", GS::UniString ("callback"));
        }
        return os;
    }

    static GS::ObjectState BuildFailure (const polygonpick::Outcome& outcome)
    {
        GS::ObjectState failure;
        failure.Add ("reason", GS::UniString (outcome.reason.c_str (), CC_UTF8));
        // Raw code only when there really was one (a real SDK failure/cancel); a
        // logical refusal never invents a code.
        if (outcome.haveSdkCode)
            failure.Add ("code", (GS::Int32) outcome.sdkCode);
        return failure;
    }

    static GS::ObjectState BuildDiagnostics (const polygonpick::Outcome& outcome)
    {
        GS::ObjectState diagnostics;
        diagnostics.Add ("sessionGeneration", (GS::Int64) outcome.sessionGeneration);
        diagnostics.Add ("captureStamp", (GS::Int64) outcome.expected.captureStamp);
        diagnostics.Add ("haveSnapshot", outcome.expected.haveSnapshot);
        diagnostics.Add ("snapshotId", (GS::Int64) outcome.expected.snapshotId);
        diagnostics.Add ("callbackSawComponent", outcome.callbackSawComponent);
        diagnostics.Add ("callbackMatchedPick", outcome.callbackMatchedPick);
        return diagnostics;
    }

    // MAIN THREAD. Everything here that is not the SDK input is a quick,
    // non-interactive check.
    static void RunPick (const polygonpick::Token& token, const std::string& prompt)
    {
        const polygonpick::InputStart start = s_pickState.BeginInput (token, ReadWorld ());
        if (start != polygonpick::InputStart::Started) {
            // BeginInput already recorded the terminal state for Cancelled /
            // StaleModel; Superseded/LifecycleChanged/NotOwned are not ours to
            // touch. Either way GetPoint is NOT called.
            return;
        }

        ScopedPickInput inputGuard (s_pickState, token);

        if (!ProjectOpen ()) {
            s_pickState.Fail (token, 0, "no project is open");
            return;
        }
        GSErrCode windowErr = NoError;
        if (!FrontWindowIs3D (windowErr)) {
            s_pickState.Fail (token, windowErr,
                              "the front window is not a 3D model window; a face pick needs the 3D window");
            return;
        }

        API_GetPointType pointInfo = {};
        CopyPrompt (pointInfo.prompt, prompt);
        pointInfo.changeFilter = false;
        pointInfo.changeCursorSet = false;
        pointInfo.changePlane = false;
        pointInfo.enableQuickSelection = true; // magnetic face picking
        pointInfo.planeCoord = { 0.0, 0.0, 0.0 };
        pointInfo.planeNormal = { 0.0, 0.0, 1.0 };

        GSErrCode err = NoError;
        {
            s_callbackTarget.store (&s_pickState, std::memory_order_release);
            s_callbackToken = token;
            err = ACAPI_UserInput_GetPoint (&pointInfo, nullptr, &SurfacePickComponentProc);
            s_callbackTarget.store (nullptr, std::memory_order_release);
        }

        if (err == APIERR_CANCEL) {
            // A real SDK cancellation: keep its raw code in the failure.
            s_pickState.Cancelled (token, err, "the user cancelled the pick");
            return;
        }

        const polygonpick::Abort abort = s_pickState.ShouldAbort (token);
        if (abort == polygonpick::Abort::CancelRequested) {
            s_pickState.Cancelled (token, 0, "the pick was cancelled while it was running");
            return;
        }
        if (abort == polygonpick::Abort::LifecycleChanged) {
            s_pickState.Fail (token, 0, "the document changed while the pick was running");
            return;
        }
        if (abort == polygonpick::Abort::Superseded)
            return; // no-op: the record is not ours

        if (err == APIERR_BADWINDOW) {
            s_pickState.Fail (token, err, "the front window does not accept a 3D point input");
            return;
        }
        if (err != NoError) {
            s_pickState.Fail (token, err, "the point input failed");
            return;
        }

        polygonpick::FacePick pick;
        pick.found = (pointInfo.guid != APINULLGuid);
        if (pick.found)
            pick.guid = APIGuidToString (pointInfo.guid).ToCStr ().Get ();
        pick.dbElemIdx = pointInfo.dbElemIdx;
        pick.bodyIdx = pointInfo.bodyIdx;
        pick.ipgon = pointInfo.ipgon;
        pick.pos[0] = pointInfo.pos.x;
        pick.pos[1] = pointInfo.pos.y;
        pick.pos[2] = pointInfo.pos.z;
        pick.neigID = (int) pointInfo.neig.neigID;
        pick.neigInIndex = pointInfo.neig.inIndex;
        pick.neigFlags = pointInfo.neig.flags;
        pick.neigElemPart = (int) pointInfo.neig.elemPartType;
        pick.neigElemPartIndex = pointInfo.neig.elemPartIndex;

        if (pick.found) {
            // An element without a polygon (a 2D hit, or nothing 3D under the
            // cursor) is NOT a face: never report found:true on guid alone.
            if (pick.ipgon == 0) {
                s_pickState.Fail (token, 0, "the picked element has no 3D polygon (ipgon 0); not a face pick");
                return;
            }
        }
        if (!std::isfinite (pick.pos[0]) || !std::isfinite (pick.pos[1]) || !std::isfinite (pick.pos[2])) {
            s_pickState.Fail (token, 0, "the picked point is not finite");
            return;
        }

        s_pickState.Complete (token, pick, ReadWorld ());

        archviz::ArchVizLog ("surface pick: done id=" + token.requestId + " found=" + (pick.found ? "true" : "false") +
                             " guid=" + pick.guid + " body=" + std::to_string (pick.bodyIdx) +
                             " ipgon=" + std::to_string (pick.ipgon));
    }
};

// The live "component under cursor" callback. MAIN THREAD, inside GetPoint.
// Bounded and silent: copies the transient struct into the owned record by exact
// token and does nothing else (no logging, no ACAPI).
void SurfacePickComponentProc (const API_Get3DComponentType* info)
{
    polygonpick::PolygonPickState* state = s_callbackTarget.load (std::memory_order_acquire);
    if (state == nullptr || info == nullptr)
        return;

    polygonpick::Sighting sighting;
    sighting.valid = (info->guid != APINULLGuid);
    if (sighting.valid)
        sighting.guid = APIGuidToString (info->guid).ToCStr ().Get ();
    sighting.dbElemIdx = info->dbElemIdx;
    sighting.bodyIdx = info->bodyIdx;
    sighting.ipgon = info->ipgon;
    sighting.normal[0] = info->normal.x;
    sighting.normal[1] = info->normal.y;
    sighting.normal[2] = info->normal.z;
    state->OnComponent (s_callbackToken, sighting);
}

constexpr const char kQuantitiesInput[] =
    R"json({"type":"object","properties":{"elements":{"type":"array","minItems":1,"items":{"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"}},"additionalProperties":false,"required":["elementId"]}},"coverElements":{"type":"array","items":{"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"}},"additionalProperties":false,"required":["elementId"]}}},"additionalProperties":false,"required":["elements"],"$defs":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}}})json";
constexpr const char kQuantitiesOutput[] =
    R"json({
    "type":"object",
    "properties":{
        "unit":{"type":"string","const":"m2"},
        "count":{"type":"integer","minimum":0},
        "coverCount":{"type":"integer","minimum":0},
        "coverElements":{"type":"array","items":{"$ref":"#/$defs/element"}},
        "elements":{"type":"array","items":{"$ref":"#/$defs/row"}}
    },
    "additionalProperties":false,
    "required":["unit","count","coverCount","coverElements","elements"],
    "$defs":{
        "elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},
        "element":{"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"}},"additionalProperties":false,"required":["elementId"]},
        "quantity":{
            "type":"object",
            "properties":{
                "materialIndex":{"type":"string","description":"Archicad surface material attribute index."},
                "materialName":{"type":"string","description":"Resolved surface name; empty when unresolved."},
                "buildMatIdx":{"type":"string","description":"Building material attribute index."},
                "buildingMaterialName":{"type":"string","description":"Resolved building material name; empty when unresolved."},
                "exposedSurface":{"type":"number","description":"Exposed surface area in square metres."},
                "unit":{"type":"string","const":"m2"}
            },
            "additionalProperties":false,
            "required":["materialIndex","buildMatIdx","exposedSurface","unit"]
        },
        "row":{"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"},"found":{"type":"boolean"},"quantities":{"type":"array","items":{"$ref":"#/$defs/quantity"}}},"additionalProperties":false,"required":["elementId","found","quantities"]}
    }
})json";
constexpr const char kPickInput[] =
    R"json({"type":"object","properties":{"action":{"type":"string","enum":["start","poll","cancel"]},"requestId":{"type":"string","minLength":1},"prompt":{"type":"string"}},"additionalProperties":false,"required":["action"],"oneOf":[{"properties":{"action":{"const":"start"},"prompt":{"type":"string"}},"required":["action"],"additionalProperties":false},{"properties":{"action":{"enum":["poll","cancel"]},"requestId":{"type":"string","minLength":1}},"required":["action","requestId"],"additionalProperties":false}]})json";
constexpr const char kPickOutput[] =
    R"json({"type":"object","properties":{"state":{"type":"string","enum":["none","pending","done","cancelled","failed"]},"requestId":{"type":"string"},"cancelRequested":{"type":"boolean"},"running":{"type":"boolean"},"pick":{"$ref":"#/$defs/pick"},"failure":{"$ref":"#/$defs/failure"},"diagnostics":{"$ref":"#/$defs/diagnostics"}},"additionalProperties":false,"required":["state","requestId"],"$defs":{"point3":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}},"additionalProperties":false,"required":["x","y","z"]},"neig":{"type":"object","properties":{"neigID":{"type":"integer"},"inIndex":{"type":"integer"},"flags":{"type":"integer"},"elemPart":{"type":"integer"},"elemPartIndex":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["neigID","inIndex","flags","elemPart","elemPartIndex"]},"pick":{"type":"object","properties":{"found":{"type":"boolean"},"guid":{"type":"string"},"dbElemIdx":{"type":"integer"},"bodyIdx":{"type":"integer"},"ipgon":{"type":"integer","minimum":0},"pos":{"$ref":"#/$defs/point3"},"neig":{"$ref":"#/$defs/neig"},"normal":{"$ref":"#/$defs/point3","description":"Component normal, present only when the live callback named the same component as the committed pick and the normal was finite and non-zero."},"normalSource":{"type":"string","enum":["callback"]}},"additionalProperties":false,"required":["found","guid","dbElemIdx","bodyIdx","ipgon","pos","neig"]},"failure":{"type":"object","properties":{"code":{"type":"integer","description":"Raw GSErrCode, present only for a real native SDK failure or cancel."},"reason":{"type":"string"}},"additionalProperties":false,"required":["reason"]},"diagnostics":{"type":"object","properties":{"sessionGeneration":{"type":"integer","minimum":0},"captureStamp":{"type":"integer","minimum":0},"haveSnapshot":{"type":"boolean"},"snapshotId":{"type":"integer","minimum":0},"callbackSawComponent":{"type":"boolean"},"callbackMatchedPick":{"type":"boolean"}},"additionalProperties":false,"required":["sessionGeneration","captureStamp","haveSnapshot","snapshotId","callbackSawComponent","callbackMatchedPick"]}}})json";

const NativeCommandRegistration kSurfaceInspectorCommandRegistrations[] = {
    { "GetExposedSurfaceQuantities", &MakeRegisteredNativeCommand<GetExposedSurfaceQuantitiesCommand>, false,
      kQuantitiesInput, kQuantitiesOutput },
    // cancelExempt: the picker's own poll/cancel must still answer after a run is
    // cancelled, or the modal input could be stranded with no way to report it.
    { "PickPolygon", &MakeRegisteredNativeCommand<PickPolygonCommand>, true, kPickInput, kPickOutput },
};

} // namespace

NativeCommandRegistrations GetSurfaceInspectorCommandRegistrations ()
{
    return MakeRegistrationView (kSurfaceInspectorCommandRegistrations);
}

void ResetSurfaceInspectorState ()
{
    s_pickState.Reset ();
}

} // namespace geomsrv
