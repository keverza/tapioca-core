// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. What this says about the overlay obeys §7: deltas, not
// totals, and nothing printed that was not measured.
// ArchViz/OverlayHudModel -- see the header.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/OverlayHudModel.hpp"
#include "ArchViz/OverlayVisibility.hpp"

#include "ArchViz/Dxgi/PlanGuest.hpp"
#include "ArchViz/Dxgi/PrelockHud.hpp"
#include "ArchViz/Dxgi/SceneGuest.hpp"
#include "ArchViz/ExtractionThread.hpp"
#include "ArchViz/InjectedOverlayRuntime.hpp"
#include "ArchViz/OverlayAnnotations.hpp" // Settings' displays: the Watch annotations
#include "ArchViz/OverlayController.hpp"
#include "ArchViz/OverlayGuestText.hpp"
#include "ArchViz/OverlayInput.hpp"
#include "ArchViz/OverlayLayers.hpp"
#include "ArchViz/OverlayRuntimeReport.hpp"
#include "ArchViz/PlanOverlayRuntime.hpp"
#include "ArchViz/SectionModel.hpp"
#include "ArchViz/SelectionMetadata.hpp"
#include "ArchViz/MassingModel.hpp"
#include "ArchViz/MassingHybrid.hpp"
#include "ArchViz/MassingSlicesModel.hpp"
#include "ArchViz/StorySliceOverlay.hpp" // Settings' displays: the storey slices
#include "Metadata/MetadataExtractor.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace overlayhudmodel {

namespace {

using hudshell::Card;
using hudshell::Figure;

// How many selected elements the Selection page lists by name; the rest are counted.
constexpr size_t kListed = 12;
// A colour that says something needs attention, and one that says something is wrong.
constexpr uint32_t kAmber = 0xD9822BFFu;
constexpr uint32_t kRed = 0xD64545FFu;

// ---- the selection, read when Archicad says it changed ----------------------------------
// ⚠️ AND WHEN THE HUD WROTE ITS METADATA: the writer lays the page out again with what the
// elements then hold (SelectionMetadata.hpp `Request`'s `done`).
bool g_selectionDirty = true;
hudshell::SelectionPage g_selection;
hudmeta::Page g_metadata;
// The selected massing slabs' building section, and what of it is on the 3D overlay: the run
// shown, of which reading (moved on every read).
sectionmodel::Reading g_section;
uint64_t g_sectionReads = 0;
hudsection::Run g_shownRun;
uint64_t g_shownReads = 0;

const hudshell::SelectionPage& Selection ()
{
    if (!g_selectionDirty)
        return g_selection;
    g_selectionDirty = false;
    g_selection = hudshell::SelectionPage {};
    g_selection.known = true;
    g_metadata = hudmeta::Page {};
    g_metadata.known = true;
    const std::vector<std::string> guids = selectionmetadata::SelectedGuids ();
    g_selection.count = uint32_t (guids.size ());
    // The section: its floors picked are another building's when its slabs are others.
    sectionmodel::Reading section = sectionmodel::Read (guids);
    if (section.slabs != g_section.slabs)
        overlayhud::SetPickedFloors (*guesttext::HudState (), hudsection::Run {});
    g_section = std::move (section);
    ++g_sectionReads;
    if (guids.empty ())
        return g_selection;
    const std::vector<std::string> listed (guids.begin (), guids.begin () + (std::min) (guids.size (), kListed));
    g_metadata = selectionmetadata::Read (listed, g_selection.count);
    const std::shared_ptr<const MetaSet> facts = ExtractMetadataFor (listed, MetaLevel::Basic);
    for (const std::string& guid : listed) {
        hudshell::SelectedElement element;
        element.guid = guid;
        if (facts != nullptr)
            if (const ElementMeta* meta = facts->Find (guid)) {
                element.type = meta->typeName;
                element.id = meta->elemId;
                element.layer = meta->layer;
                element.storey = meta->story;
            }
        g_selection.elements.push_back (std::move (element));
    }
    return g_selection;
}

// ---- rates over a second or more (§7) ---------------------------------------------------
struct Rate {
    std::chrono::steady_clock::time_point at {};
    uint64_t last = 0;
    double perSecond = -1.0; // -1: not measured yet
    void Note (uint64_t total)
    {
        const auto now = std::chrono::steady_clock::now ();
        if (at == std::chrono::steady_clock::time_point {} || total < last) {
            at = now;
            last = total;
            return;
        }
        const double seconds = std::chrono::duration<double> (now - at).count ();
        if (seconds < 1.0)
            return;
        perSecond = double (total - last) / seconds;
        at = now;
        last = total;
    }
};
Rate g_composes3D, g_presentsPlan, g_drawnPlan, g_prelockDrawn;

std::string Format (const char* format, double value)
{
    char text[64] = {};
    std::snprintf (text, sizeof (text), format, value);
    return text;
}

std::string RateText (const Rate& rate, const char* unit)
{
    return rate.perSecond < 0.0 ? std::string ("measuring") : Format ("%.1f", rate.perSecond) + " " + unit;
}

std::string LayersText ()
{
    const std::vector<std::shared_ptr<const overlaylayers::Layer>> layers = overlaylayers::Layers ();
    const size_t hidden = overlayhud::HiddenLayers (*guesttext::HudState ()).size ();
    return std::to_string (layers.size ()) + (hidden > 0 ? " (" + std::to_string (hidden) + " hidden)" : "");
}

// ---- the dock's circles (HudShell.hpp `Circle`) ---------------------------------------------
// ⚠️ ATTENTION IS MEASURED, NOT ASSUMED: the camera is found in the frames Archicad draws, and a
// still 3D window draws none -- the cold start's redraws are spent by then (OverlayRedrawBudget)
// -- so the circle blinks only once the model frames have not moved for a while.
constexpr double kAttentionSeconds = 4.0;
uint64_t g_framesSeen = 0;
std::chrono::steady_clock::time_point g_framesMovedAt {};

// The model read for the overlay: how far, 0 to 1, and in words; false while nothing is read.
bool Reading (float& progress, std::string& words)
{
    const ExtractionWorker::Progress p = ExtractionWorker::Get ().Snapshot ();
    if (!p.running || p.total == 0 || p.done)
        return false;
    const uint32_t read = (std::min) (p.extracted + p.empty, p.total);
    progress = float (read) / float (p.total);
    words = "Reading the model: " + std::to_string (read) + " of " + std::to_string (p.total) + " elements";
    return true;
}

// The model being read for the overlay, as a card.
void Extraction (std::vector<Card>& cards)
{
    Card card;
    float progress = 0.0f;
    if (!Reading (progress, card.progressText))
        return;
    card.title = "Reading the model";
    card.progress = progress;
    cards.push_back (std::move (card));
}

hudshell::Circle ViewerCircle ()
{
    hudshell::Circle circle; // Off: the viewer is closed while an overlay runs
    circle.tip = "The separate viewer: press to switch to it (the overlay closes)";
    return circle;
}

hudshell::Circle OverlayCircle3D (const overlayruntime::Health& health)
{
    hudshell::Circle circle;
    const overlaycontrol::Outcome intent = overlaycontrol::Describe (overlaycontrol::Overlay::ThreeD);
    const auto now = std::chrono::steady_clock::now ();
    if (health.modelFramesSeen != g_framesSeen || g_framesMovedAt == std::chrono::steady_clock::time_point {}) {
        g_framesSeen = health.modelFramesSeen;
        g_framesMovedAt = now;
    }
    if (!intent.ok && !intent.retryable) {
        circle.phase = hudshell::Phase::Error;
        circle.tip = "The 3D overlay did not start (" + intent.code + "): " + intent.message;
        return circle;
    }
    if (!health.running) {
        circle.phase = hudshell::Phase::Busy;
        circle.tip = intent.message;
        return circle;
    }
    if (health.camera != overlayruntime::CameraState::Locked) {
        const double still = std::chrono::duration<double> (now - g_framesMovedAt).count ();
        circle.phase = still > kAttentionSeconds ? hudshell::Phase::Attention : hudshell::Phase::Busy;
        circle.tip =
            still > kAttentionSeconds
                ? "Orbit, pan or zoom the 3D window: the overlay finds Archicad's camera in the frames it draws"
                : "Finding Archicad's camera";
        return circle;
    }
    std::string words;
    if (Reading (circle.progress, words)) {
        circle.phase = hudshell::Phase::Busy;
        circle.tip = words;
        return circle;
    }
    if (health.host == overlayruntime::HostState::Dirty || health.host == overlayruntime::HostState::Extracting) {
        circle.phase = hudshell::Phase::Busy;
        circle.tip = "The model changed: reading it again";
        return circle;
    }
    circle.phase = hudshell::Phase::Ready;
    circle.tip = "The 3D overlay: press to hide or show it";
    return circle;
}

hudshell::Circle OverlayCirclePlan (const planruntime::Status& status)
{
    hudshell::Circle circle;
    const overlaycontrol::Outcome intent = overlaycontrol::Describe (overlaycontrol::Overlay::TwoD);
    if (!intent.ok && !intent.retryable) {
        circle.phase = hudshell::Phase::Error;
        circle.tip = "The 2D overlay did not start (" + intent.code + "): " + intent.message;
    }
    else if (!status.running) {
        circle.phase = hudshell::Phase::Busy;
        circle.tip = intent.message;
    }
    else {
        circle.phase = hudshell::Phase::Ready;
        circle.tip = "The 2D overlay: press to hide or show it";
    }
    return circle;
}

overlayhud::OwnPages ThreeD ()
{
    overlayhud::OwnPages pages;
    pages.standalone = true;
    const overlayruntime::Health health = overlayruntime::GetHealth ();
    g_composes3D.Note (health.overlayDraws);
    const bool locked = health.camera == overlayruntime::CameraState::Locked;
    pages.overlay = OverlayCircle3D (health);
    pages.viewer = ViewerCircle ();

    Card overlay;
    overlay.title = "Overlay - 3D";
    overlay.figures.push_back ({ "Camera", overlayruntime::CameraStateName (health.camera), locked ? 0u : kAmber });
    overlay.figures.push_back ({ "Model", std::string (overlayruntime::HostStateName (health.host)) +
                                              (health.hostOpaqueTriangles > 0
                                                   ? ", " + std::to_string (health.hostOpaqueTriangles) + " triangles"
                                                   : std::string ()) });
    overlay.figures.push_back ({ "Layers", LayersText () });
    if (!locked && !health.blockedAt.empty ())
        overlay.note = "Waiting at " + health.blockedAt;
    pages.debug.push_back (std::move (overlay));
    Extraction (pages.stats);

    // ---- Debug: what one Archicad frame costs, ours and Archicad's own --------------------
    // ⚠️ RATES ARE DEBUG'S, NEVER STATS': Stats asks for a frame when it changes (the
    // controller's heartbeat), and a figure that moves every second would ask every second.
    Card frame;
    frame.title = "Frame";
    frame.figures.push_back ({ "Composing", RateText (g_composes3D, "a second") });
    dxgi::composetiming::Window cost;
    uint64_t age = 0;
    if (overlayruntime::report::LastCost (cost, age)) {
        if (cost.intervals > 0)
            frame.figures.push_back (
                { "Archicad frame", "<= " + Format ("%.1f", cost.intervalP50Ms) + " ms p50 (" +
                                        Format ("%.0f", cost.intervalP50Ms > 0.0 ? 1000.0 / cost.intervalP50Ms : 0.0) +
                                        " fps)" });
        if (cost.timed > 0)
            frame.figures.push_back ({ "Overlay GPU", "<= " + Format ("%.2f", cost.gpuP50Ms) +
                                                          " ms p50, <= " + Format ("%.2f", cost.gpuP95Ms) + " p95" });
        if (cost.cpuFrames > 0)
            frame.figures.push_back ({ "Overlay CPU", Format ("%.2f", cost.cpuMeanMs) + " ms mean" });
        frame.note = std::to_string (cost.timed + cost.untimed) + " frames, measured " +
                     Format ("%.0f", double (age) / 1000.0) + " s ago";
    }
    else {
        frame.note = "Nothing composed yet: the cost is measured once the overlay draws";
    }
    pages.debug.push_back (std::move (frame));

    const dxgi::sceneguest::Stats guest = dxgi::sceneguest::GetStats ();
    Card surface;
    surface.title = "Surface";
    if (health.targetWidth > 0)
        surface.figures.push_back (
            { "Size", std::to_string (health.targetWidth) + " x " + std::to_string (health.targetHeight) + " px" });
    surface.figures.push_back (
        { "GPU", guest.adapter[0] != 0 ? std::string (guest.adapter) : std::string ("not attached yet") });
    surface.figures.push_back ({ "Guest draw", std::to_string (guest.lastDrawMicroseconds) + " us last, " +
                                                   std::to_string (guest.drawMicroseconds) + " us mean" });
    if (guest.failure != nullptr && guest.failure[0] != 0)
        surface.figures.push_back ({ "Failed", guest.failure, kRed });
    pages.debug.push_back (std::move (surface));

    Card camera;
    camera.title = "Camera";
    camera.figures.push_back ({ "State", overlayruntime::CameraStateName (health.camera) });
    camera.figures.push_back ({ "Blocked at", health.blockedAt.empty () ? std::string ("-") : health.blockedAt });
    camera.figures.push_back ({ "Model frames", std::to_string (health.modelFramesSeen) });
    camera.figures.push_back ({ "Re-acquired", std::to_string (health.reacquisitions) });
    // The HUD drawn alone before the camera (Dxgi/PrelockHud.hpp): how often, and why not.
    const dxgi::prelockhud::Stats prelock = dxgi::prelockhud::GetStats ();
    g_prelockDrawn.Note (prelock.drawn);
    if (!locked) {
        camera.figures.push_back ({ "HUD alone", RateText (g_prelockDrawn, "a second") });
        if (prelock.failed + prelock.noTarget > 0)
            camera.figures.push_back ({ "HUD alone refused",
                                        std::to_string (prelock.failed) + " failed, " +
                                            std::to_string (prelock.noTarget) + " without a target",
                                        kRed });
    }
    pages.debug.push_back (std::move (camera));
    return pages;
}

overlayhud::OwnPages Plan ()
{
    overlayhud::OwnPages pages;
    pages.standalone = true;
    const planruntime::Status status = planruntime::GetStatus ();
    g_presentsPlan.Note (status.canvasPresents);
    g_drawnPlan.Note (status.drawn);
    pages.overlay = OverlayCirclePlan (status);
    pages.viewer = ViewerCircle ();

    Card overlay;
    overlay.title = "Overlay - floor plan";
    overlay.figures.push_back ({ "Storey", std::to_string (status.storey) });
    overlay.figures.push_back (
        { "Walls", std::to_string (status.rings) + " outlines, " + std::to_string (status.segments) + " edges" });
    overlay.figures.push_back ({ "Layers", LayersText () });
    if (!status.lastError.empty ()) {
        overlay.note = status.lastError;
        overlay.noteRgba = kRed;
    }
    pages.stats.push_back (std::move (overlay));

    const dxgi::planguest::Stats guest = dxgi::planguest::GetStats ();
    Card frame;
    frame.title = "Frame";
    frame.figures.push_back ({ "Plan presents", RateText (g_presentsPlan, "a second") });
    frame.figures.push_back ({ "Overlay drawn", RateText (g_drawnPlan, "a second") });
    frame.figures.push_back ({ "Guest draw", std::to_string (guest.lastDrawMicroseconds) + " us last, " +
                                                 std::to_string (guest.drawMicroseconds) + " us mean" });
    pages.debug.push_back (std::move (frame));

    Card surface;
    surface.title = "Surface";
    surface.figures.push_back (
        { "Size", std::to_string (status.canvasWidth) + " x " + std::to_string (status.canvasHeight) + " px" });
    surface.figures.push_back ({ "DPI scale", Format ("%.2f", status.dpi) });
    surface.figures.push_back ({ "GPU", guest.adapter.empty () ? std::string ("not attached yet") : guest.adapter });
    if (!guest.lastError.empty ())
        surface.figures.push_back ({ "Failed", guest.lastError, kRed });
    pages.debug.push_back (std::move (surface));
    return pages;
}

} // namespace

overlayhud::OwnPages Pages (overlayinput::View view)
{
    overlayhud::OwnPages pages = view == overlayinput::View::ThreeD ? ThreeD () : Plan ();
    pages.selection = Selection ();
    pages.metadata = g_metadata;
    pages.massing = massingmodel::Read ();
    const auto calculation = massinghybrid::Read ();
    pages.massing.calculationBusy = calculation.busy;
    pages.massing.calculationNote = calculation.note;
    pages.massing.preview = calculation.preview;
    if (calculation.preview) {
        const auto& result = calculation.preview->result;
        Card card;
        card.title = "Massing envelope";
        if (calculation.preview->parcels.size () > 1) {
            card.figures.push_back ({ "Parcels", std::to_string (calculation.preview->parcels.size ()) });
            card.figures.push_back ({ "Envelopes", std::to_string (result.layer.meshes.size ()) });
        }
        card.figures.push_back ({ "Parcel area", Format ("%.2f m2", result.parcelArea) });
        card.figures.push_back (
            { calculation.preview->parcels.size () > 1 ? "Allowed footprints (parcel sum)" : "Allowed footprint",
              Format ("%.2f m2", result.allowedArea) });
        if (result.hasMeanASL)
            card.figures.push_back ({ "Mean elevation ASL", Format ("%.2f m", result.meanASL) });
        else if (result.hasMeanZ)
            card.figures.push_back ({ "Mean elevation (project Z)", Format ("%.2f m", result.meanZ) });
        card.note = result.hasEnvelope ? "Shared Python envelope; allowed story areas are below." : result.note;
        if (calculation.preview->parcels.size () > 1)
            card.note += " Parcel areas sum; overlapping envelopes count once for allowed story areas.";
        pages.stats.push_back (std::move (card));
    }
    pages.section = g_section.section;
    if (const auto slices = massingslicesmodel::Read (); slices && (!slices->rows.empty () || !slices->note.empty ())) {
        pages.massingStats = slices;
        std::vector<std::string> selected;
        for (const auto& element : g_selection.elements)
            selected.push_back (element.guid);
        pages.section = hudsection::Filter (slices->section, selected);
        for (const auto& page : slices->heightControls)
            if (std::find (selected.begin (), selected.end (), page.element) != selected.end ())
                pages.storyHeights.push_back (page);
        for (auto& field : pages.metadata.fields)
            if (field.id == "massing.story") {
                std::map<std::string, size_t> counts;
                for (const auto& row : slices->rows)
                    ++counts[row.guid];
                field.set = !g_selection.elements.empty ();
                field.text.clear ();
                for (const auto& element : g_selection.elements) {
                    const auto count = counts.find (element.guid);
                    if (count == counts.end ()) {
                        field.set = false;
                        field.text.clear ();
                        break;
                    }
                    const auto text = std::to_string (count->second);
                    if (!field.text.empty () && field.text != text) {
                        field.text = "mixed";
                        break;
                    }
                    field.text = text;
                }
            }
        Card card;
        card.title = "Massing story slices";
        card.figures.push_back ({ "Slices", std::to_string (slices->rows.size ()) });
        card.figures.push_back ({ "Slab slice area sum", Format ("%.2f m2", slices->rawArea) });
        const double total = slices->clipped ? slices->allowedArea : slices->rawArea;
        const double first = slices->clipped ? slices->firstFloorArea : slices->rawFirstFloorArea;
        card.figures.push_back ({ "1st floor area", Format ("%.2f m2", first) });
        card.figures.push_back ({ "Total floor area", Format ("%.2f m2", total) });
        card.figures.push_back ({ "Gross area (total x 0.78)", Format ("%.2f m2", total * 0.78) });
        card.figures.push_back ({ "Sellable area (total x 0.71)", Format ("%.2f m2", total * 0.71) });
        card.figures.push_back ({ "Slab volume (slice estimate)", Format ("%.2f m3", slices->rawVolume) });
        if (slices->clipped)
            card.figures.push_back ({ "Allowed volume (slice estimate)", Format ("%.2f m3", slices->allowedVolume) });
        if (slices->hasCoverage) {
            card.figures.push_back ({ "Built footprint in parcels", Format ("%.2f m2", slices->builtArea) });
            card.figures.push_back ({ "Unbuilt parcel area", Format ("%.2f m2", slices->unbuiltArea) });
            card.figures.push_back (
                { "Built / unbuilt ratio", slices->unbuiltArea > 1e-9
                                               ? Format ("%.3f", slices->builtArea / slices->unbuiltArea)
                                               : "n/a (fully built)" });
        }
        if (calculation.preview) {
            const double parcel = calculation.preview->result.parcelArea;
            card.figures.push_back ({ "Parcel / 1st floor area", first > 0 ? Format ("%.3f", parcel / first) : "n/a" });
            card.figures.push_back (
                { "Parcel / (total x 0.78)", total > 0 ? Format ("%.3f", parcel / (total * 0.78)) : "n/a" });
        }
        card.figures.push_back (
            { "Facade area (exposed slab walls)", slices->hasFacade ? Format ("%.2f m2", slices->facadeArea) : "n/a" });
        if (slices->clipped) {
            card.figures.push_back ({ "Allowed slice area sum", Format ("%.2f m2", slices->allowedArea) });
            if (calculation.preview && calculation.preview->result.parcelArea > 0)
                card.figures.push_back (
                    { "Allowed floor-area / parcel ratio",
                      Format ("%.3f", slices->allowedArea / calculation.preview->result.parcelArea) });
        }
        card.note = slices->note;
        card.note += " Volume estimates sum slice area x height to the next floor/slab top; not exact SEO body volume.";
        pages.stats.push_back (std::move (card));
    }
    pages.console = hudconsole::Entries (); // the Debug tab's console: what to check when something fails
    // The add-on's own displays as they are: Settings switches and styles them (ApplyDisplays).
    const storysliceoverlay::State slices = storysliceoverlay::Describe ();
    pages.displays.wireframeOn = overlayvisibility::WireframeShown ();
    pages.displays.slicesOn = slices.enabled;
    pages.displays.massingSlicesOn = massingslicesmodel::Shown ();
    pages.displays.slicesFromModel = storysliceoverlay::LastRequest ().source == storysliceoverlay::Source::Model;
    pages.displays.slices = massingslicesmodel::Controls ();
    pages.displays.slicesSaid = slices.message;
    pages.displays.annotationsOn = overlayannotations::Describe ().enabled;
    return pages;
}

void ApplyDisplays (const overlayhud::Displays& displays)
{
    overlayvisibility::SetWireframeShown (displays.wireframeOn);
    massingslicesmodel::Display (displays.massingSlicesOn, displays.slices);
    namespace slices = storysliceoverlay;
    const slices::State now = slices::Describe ();
    slices::Request request = slices::LastRequest ();
    const slices::Source source = displays.slicesFromModel ? slices::Source::Model : slices::Source::Selection;
    if (!displays.slicesOn) {
        if (now.enabled)
            slices::Apply (false, slices::Request {}, displays.slices, false);
    }
    else if (!now.enabled || request.source != source) {
        // On, or from elsewhere: the selected slabs taken now, or the model's cut asked for.
        request.source = source;
        request.elements.clear ();
        slices::Apply (true, request, displays.slices, false);
    }
    else {
        slices::Restyle (displays.slices);
    }
    if (displays.annotationsOn != overlayannotations::Describe ().enabled)
        overlayannotations::Apply (displays.annotationsOn);
    overlayinput::RequestLayout (overlayinput::View::ThreeD);
    overlayinput::RequestLayout (overlayinput::View::Plan);
}

overlayhud::Engine* Prepare (overlayinput::View view)
{
    overlayhud::Engine* const hud = guesttext::Hud (view);
    if (hud != nullptr)
        hud->SetOwnPages (Pages (view));
    return hud;
}

void SelectionChanged ()
{
    massingslicesmodel::Changed ();
    massingmodel::Changed ();
    g_selectionDirty = true;
    overlayinput::RequestLayout (overlayinput::View::ThreeD);
    overlayinput::RequestLayout (overlayinput::View::Plan);
}

void FollowFloors (const hudsection::Run& run)
{
    if (run == g_shownRun && g_shownReads == g_sectionReads && (run.Empty () || sectionmodel::Shown ()))
        return;
    g_shownRun = run;
    g_shownReads = g_sectionReads;
    if (run.Empty ())
        sectionmodel::Hide ();
    else
        sectionmodel::Show (g_section.slices, run, hudshell::PlainLook ().accentRgba);
}

void Forget ()
{
    massingslicesmodel::Forget ();
    massingmodel::Forget ();
    g_framesSeen = 0;
    g_framesMovedAt = std::chrono::steady_clock::time_point {};
    g_selectionDirty = true;
    g_selection = hudshell::SelectionPage {};
    g_metadata = hudmeta::Page {};
    g_section = sectionmodel::Reading {};
    g_shownRun = hudsection::Run {};
    sectionmodel::Forget ();
    g_composes3D = Rate {};
    g_presentsPlan = Rate {};
    g_drawnPlan = Rate {};
    g_prelockDrawn = Rate {};
}

void WakeOnConsole (bool on)
{
    if (!on) {
        hudconsole::SetListener ({});
        return;
    }
    hudconsole::SetListener ([] () {
        // One layout for a burst: the next entry posts again only once this one has run, and
        // not twice in half a second -- an entry said by a layout must not lay the HUD out again
        // and again with every new number in it.
        static std::atomic<bool> pending { false };
        static std::atomic<int64_t> lastMs { 0 };
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds> (
                                std::chrono::steady_clock::now ().time_since_epoch ())
                                .count ();
        if (now - lastMs.load () < 500 || pending.exchange (true))
            return;
        lastMs = now;
        const bool posted = selectionmetadata::Later ([] () {
            pending = false;
            overlayinput::RequestLayout (overlayinput::View::ThreeD);
            overlayinput::RequestLayout (overlayinput::View::Plan);
        });
        if (!posted)
            pending = false; // no window (unloading): nothing to wake, and no burst held shut
    });
}

} // namespace overlayhudmodel
} // namespace archviz
} // namespace geomsrv
