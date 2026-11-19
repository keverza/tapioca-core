#include "ArchViz/DiagramModel.hpp"
#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/DiagramReplay.hpp"
#include "ArchViz/MassingHybrid.hpp"
#include "ArchViz/MassingModel.hpp"
#include "ArchViz/MassingSlicesModel.hpp"
#include "ArchViz/ModelWatch.hpp"
#include "ArchViz/OverlayGuestText.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "Server/ServerState.hpp"

#include <windows.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>

namespace geomsrv::archviz::diagrammodel {
namespace {
std::mutex s_mutex; // guards s_controls, s_status
Controls s_controls;
Status s_status;
diagram::EntityId s_selected;
std::atomic<std::shared_ptr<const diagram::Publication>> s_publication;
std::atomic<std::shared_ptr<const std::vector<std::string>>> s_hidden;
std::atomic<bool> s_saveRequested { false };

// ---- main thread only ----
diagram::StoreyBindings s_bindings;
std::future<diagram::Prepared> s_job;
uint64_t s_requested = 0; // generation of the latest request
std::shared_ptr<const massingslices::Result> s_seenSlices;
std::shared_ptr<const diagram::FloorPlans> s_seenPlans;
std::string s_seenStoreys;
uint64_t s_slicesGeneration = 0, s_lastFollow = 0;
std::shared_ptr<const diagram::DiagramInputSnapshot> s_lastSnapshot;
std::string s_lastContent; // ContentSignature of s_lastSnapshot
std::shared_ptr<const diagram::Publication> s_hiddenFor;
bool s_hiddenWanted = false;

std::string StoreySignature (const ProjectStoreys& storeys)
{
    std::string signature;
    for (size_t i = 0; i < storeys.levels.size (); ++i)
        signature += std::to_string (i < storeys.indices.size () ? storeys.indices[i] : 0) + ":" +
                     std::to_string (storeys.levels[i]) + ":" +
                     std::to_string (i < storeys.floorIds.size () ? storeys.floorIds[i] : 0) + ";";
    return signature;
}

void SetState (const std::string& state, const std::vector<std::string>* notes = nullptr)
{
    std::lock_guard<std::mutex> lock (s_mutex);
    s_status.state = state;
    if (notes)
        s_status.notes = *notes;
}

void Save ()
{
    if (!s_saveRequested.exchange (false))
        return;
    std::string path, error;
    if (!s_lastSnapshot)
        error = "Nothing captured yet: turn Diagram Mode on first.";
    else {
        const auto* root = _wgetenv (L"LOCALAPPDATA");
        if (!root || !*root)
            error = "LOCALAPPDATA is unavailable.";
        else {
            try {
                const auto directory = std::filesystem::path (root) / "Tapioca" / "dumps";
                std::filesystem::create_directories (directory);
                const auto stamp = std::chrono::duration_cast<std::chrono::seconds> (
                                       std::chrono::system_clock::now ().time_since_epoch ())
                                       .count ();
                const auto file = directory / ("diagram-capture-" + std::to_string (stamp) + ".json");
                std::ofstream out (file, std::ios::binary | std::ios::trunc);
                out << diagram::WriteCapture (*s_lastSnapshot, "live capture from the Tapioca viewer");
                if (!out)
                    error = "Writing " + file.string () + " failed.";
                else
                    path = file.string ();
            }
            catch (const std::exception& exception) {
                error = exception.what ();
            }
        }
    }
    ArchVizLog (error.empty () ? "DIAGRAM capture saved: " + path : "DIAGRAM capture not saved: " + error);
    std::lock_guard<std::mutex> lock (s_mutex);
    s_status.savedPath = path;
    s_status.saveError = error;
}

// The hidden set follows the controls and the publication; a new vector only on change.
void UpdateHidden (const Controls& controls)
{
    const auto publication = s_publication.load ();
    const bool wanted = controls.enabled && controls.hideSource && publication;
    if (wanted == s_hiddenWanted && publication == s_hiddenFor)
        return;
    s_hiddenWanted = wanted;
    s_hiddenFor = publication;
    s_hidden.store (wanted ? std::make_shared<const std::vector<std::string>> (publication->representedGuids)
                           : nullptr);
}

void Adopt ()
{
    if (!s_job.valid () || s_job.wait_for (std::chrono::seconds (0)) != std::future_status::ready)
        return;
    diagram::Prepared prepared = s_job.get ();
    const uint64_t generation = prepared.publication ? prepared.publication->generation : 0;
    std::lock_guard<std::mutex> lock (s_mutex);
    s_status.building = false;
    if (prepared.publication && generation != s_requested) {
        ++s_status.discarded; // superseded: a newer request is already on its way
        return;
    }
    if (!prepared.publication) {
        ++s_status.refused;
        std::string why;
        for (auto issue : prepared.status.failures)
            why += (why.empty () ? "" : "; ") + std::string (diagram::Describe (issue));
        s_status.state = (s_publication.load () ? "Kept the last diagram: " : "No diagram: ") +
                         (why.empty () ? std::string ("the snapshot was refused") : why);
        s_status.notes = prepared.notes;
        return;
    }
    s_bindings = std::move (prepared.bindings);
    const auto& p = *prepared.publication;
    std::vector<std::string> kept;
    if (!s_selected.Empty () && p.plan.Find (s_selected) < 0) {
        kept.push_back ("Selection cleared: its storey is no longer in the diagram.");
        s_selected = {};
    }
    s_status.state = std::to_string (p.buildings) + " building(s), " + std::to_string (p.storeys) + " storey(s)" +
                     (p.domain == diagram::StoreyDomain::ProjectStories ? " on project stories" : " on massing floors");
    s_status.notes = p.notes;
    s_status.notes.insert (s_status.notes.begin (), kept.begin (), kept.end ());
    ++s_status.publications;
    s_publication.store (std::move (prepared.publication));
}

void Request (std::shared_ptr<const diagram::DiagramInputSnapshot> snapshot, std::string content)
{
    s_lastSnapshot = snapshot;
    s_lastContent = std::move (content);
    const uint64_t generation = ++s_requested;
    {
        std::lock_guard<std::mutex> lock (s_mutex);
        s_status.building = true;
    }
    // The worker sees values only: the snapshot, a copy of the bindings, the generation.
    s_job = std::async (std::launch::async, [snapshot, bindings = s_bindings, generation] () mutable {
        return diagram::Prepare (*snapshot, std::move (bindings), diagram::DefaultRequest (), generation);
    });
}
} // namespace

diagram::EntityId Selected ()
{
    std::lock_guard<std::mutex> lock (s_mutex);
    return s_selected;
}
void Select (const diagram::EntityId& id)
{
    std::lock_guard<std::mutex> lock (s_mutex);
    s_selected = id;
}
Controls ReadControls ()
{
    std::lock_guard<std::mutex> lock (s_mutex);
    return s_controls;
}
void SetControls (const Controls& controls)
{
    std::lock_guard<std::mutex> lock (s_mutex);
    s_controls = controls;
    s_controls.spacing.storeyGap = controls.spacing.GapOf ({});
    for (auto& [key, gap] : s_controls.spacing.buildingGap)
        gap = controls.spacing.GapOf (key);
}
diagram::DiagramView View ()
{
    diagram::DiagramView view;
    std::lock_guard<std::mutex> lock (s_mutex);
    if (!s_controls.enabled)
        return view;
    view.publication = s_publication.load ();
    view.spacing = s_controls.spacing;
    view.colouring = s_controls.colouring;
    view.detail = s_controls.detail;
    view.outlines = s_controls.outlines;
    view.connectors = s_controls.connectors;
    view.selected = s_selected;
    return view;
}
std::shared_ptr<const diagram::Publication> Read ()
{
    return s_publication.load ();
}
std::shared_ptr<const std::vector<std::string>> HiddenSource ()
{
    return s_hidden.load ();
}
Status ReadStatus ()
{
    std::lock_guard<std::mutex> lock (s_mutex);
    return s_status;
}
void RequestSave ()
{
    s_saveRequested.store (true);
}
void ReportDraw (uint64_t uploads, size_t uploadedBytes, size_t draws, double shownGap)
{
    std::lock_guard<std::mutex> lock (s_mutex);
    s_status.uploads = uploads;
    s_status.uploadedBytes = uploadedBytes;
    s_status.draws = draws;
    s_status.shownGap = shownGap;
}

void Follow ()
{
    if (!ServerState::Get ().modelOpen.load ()) {
        Forget ();
        return;
    }
    Adopt ();
    Save ();
    const Controls controls = ReadControls ();
    UpdateHidden (controls);
    // M2: Units and Rooms ask the Massing workflow to plan its floors (State::diagramCells).
    const auto hud = guesttext::HudState ();
    const bool cells = controls.enabled && controls.detail != diagram::Detail::Topology;
    if (hud->diagramCells != cells)
        hud->diagramCells = cells;
    if (!controls.enabled)
        return;
    const uint64_t now = ::GetTickCount64 ();
    if (now - s_lastFollow < 250)
        return; // massingslicesmodel itself polls no faster
    s_lastFollow = now;
    // The viewer has no overlay controller to follow the massing; these two are its follow.
    massinghybrid::Poll ();
    massingslicesmodel::Poll ();
    auto slices = massingslicesmodel::Read ();
    ProjectStoreys storeys = ReadStoreys ();
    std::string signature = StoreySignature (storeys);
    // The planned floors travel with every snapshot once any exist, whatever the detail shown:
    // switching the detail then never rebuilds, it only picks another uploaded body.
    auto plans = overlayhud::DiagramFloorPlans (*hud);
    const bool changed =
        slices != s_seenSlices || plans != s_seenPlans || signature != s_seenStoreys || !s_lastSnapshot;
    if (!changed || s_job.valid ())
        return; // an in-flight build finishes first; the next tick sees the change again
    // ⚠️ A NEW POINTER IS NOT A NEW MASSING. massingslicesmodel republishes whenever its
    // body signature moves, and that signature carries the model capture stamp, which
    // every extraction pass bumps -- so an idle viewer saw a fresh Result each pass and
    // rebuilt and re-uploaded an identical diagram forever (#201, 2026-11-18). The build
    // is keyed on the captured content; the stamps alone never start one.
    const bool slicesMoved = slices != s_seenSlices;
    s_seenSlices = slices;
    s_seenPlans = plans;
    s_seenStoreys = std::move (signature);
    diagram::RevisionSet revisions;
    revisions.projectEpoch = massingmodel::ProjectEpoch ();
    revisions.modelStamp = modelwatch::CaptureStamp ();
    revisions.slices = s_slicesGeneration + (slicesMoved ? 1 : 0);
    revisions.topology = plans ? plans->revision : 0;
    auto snapshot = std::make_shared<const diagram::DiagramInputSnapshot> (
        diagram::Capture (std::move (slices), std::move (storeys), revisions, std::move (plans)));
    std::string content = diagram::ContentSignature (*snapshot);
    if (s_lastSnapshot && content == s_lastContent) {
        std::lock_guard<std::mutex> lock (s_mutex);
        ++s_status.unchanged;
        return;
    }
    s_slicesGeneration = revisions.slices;
    if (!snapshot->slices)
        SetState ("Waiting for a published massing result.");
    Request (std::move (snapshot), std::move (content));
}

void Forget ()
{
    if (s_job.valid ())
        s_job.wait (); // pure and short; never leave a worker writing into a forgotten session
    s_job = {};
    s_bindings = {};
    s_requested = 0;
    s_seenSlices.reset ();
    s_seenPlans.reset ();
    s_seenStoreys.clear ();
    guesttext::HudState ()->diagramCells = false;
    s_slicesGeneration = 0;
    s_lastFollow = 0;
    s_lastSnapshot.reset ();
    s_lastContent.clear ();
    s_hiddenFor.reset ();
    s_hiddenWanted = false;
    s_publication.store (nullptr);
    s_hidden.store (nullptr);
    s_saveRequested.store (false);
    std::lock_guard<std::mutex> lock (s_mutex);
    s_controls = {};
    s_status = {};
    s_selected = {};
}
} // namespace geomsrv::archviz::diagrammodel
