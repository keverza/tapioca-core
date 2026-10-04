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
    for (size_t i = 0; i < stamps.size (); ++i) {
        signature += guids[i] + ":" + std::to_string (stamps[i]) + ";";
        for (const auto& guid : operators[i])
            signature += "SEO:" + guid + ";";
    }
    const auto envelope = massinghybrid::Read ().preview;
    if (signature == s_signature && envelope == s_envelope && s_result)
        return;
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
            if (index < operators.size () && !operators[index].empty ()) {
                result.note += "SEO slab omitted: a current operated body is required for an honest slice. ";
                continue;
            }
            massingslices::Input input;
            input.slab = slab;
            bool present = false;
            if (!metadata::storage::Read (slab.guid, input.metadata, present, error))
                break;
            inputs.push_back (std::move (input));
        }
    }
    const auto skipped = result.note;
    if (error.empty ())
        massingslices::Build (inputs, storeys, envelope ? &envelope->result : nullptr, result, error);
    if (!skipped.empty ())
        result.note += " " + skipped;
    if (!error.empty ()) {
        result = {};
        result.layer.name = massingslices::kLayer;
        result.note = error;
        ArchVizLog ("MASSING SLICES  refused: " + error);
    }
    overlaylayers::Clear (massingslices::kLayer);
    if (!result.rows.empty ())
        overlaylayers::Set (result.layer);
    s_result = std::make_shared<const massingslices::Result> (std::move (result));
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
    if (!s_dirty && now - s_lastPoll < 250 && massinghybrid::Read ().preview == s_envelope)
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
    s_dirty = true;
    s_lastPoll = 0;
}

void Shutdown ()
{
    Forget ();
}
} // namespace geomsrv::archviz::massingslicesmodel
