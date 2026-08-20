#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/SelectionBridge.hpp"

#include "ArchViz/ArchVizLog.hpp"        // ArchVizLog -- one log for the whole viewer
#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/SceneCmdQueue.hpp"
#include "Geometry/GeometryExtractor.hpp"   // ResolveSelectableOwner / ExpandElementAndParts

#include <windows.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace selectionbridge {

namespace {

UINT_PTR gSelectionTimer = 0;
// Which directions are live. See SelectionBridge.hpp -- "both ways if the user
// wants" is the requirement, so each direction is separately switchable rather
// than the pair being one on/off.
int gSelectionBridgeMode = Both;
// What the bridge last saw, so neither direction fights the other. Without these
// two, mirroring Archicad's selection into the viewer would immediately be
// mirrored back out, and a single click would ping-pong forever.
uint64_t                 gLastPickSeq = 0;
std::vector<std::string> gLastArchicadSelection;

// The current selection, as GUID strings. MAIN THREAD.
std::vector<std::string> ReadArchicadSelection ()
{
    std::vector<std::string> out;

    API_SelectionInfo selInfo = {};
    GS::Array<API_Neig> neigs;
    const GSErrCode err = ACAPI_Selection_Get (&selInfo, &neigs, false);
    // ⚠️ THE MARQUEE HANDLE IS OURS TO FREE even when nothing is selected —
    // ACAPI_Selection_Get allocates it regardless, and leaking it once per tick
    // at 4 Hz is a leak that only shows up after an hour of use.
    if (selInfo.marquee.coords != nullptr)
        BMKillHandle (reinterpret_cast<GSHandle*> (&selInfo.marquee.coords));
    if (err != NoError)
        return out;   // APIERR_NOSEL among others: an empty selection, not a failure

    for (UInt32 i = 0; i < neigs.GetSize (); ++i)
        out.push_back (APIGuidToString (neigs[i].guid).ToCStr ().Get ());
    return out;
}

// The viewer's pick state. ⚠️ THIS USED TO READ TWO RENDERERS through one shape
// so the bridge did not have to branch on which viewer was open; bgfx was the
// other one and it is gone (PLAT-RE66). The struct stays because the bridge is
// written against it and a pick is still a pick — but there is nothing left to
// abstract over, so a future reader should not go looking for the second arm.
struct PickState {
    bool running = false;
    uint64_t pickSeq = 0;
    std::string pickedGuid;
};

PickState ReadPickState ()
{
    if (DiligentViewport::Get ().IsRunning ()) {
        const DiligentViewportStats stats = DiligentViewport::Get ().Stats ();
        return PickState {true, stats.pickSeq, stats.pickedGuid};
    }
    return PickState {};
}

void CALLBACK SelectionTimerProc (HWND, UINT, UINT_PTR, DWORD)
{

    const PickState pick = ReadPickState ();
    if (!pick.running)
        return;

    // ---- viewer -> Archicad ------------------------------------------------
    // ⚠️ DRIVEN BY THE SEQUENCE NUMBER, NOT THE GUID. Clicking the same element
    // twice leaves the guid unchanged, and a bridge watching the string would
    // ignore the second click — including the very common "click the same thing
    // again after deselecting in Archicad".
    if (pick.pickSeq != gLastPickSeq) {
        gLastPickSeq = pick.pickSeq;

        if ((gSelectionBridgeMode & ToArchicad) != 0) {
            if (pick.pickedGuid.empty ()) {
                // Clicked the sky. Archicad's own convention for that is deselect.
                ACAPI_Selection_DeselectAll ();
            } else {
                // ⚠️ THE OWNER, NOT THE PICKED GUID, AND THIS IS THE WHOLE FIX
                // FOR COLUMNS, RAILINGS, CURTAIN WALLS AND STAIRS. The modeler
                // enumerates those five types as their SUB-PARTS — a tread, a
                // panel, a column segment — so a GPU pick can only ever return a
                // sub-part guid, which ACAPI_Selection_SetSelectedElementNeig
                // refuses. This bridge used to drop the click there and call it
                // ordinary, which is exactly the reported symptom: picking works
                // for walls, slabs and objects and does nothing for the other
                // four. GeometryExtractor::ResolveSelectableOwner walks back up.
                const API_Guid picked = APIGuidFromString (pick.pickedGuid.c_str ());
                const API_Guid owner = geomsrv::ResolveSelectableOwner (picked);
                if (owner != APINULLGuid) {
                    API_Neig neig = {};
                    if (ACAPI_Selection_SetSelectedElementNeig (&owner, &neig) == NoError) {
                        GS::Array<API_Neig> neigs;
                        neigs.Push (neig);
                        ACAPI_Selection_DeselectAll ();
                        ACAPI_Selection_Select (neigs, true);
                    }
                }
                // A guid the database still refuses after the owner walk is
                // ordinary, not an error — the element may have been deleted
                // between the click and this tick. The click is dropped rather
                // than reported; making it noisy would put an alert in front of
                // the user for clicking a handrail.
            }
            // Whatever just happened, re-read below rather than assuming:
            // Archicad may have selected the PARENT of what we asked for.
            gLastArchicadSelection.clear ();
        }
    }

    // ---- Archicad -> viewer ------------------------------------------------
    if ((gSelectionBridgeMode & ToViewer) == 0)
        return;

    const std::vector<std::string> now = ReadArchicadSelection ();
    if (now == gLastArchicadSelection)
        return;
    gLastArchicadSelection = now;

    // ⚠️ EXPANDED TO THE SUB-PARTS, THE MIRROR IMAGE OF THE OWNER WALK ABOVE.
    // Archicad selects the STAIR; the scene holds its treads, risers and
    // structures under their own guids and would match none of them, so a
    // selected stair simply would not light up. This is the same expansion live
    // sync already needs, and it is deliberately the SAME function — two copies
    // of "which types have parts" is exactly the pair that drifts.
    std::set<std::string> expanded;
    for (const std::string& guid : now)
        geomsrv::ExpandElementAndParts (APIGuidFromString (guid.c_str ()), expanded);

    SceneCmdQueue::Get ().PushSelection (
        std::vector<std::string> (expanded.begin (), expanded.end ()));
}

}   // namespace

bool Start (int mode)
{
    gSelectionBridgeMode = mode & Both;
    if (gSelectionBridgeMode == Off) {
        Stop ();
        return false;
    }

    // ⚠️ THE LAST-SEEN STATE IS RESET WITH THE MODE, not left over. A bridge
    // re-armed after being off would otherwise compare against a selection from
    // minutes ago: the very first tick would either replay a stale pick into
    // Archicad or decide nothing had changed and never send the CURRENT
    // selection to the viewer, so the tint would be missing until the user
    // clicked something else.
    gLastPickSeq = ReadPickState ().pickSeq;
    gLastArchicadSelection.clear ();

    if (gSelectionTimer != 0)
        return true;

    // 250 ms: fast enough that a click feels connected, slow enough that
    // ACAPI_Selection_Get on a large selection is nowhere near a frame budget.
    // ⚠️ A WM_TIMER is LOW PRIORITY -- Windows delivers it only when the queue is
    // otherwise empty -- so during a heavy drag it simply does not fire, which
    // for a selection mirror is exactly the right behaviour.
    gSelectionTimer = ::SetTimer (nullptr, 0, 250, SelectionTimerProc);
    if (gSelectionTimer == 0) {
        ArchVizLog ("selection bridge: SetTimer failed. Clicking in the viewer will not "
                    "select in Archicad, and Archicad's selection will not highlight in "
                    "the viewer. The viewer itself is unaffected.");
        return false;
    }
    return true;
}

void Stop ()
{
    if (gSelectionTimer != 0) {
        ::KillTimer (nullptr, gSelectionTimer);
        gSelectionTimer = 0;
    }
    gSelectionBridgeMode = Off;
    gLastPickSeq = 0;
    gLastArchicadSelection.clear ();
}

int Mode () { return gSelectionTimer != 0 ? gSelectionBridgeMode : Off; }

}   // namespace selectionbridge
}   // namespace archviz
}   // namespace geomsrv
