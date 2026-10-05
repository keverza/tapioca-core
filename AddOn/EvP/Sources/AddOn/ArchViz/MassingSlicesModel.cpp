#include "APIEnvir.h"
#include "ACAPinc.h"
#include "ArchViz/MassingSlicesModel.hpp"
#include "ArchViz/MassingCollapseZone.hpp"
#include "ArchViz/MassingHybrid.hpp"
#include "ArchViz/MassingModel.hpp"
#include "ArchViz/SelectionMetadata.hpp"
#include "ArchViz/SlabSliceSource.hpp"
#include "ArchViz/OverlayController.hpp"
#include "ArchViz/OverlayInput.hpp"
#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/SlabBodies.hpp"
#include "ArchViz/ExtractionThread.hpp"
#include "ArchViz/ModelWatch.hpp"
#include "Metadata/MetadataStorage.hpp"

#include <algorithm>

namespace geomsrv::archviz::massingslicesmodel {
namespace {
UINT_PTR s_timer = 0;
uint64_t s_lastPoll = 0;
bool s_dirty = true, s_polling = false;
std::vector<std::string> s_selected;
std::string s_signature;
std::shared_ptr<const massingcalculation::Preview> s_envelope;
std::shared_ptr<const Mesh> s_projectionTerrain;
std::shared_ptr<const massingslices::Result> s_result;
storysliceoverlay::Controls s_display;
bool s_shown = true, s_styleDirty = false, s_bodyPass = false;
uint64_t s_bodies = 0;
std::string s_bodySignature;
std::string s_hoverFunction;
bool s_collapseShown = false;
std::string s_collapseNote;

void Highlight ()
{
    overlaylayers::Clear (massingslices::kHighlightLayer);
    if (!s_shown || !s_result || s_hoverFunction.empty ())
        return;
    overlaylayers::Layer layer;
    std::string error;
    if (massingslices::Highlight (*s_result, s_hoverFunction, layer, error))
        overlaylayers::Set (std::move (layer));
    else
        ArchVizLog ("MASSING HIGHLIGHT refused: " + error);
}

void Publish ()
{
    overlaycontrol::PublishLayers ();
    overlayinput::RequestLayout (overlayinput::View::Plan);
    overlayinput::RequestLayout (overlayinput::View::ThreeD);
}

void Update ()
{
    if (s_dirty) {
        s_selected = selectionmetadata::SelectedGuids ();
        s_dirty = false;
        s_signature.clear ();
    }
    const auto defined = massingmodel::Read ();
    auto guids = defined.guids[3];
    guids.insert (guids.end (), s_selected.begin (), s_selected.end ());
    std::sort (guids.begin (), guids.end ());
    guids.erase (std::unique (guids.begin (), guids.end ()), guids.end ());
    const bool overBudget = guids.size () > 128;
    std::string signature = overBudget ? "over-budget:" + std::to_string (guids.size ()) : std::string ();
    const auto storeys = ReadStoreys ();
    for (size_t i = 0; i < storeys.levels.size (); ++i)
        signature += std::to_string (storeys.indices[i]) + ":" + std::to_string (storeys.levels[i]) + ";";
    // Refuse before per-element ACAPI work: selecting thousands of elements must
    // not turn the idle timer into thousands of header/SEO reads each tick.
    const auto stamps = overBudget ? std::vector<uint64_t> {} : slabsource::Stamps (guids);
    const auto operators = overBudget ? std::vector<std::vector<std::string>> {} : slabsource::Operators (guids);
    std::vector<std::string> bodyTargets, watchedOperators;
    std::string bodySignature;
    for (size_t i = 0; i < stamps.size (); ++i) {
        signature += guids[i] + ":" + std::to_string (stamps[i]) + ";";
        for (const auto& guid : operators[i])
            signature += "SEO:" + guid + ";";
        // Facade slope classification requires the completed model surfaces even
        // without SEO. Keep this demand independent of the collapse-zone toggle.
        {
            bodyTargets.push_back (guids[i]);
            bodySignature += guids[i] + ":" + std::to_string (stamps[i]) + ";";
            for (const auto& guid : operators[i])
                bodySignature += "SEO:" + guid + ";";
            watchedOperators.insert (watchedOperators.end (), operators[i].begin (), operators[i].end ());
        }
    }
    if (watchedOperators.size () > 1024) {
        bodyTargets.clear ();
        bodySignature = "operator-budget";
    }
    else {
        const auto operatorStamps = slabsource::Stamps (watchedOperators);
        for (size_t i = 0; i < watchedOperators.size (); ++i)
            bodySignature += watchedOperators[i] + ":" + std::to_string (operatorStamps[i]) + ";";
        // SEO operation/link edits may leave the participating element records
        // unchanged. The extraction's model revision also invalidates held bodies.
        if (!bodyTargets.empty ())
            bodySignature += "model:" + std::to_string (modelwatch::CaptureStamp ());
    }
    slabbodies::Want (bodyTargets, "massing");
    if (bodySignature != s_bodySignature) {
        s_bodySignature = bodySignature;
        slabbodies::Invalidate (bodyTargets);
        s_bodyPass = !bodyTargets.empty ();
    }
    if (s_bodyPass && !ExtractionWorker::Get ().IsRunning ()) {
        s_bodyPass = false;
        ExtractionWorker::Get ().Start (true);
    }
    const auto bodies = slabbodies::Latest ();
    const uint64_t bodyGeneration = bodies && !bodyTargets.empty () ? bodies->generation : 0;
    signature += bodySignature;
    const auto calculation = massinghybrid::Read ();
    const auto envelope = calculation.preview;
    if (signature == s_signature && envelope == s_envelope && calculation.terrain == s_projectionTerrain && s_result &&
        bodyGeneration == s_bodies && !s_styleDirty)
        return;
    s_styleDirty = false;
    s_bodies = bodyGeneration;
    s_signature = std::move (signature);
    s_envelope = envelope;
    s_projectionTerrain = calculation.terrain;
    massingslices::Result result;
    result.layer.name = massingslices::kLayer;
    std::string error;
    std::vector<massingslices::Input> inputs;
    std::vector<massingslices::Input> zoneInputs;
    bool zoneMissing = overBudget;
    bool facadeMissing = overBudget;
    if (overBudget)
        error = "Automatic preview accepts at most 128 selected/defined elements.";
    if (error.empty ()) {
        const auto reading = slabsource::Read (guids, storeys);
        for (const auto& skip : reading.skipped)
            if (skip.reason.find ("not a slab") == std::string::npos) {
                result.note += skip.guid + ": " + skip.reason + ". ";
                zoneMissing = true;
                facadeMissing = true;
            }
        for (const auto& slab : reading.slabs) {
            const auto index = size_t (std::lower_bound (guids.begin (), guids.end (), slab.guid) - guids.begin ());
            massingslices::Input input;
            input.slab = slab;
            const auto found =
                bodies ? bodies->meshes.find (slab.guid) : std::map<std::string, Mesh>::const_iterator {};
            const bool hasBody = bodySignature != "operator-budget" && bodies && found != bodies->meshes.end ();
            if (hasBody)
                input.facadeBody = std::shared_ptr<const Mesh> (bodies, &found->second);
            else
                facadeMissing = true;
            if (slab.slopedEdges || (index < operators.size () && !operators[index].empty ())) {
                if (!hasBody) {
                    result.note += "SEO/sloped slab awaiting a current 3D body (must be visible in the 3D model). ";
                    zoneMissing = true;
                    continue;
                }
                input.body = input.facadeBody;
            }
            bool present = false;
            if (!metadata::storage::Read (slab.guid, input.metadata, present, error))
                break;
            if (s_collapseShown) {
                auto zone = input;
                if (!hasBody)
                    zoneMissing = true;
                else {
                    zone.body = std::shared_ptr<const Mesh> (bodies, &found->second);
                    zoneInputs.push_back (std::move (zone));
                }
            }
            inputs.push_back (std::move (input));
        }
    }
    overlaylayers::Clear (massingcollapse::kLayer);
    overlaylayers::Clear (massingcollapse::kProjectedLayer);
    s_collapseNote.clear ();
    if (s_collapseShown) {
        if (zoneMissing || !error.empty ())
            s_collapseNote = "Collapse zone awaiting complete current 3D slab bodies; no partial zone displayed.";
        else {
            massingcollapse::Result zone;
            const double drawingZ = envelope && envelope->result.hasMeanZ ? envelope->result.meanZ : 0;
            if (massingcollapse::Build (zoneInputs, drawingZ, zone, s_collapseNote)) {
                zone.layer.views = overlaylayers::Views::TwoD;
                if (!zone.layer.meshes.empty ())
                    overlaylayers::Set (std::move (zone.layer));
                s_collapseNote =
                    zoneInputs.empty ()
                        ? "No massing slabs defined/selected."
                        : "Current operated surfaces; local top minus local base height, merged into one fill.";
                if (!zoneInputs.empty ()) {
                    overlaylayers::Layer projected;
                    std::string projectionError;
                    if (!s_projectionTerrain)
                        s_collapseNote += " 3D projection awaits the selected current topography mesh.";
                    else if (massingcollapse::Project (zone, *s_projectionTerrain, projected, projectionError)) {
                        if (!projected.meshes.empty ())
                            overlaylayers::Set (std::move (projected));
                        s_collapseNote += " 3D zone projected onto topography; outside its mesh extent is undrawn.";
                    }
                    else
                        s_collapseNote += " " + projectionError;
                }
            }
        }
    }
    const auto skipped = result.note;
    if (error.empty ())
        massingslices::Build (inputs, storeys, envelope ? &envelope->result : nullptr, result, error, s_display);
    if (facadeMissing && error.empty ()) {
        result.hasFacade = false;
        result.facadeArea = 0;
        result.note +=
            " Facade awaiting all current 3D slab bodies; no partial area or unoperated substitute reported.";
    }
    if (error.empty () && envelope && skipped.empty ()) {
        std::string coverageError;
        massingslices::Coverage (result, *envelope, coverageError);
        if (!coverageError.empty ())
            result.note += " " + coverageError;
    }
    else if (!skipped.empty ())
        result.note += " Parcel coverage unavailable while slab sources are incomplete.";
    if (!skipped.empty ())
        result.note += " " + skipped;
    if (!error.empty ()) {
        result = {};
        result.layer.name = massingslices::kLayer;
        result.note = error;
        ArchVizLog ("MASSING SLICES  refused: " + error);
    }
    overlaylayers::Clear (massingslices::kLayer);
    if (s_shown && !result.rows.empty ())
        overlaylayers::Set (result.layer);
    s_result = std::make_shared<const massingslices::Result> (std::move (result));
    Highlight ();
    Publish (); // Errors and empty sets also change Stats; wake an idle HUD once.
}
} // namespace

void Changed ()
{
    s_dirty = true;
    if (s_timer == 0)
        s_timer = ::SetTimer (nullptr, 0, 250, [] (HWND, UINT, UINT_PTR, DWORD) { Poll (); });
}

void Poll ()
{
    if (s_polling)
        return;
    const uint64_t now = ::GetTickCount64 ();
    if (!s_dirty && !s_styleDirty && now - s_lastPoll < 250 && massinghybrid::Read ().preview == s_envelope)
        return;
    s_lastPoll = now;
    s_polling = true;
    if (s_timer == 0)
        Changed ();
    Update ();
    s_polling = false;
}

std::shared_ptr<const massingslices::Result> Read ()
{
    return s_result;
}

void Display (bool shown, const storysliceoverlay::Controls& controls)
{
    s_shown = shown;
    if (!shown)
        overlaylayers::Clear (massingslices::kHighlightLayer);
    s_display = controls;
    s_styleDirty = true;
    Poll ();
}

bool Shown ()
{
    return s_shown;
}

storysliceoverlay::Controls Controls ()
{
    return s_display;
}

void HoverFunction (const std::string& function)
{
    if (s_hoverFunction == function)
        return;
    s_hoverFunction = function;
    Highlight ();
    Publish ();
}

void CollapseZone (bool shown)
{
    if (shown == s_collapseShown)
        return;
    s_collapseShown = shown;
    s_signature.clear ();
    overlaylayers::Clear (massingcollapse::kLayer);
    overlaylayers::Clear (massingcollapse::kProjectedLayer);
    s_collapseNote.clear ();
    Changed ();
    Publish ();
}

std::string CollapseNote ()
{
    return s_collapseNote;
}

void Forget ()
{
    if (s_timer != 0) {
        ::KillTimer (nullptr, s_timer);
        s_timer = 0;
    }
    overlaylayers::Clear (massingslices::kLayer);
    s_selected.clear ();
    s_signature.clear ();
    s_envelope.reset ();
    s_result.reset ();
    s_hoverFunction.clear ();
    s_collapseShown = false;
    s_collapseNote.clear ();
    overlaylayers::Clear (massingcollapse::kLayer);
    overlaylayers::Clear (massingcollapse::kProjectedLayer);
    s_projectionTerrain.reset ();
    overlaylayers::Clear (massingslices::kHighlightLayer);
    slabbodies::Want ({}, "massing");
    s_bodies = 0;
    s_bodySignature.clear ();
    s_bodyPass = false;
    s_display = {};
    s_shown = true;
    s_dirty = true;
    s_lastPoll = 0;
}

void Shutdown ()
{
    Forget ();
}
} // namespace geomsrv::archviz::massingslicesmodel
