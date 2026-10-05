#include "APIEnvir.h"
#include "ACAPinc.h"
#include "ArchViz/MassingSlicesModel.hpp"
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
std::shared_ptr<const massingslices::Result> s_result;
storysliceoverlay::Controls s_display;
bool s_shown = true, s_styleDirty = false, s_bodyPass = false;
uint64_t s_bodies = 0;
std::string s_bodySignature;
std::string s_hoverFunction;

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
        if (!operators[i].empty ()) {
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
    const auto envelope = massinghybrid::Read ().preview;
    if (signature == s_signature && envelope == s_envelope && s_result && bodyGeneration == s_bodies && !s_styleDirty)
        return;
    s_styleDirty = false;
    s_bodies = bodyGeneration;
    s_signature = std::move (signature);
    s_envelope = envelope;
    massingslices::Result result;
    result.layer.name = massingslices::kLayer;
    std::string error;
    std::vector<massingslices::Input> inputs;
    if (overBudget)
        error = "Automatic preview accepts at most 128 selected/defined elements.";
    if (error.empty ()) {
        const auto reading = slabsource::Read (guids, storeys);
        for (const auto& skip : reading.skipped)
            if (skip.reason.find ("not a slab") == std::string::npos)
                result.note += skip.guid + ": " + skip.reason + ". ";
        for (const auto& slab : reading.slabs) {
            const auto index = size_t (std::lower_bound (guids.begin (), guids.end (), slab.guid) - guids.begin ());
            massingslices::Input input;
            input.slab = slab;
            if (index < operators.size () && !operators[index].empty ()) {
                const auto found =
                    bodies ? bodies->meshes.find (slab.guid) : std::map<std::string, Mesh>::const_iterator {};
                if (bodySignature == "operator-budget" || !bodies || found == bodies->meshes.end ()) {
                    result.note += "SEO slab awaiting a current 3D body (must be visible in the 3D model). ";
                    continue;
                }
                input.body = std::shared_ptr<const Mesh> (bodies, &found->second);
            }
            bool present = false;
            if (!metadata::storage::Read (slab.guid, input.metadata, present, error))
                break;
            inputs.push_back (std::move (input));
        }
    }
    const auto skipped = result.note;
    if (error.empty ())
        massingslices::Build (inputs, storeys, envelope ? &envelope->result : nullptr, result, error, s_display);
    if (error.empty () && envelope) {
        std::string coverageError;
        massingslices::Coverage (result, *envelope, coverageError);
        if (!coverageError.empty ())
            result.note += " " + coverageError;
    }
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
