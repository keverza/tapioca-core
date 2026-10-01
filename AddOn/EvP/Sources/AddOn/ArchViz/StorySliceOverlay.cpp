// ArchViz/StorySliceOverlay -- see the header.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/StorySliceOverlay.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/ExtractionThread.hpp"
#include "ArchViz/OverlayController.hpp"
#include "ArchViz/SlabBodies.hpp"
#include "ArchViz/StorySliceSnapshot.hpp"

#include <windows.h>

#include <cstdio>

namespace geomsrv {
namespace archviz {
namespace storysliceoverlay {

namespace {

constexpr UINT kTickMs = 500;
// Full passes asked for while none has cut a storey. Past this the answer is that
// the model reaches no storey, or its passes do not finish -- said, not retried.
constexpr uint32_t kMaxRequests = 3;
// The storeys are re-read every fourth tick: a storey edit is rarer than a slab's.
constexpr uint32_t kStoreyTicks = 4;

bool g_enabled = false;
Request g_request;
Controls g_controls;
UINT_PTR g_timer = 0;
State g_state;

// The model source.
uint64_t g_built = 0;
uint32_t g_requests = 0;
bool g_awaitingPass = false; // a pass we asked for has not been seen idle since

// The slab sources.
std::vector<std::string> g_targets;
std::vector<uint64_t> g_stamps; // the slabs' and their operators'
std::vector<double> g_levels;
uint32_t g_ticks = 0;
// ⚠️ A SLAB SOLID ELEMENT OPERATIONS CUT IS SLICED FROM ITS 3D BODY (SlabSlices
// `SliceBody`), and neither its record nor its stamp moves when an operator does, or when
// a link is made (the user, 2026-10-01: a subtraction left the slices as they were). Each
// slab's operators are followed with it, and its body comes from the extraction pass that
// draws the wireframe, after it (SlabBodies.hpp).
std::vector<std::vector<std::string>> g_operators; // per slab taken
uint64_t g_bodies = 0;                             // the bodies' generation the slices were cut from
bool g_bodyPass = false;                           // a pass for the bodies is wanted once the worker is free

void Say (const std::string& message)
{
    g_state.message = message;
    ArchVizLog ("STOREY SLICES  " + message);
}

// The layer on the overlays, or gone when it has nothing in it. False with the
// refusal said when the store will not take it.
bool Show (Built built)
{
    g_state.slices = built.slices;
    g_state.areaM2 = built.areaM2;
    if (built.slices == 0) {
        if (overlaylayers::Clear (kLayerName))
            overlaycontrol::PublishLayers ();
        return true;
    }
    const std::string refused = overlaylayers::Validate (built.layer);
    if (!refused.empty ()) {
        g_state.slices = 0;
        g_state.areaM2 = 0.0;
        Say ("NOT DRAWN: " + refused);
        return false;
    }
    overlaylayers::Set (std::move (built.layer));
    overlaycontrol::PublishLayers ();
    return true;
}

// ---- the slabs ----------------------------------------------------------------------

// The slabs taken and every operator of theirs: an operator's edit moves the slab's body
// and not its record.
std::vector<std::string> Watched ()
{
    std::vector<std::string> all = g_targets;
    for (const std::vector<std::string>& operators : g_operators)
        all.insert (all.end (), operators.begin (), operators.end ());
    return all;
}

std::vector<std::string> OperatedSlabs ()
{
    std::vector<std::string> operated;
    for (size_t i = 0; i < g_targets.size () && i < g_operators.size (); ++i)
        if (!g_operators[i].empty ())
            operated.push_back (g_targets[i]);
    return operated;
}

const std::vector<std::string>* OperatorsOf (const std::string& guid)
{
    for (size_t i = 0; i < g_targets.size () && i < g_operators.size (); ++i)
        if (g_targets[i] == guid)
            return &g_operators[i];
    return nullptr;
}

// A pass that hands over the operated slabs' bodies: now if the extraction is free, else
// once it is -- a pass already running read what it wanted at its start.
void AskForBodies ()
{
    g_bodyPass = false;
    if (OperatedSlabs ().empty ())
        return;
    ExtractionWorker& worker = ExtractionWorker::Get ();
    if (worker.IsRunning ()) {
        g_bodyPass = true;
        return;
    }
    worker.Start (true);
}

void CutSlabs (const char* why)
{
    const ProjectStoreys storeys = ReadStoreys ();
    g_levels = storeys.levels;
    g_operators = slabsource::Operators (g_targets);
    slabbodies::Want (OperatedSlabs ());
    const std::shared_ptr<const slabbodies::Bodies> bodies = slabbodies::Latest ();
    g_bodies = bodies != nullptr ? bodies->generation : 0;
    g_stamps = slabsource::Stamps (Watched ());
    slabsource::Reading reading = slabsource::Read (g_targets, storeys);
    std::vector<Slice> slices;
    g_state.slabs.clear ();
    g_state.waiting = false;
    std::string problems;
    uint32_t waiting = 0;
    for (const slabslices::Slab& slab : reading.slabs) {
        const std::vector<std::string>* operators = OperatorsOf (slab.guid);
        const Mesh* body = nullptr;
        if (operators != nullptr && !operators->empty () && bodies != nullptr) {
            const auto found = bodies->meshes.find (slab.guid);
            body = found != bodies->meshes.end () ? &found->second : nullptr;
        }
        if (body != nullptr)
            g_state.slabs.push_back (slabslices::SliceBody (slab, *body, g_request.rule, storeys, slices));
        else
            g_state.slabs.push_back (slabslices::SliceSlab (slab, g_request.rule, storeys, slices));
        slabslices::Summary& summary = g_state.slabs.back ();
        summary.operators = operators != nullptr ? uint32_t (operators->size ()) : 0u;
        if (summary.operators != 0 && body == nullptr)
            ++waiting;
        if (!summary.problem.empty ())
            problems += "; " + (summary.id.empty () ? summary.guid : summary.id) + ": " + summary.problem;
        if (summary.slopedEdges != 0)
            problems += "; " + (summary.id.empty () ? summary.guid : summary.id) + ": " +
                        std::to_string (summary.slopedEdges) + " edge(s) trimmed off vertical, cut as vertical";
    }
    g_state.skipped = std::move (reading.skipped);
    // Drawn as its polygon until its body comes: right everywhere the operations are not.
    if (waiting != 0) {
        g_state.waiting = true;
        problems += "; " + std::to_string (waiting) +
                    " slab(s) cut by solid element operations drawn as their polygon until the 3D "
                    "model's body arrives (it must be in the 3D window: shown, on a visible layer)";
    }
    ++g_state.cuts;
    if (!Show (BuildLayer (slices, g_controls)))
        return;

    char line[200] = {};
    std::snprintf (line, sizeof (line), "%s: %zu slab(s) cut by %s into %u slice(s), %.1f m2", why,
                   g_state.slabs.size (), slabslices::CutName (g_request.rule.cut), g_state.slices, g_state.areaM2);
    std::string message = g_targets.empty () ? std::string (why) + ": nothing selected -- select the massing slabs, "
                                                                   "then switch the slices on"
                                             : std::string (line);
    if (!g_state.skipped.empty ())
        message += "; skipped " + std::to_string (g_state.skipped.size ()) + " (" + g_state.skipped[0].reason + ")";
    Say (message + problems);
}

// An edit to a slab taken, or to the storeys, re-cuts them all: a handful of header
// reads per tick, and the cut itself only when something moved.
void FollowSlabs ()
{
    if (g_targets.empty ())
        return;
    if (slabsource::Operators (g_targets) != g_operators) {
        CutSlabs ("a solid element operation changed");
        AskForBodies ();
        return;
    }
    if (slabsource::Stamps (Watched ()) != g_stamps) {
        CutSlabs ("a slab changed");
        AskForBodies ();
        return;
    }
    if (g_bodyPass && !ExtractionWorker::Get ().IsRunning ())
        AskForBodies ();
    // A pass handed over new bodies -- one asked for here, or the model watch's after an
    // edit the 3D model saw.
    const std::shared_ptr<const slabbodies::Bodies> bodies = slabbodies::Latest ();
    if (bodies != nullptr && bodies->generation != g_bodies && !OperatedSlabs ().empty ()) {
        CutSlabs ("the 3D model changed");
        return;
    }
    if (++g_ticks % kStoreyTicks == 0 && ReadStoreys ().levels != g_levels)
        CutSlabs ("the storeys changed");
}

// ---- the model ----------------------------------------------------------------------

void RebuildModel (const storeyslices::Snapshot& snapshot)
{
    g_built = snapshot.generation;
    g_state.snapshot = snapshot.generation;
    g_state.storeys = uint32_t (snapshot.storeys.size ());
    ++g_state.cuts;
    if (!Show (BuildLayer (FromStoreys (snapshot), g_controls)))
        return;
    g_state.waiting = false;
    char line[160] = {};
    std::snprintf (line, sizeof (line), "drawn: %u of %u storeys, %.1f m2 of slices", g_state.slices, g_state.storeys,
                   g_state.areaM2);
    Say (line);
}

// Ask the extraction worker for a full pass with its storey cuts.
void RequestCut ()
{
    ExtractionWorker& worker = ExtractionWorker::Get ();
    worker.SetStorySlicesWanted (true);
    if (worker.IsRunning ()) {
        g_awaitingPass = true;
        return; // its cuts come with the next full pass, asked for once this one ends
    }
    if (g_requests >= kMaxRequests) {
        g_state.waiting = false;
        Say ("no storey was cut after " + std::to_string (kMaxRequests) +
             " full passes: the model reaches no storey, or its passes do not finish");
        return;
    }
    ++g_requests;
    g_awaitingPass = true;
    g_state.waiting = true;
    Say ("asking the extraction for a full pass with its storey cuts");
    worker.Start (true);
}

void FollowModel ()
{
    const std::shared_ptr<const storeyslices::Snapshot> latest = storeyslices::Latest ();
    if (latest != nullptr && latest->generation != g_built) {
        g_awaitingPass = false;
        RebuildModel (*latest);
        return;
    }
    // A pass we asked for ended without cutting: ask again, a bounded number of times.
    if (latest == nullptr && g_awaitingPass && !ExtractionWorker::Get ().IsRunning ())
        RequestCut ();
}

void ApplyModel (bool refresh)
{
    const std::shared_ptr<const storeyslices::Snapshot> latest = storeyslices::Latest ();
    if (refresh || latest == nullptr) {
        g_requests = 0;
        g_built = latest != nullptr ? latest->generation : 0; // redrawn when the new pass lands
        if (latest != nullptr)
            RebuildModel (*latest); // the controls apply now; the fresh cut replaces it
        RequestCut ();
    }
    else {
        RebuildModel (*latest);
    }
}

// ---- the switch -----------------------------------------------------------------------

void CALLBACK Tick (HWND, UINT, UINT_PTR, DWORD)
{
    if (!g_enabled)
        return;
    if (g_request.source == Source::Model)
        FollowModel ();
    else
        FollowSlabs ();
}

void StopTimer ()
{
    if (g_timer != 0) {
        ::KillTimer (nullptr, g_timer);
        g_timer = 0;
    }
}

void Forget ()
{
    g_built = 0;
    g_requests = 0;
    g_awaitingPass = false;
    g_targets.clear ();
    g_stamps.clear ();
    g_levels.clear ();
    g_ticks = 0;
    g_operators.clear ();
    g_bodies = 0;
    g_bodyPass = false;
    slabbodies::Clear ();
    g_state = State {};
}

} // namespace

const char* SourceName (Source source)
{
    switch (source) {
        case Source::Selection:
            return "selection";
        case Source::Elements:
            return "elements";
        case Source::Model:
            return "model";
    }
    return "selection";
}

State Apply (bool enabled, const Request& request, const Controls& controls, bool refresh)
{
    if (!enabled) {
        const bool was = g_enabled;
        g_enabled = false;
        StopTimer ();
        Forget ();
        if (was && overlaylayers::Clear (kLayerName))
            overlaycontrol::PublishLayers ();
        if (was)
            Say ("off");
        return g_state;
    }
    const bool sourceChanged = !g_enabled || request.source != g_request.source;
    g_request = request;
    g_controls = controls;
    g_enabled = true;
    g_state.enabled = true;
    g_state.source = request.source;
    g_state.cut = request.rule.cut;
    g_state.waiting = false;
    if (g_timer == 0)
        g_timer = ::SetTimer (nullptr, 0, kTickMs, Tick);

    switch (request.source) {
        case Source::Model:
            g_targets.clear ();
            g_operators.clear ();
            slabbodies::Clear ();
            g_state.slabs.clear ();
            g_state.skipped.clear ();
            ApplyModel (refresh);
            break;
        case Source::Selection:
            // `on` takes the selection; `refresh` keeps the slabs taken, unless there are none.
            if (!refresh || sourceChanged || g_targets.empty ()) {
                std::vector<std::string> guids;
                std::string error;
                if (!slabsource::Selected (guids, error)) {
                    Say ("NOT CUT: " + error);
                    return g_state;
                }
                g_targets = std::move (guids);
            }
            CutSlabs (refresh ? "refresh" : "on");
            AskForBodies ();
            break;
        case Source::Elements:
            g_targets = request.elements;
            CutSlabs (refresh ? "refresh" : "on");
            AskForBodies ();
            break;
    }
    return g_state;
}

State Describe ()
{
    return g_state;
}

void OnProjectClosed ()
{
    storeyslices::Clear ();
    StopTimer ();
    g_enabled = false;
    Forget ();
}

void Shutdown ()
{
    StopTimer ();
    g_enabled = false;
}

} // namespace storysliceoverlay
} // namespace archviz
} // namespace geomsrv
