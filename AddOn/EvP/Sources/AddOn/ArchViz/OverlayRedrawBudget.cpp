// See OVERLAY-INVARIANTS.md. Moved out of InjectedOverlayRuntime unchanged; the
// header carries the reasoning.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/OverlayRedrawBudget.hpp"

#include "ArchViz/Dxgi/CameraFreshness.hpp"
#include "ArchViz/OverlayRuntimeReport.hpp"

namespace geomsrv {
namespace archviz {
namespace redrawbudget {

namespace {

namespace fresh = dxgi::injection::freshness;
namespace report = overlayruntime::report;

uint64_t g_requests = 0;
uint32_t g_spentThisEpoch = 0;
uint32_t g_epoch = 0;
bool g_gaveUp = false;

API_WindowTypeID g_servedType = APIWind_3DModelID;
Int32 g_servedIndex = 0;
bool g_servedKnown = false;

// ⚠️ AT MOST TWICE PER STALL, JUDGED BY THE EXTENT AND NOT BY
// TICKS. See the header: the heartbeat and Present both re-raise, so a bound
// tied to either is a storm wearing a limit.
const uint32_t kMaxPerEpoch = 2;

// ⚠️ A SEPARATE AND SMALLER BUDGET, SPACED IN TIME. The
// cold-start case cannot be judged by extent -- there is no camera yet, so there
// is no extent -- so it is bounded by attempts alone and closed for good the
// moment a single model frame arrives. Three attempts a second apart is enough
// for Archicad to answer a menu click; a tick-rate loop would be a storm.
const uint32_t kMaxColdStarts = 3;
const uint32_t kTicksBetweenColdStarts = 4; // the heartbeat runs 4x a second
uint32_t g_coldStarts = 0;
uint32_t g_ticksSinceCold = kTicksBetweenColdStarts;

} // namespace

bool FrontWindowIsServedSession ()
{
    API_WindowInfo info = {};
    if (ACAPI_Window_GetCurrentWindow (&info) != NoError)
        return false;
    if (info.typeID != APIWind_3DModelID)
        return false;
    if (!g_servedKnown) {
        g_servedType = info.typeID;
        g_servedIndex = info.index;
        g_servedKnown = true;
        return true;
    }
    return info.typeID == g_servedType && info.index == g_servedIndex;
}

void Consider (uint64_t modelFramesSeen)
{
    // ⚠️ ONE FRAME CLOSES THIS FOR THE SESSION. Not "one
    // frame resets the budget": a viewport that redraws for its own reasons must
    // never buy further attempts, which is the storm the extent budget is
    // shaped to avoid and the same trap one level along.
    if (modelFramesSeen == 0 && g_coldStarts < kMaxColdStarts) {
        if (++g_ticksSinceCold >= kTicksBetweenColdStarts && FrontWindowIsServedSession ()) {
            g_ticksSinceCold = 0;
            ++g_coldStarts;
            ++g_requests;
            ACAPI_View_Redraw ();
            if (g_coldStarts == kMaxColdStarts)
                report::Say ("CAMERA", "asked the 3D window to redraw three times and it produced no model "
                                       "frames -- navigate it once, or check it is the window in front");
        }
    }

    // ⚠️ THE BUDGET BELONGS TO THE EXTENT, NOT TO MODEL FRAMES.
    // Resetting on "model generations advanced" buys another attempt every time a
    // redraw advances the generation WITHOUT producing a camera for this extent,
    // and a window redrawing for unrelated reasons buys attempts forever. A new
    // extent is a new problem and gets a fresh budget; the same extent twice
    // unresolved is a failure, and it is reported as one.
    const uint32_t epoch = fresh::TargetEpoch ();
    if (epoch != g_epoch) {
        g_epoch = epoch;
        g_spentThisEpoch = 0;
        g_gaveUp = false;
    }
    if (!fresh::Stale ()) {
        g_spentThisEpoch = 0;
        g_gaveUp = false;
    }
    if (fresh::TakeRedrawRequest () && FrontWindowIsServedSession () && !g_gaveUp) {
        if (g_spentThisEpoch < kMaxPerEpoch) {
            ++g_spentThisEpoch;
            ++g_requests;
            // The render hook only set an atomic; this is the main thread, where
            // ACAPI may be called.
            ACAPI_View_Redraw ();
        }
        else {
            g_gaveUp = true;
            report::Say ("CAMERA", "redraw twice requested and no camera accepted for the current window size");
        }
    }
}

uint64_t Requests ()
{
    return g_requests;
}

void Reset ()
{
    g_requests = 0;
    g_spentThisEpoch = 0;
    g_epoch = 0;
    g_gaveUp = false;
    g_servedKnown = false;
    g_coldStarts = 0;
    g_ticksSinceCold = kTicksBetweenColdStarts;
}

} // namespace redrawbudget
} // namespace archviz
} // namespace geomsrv
