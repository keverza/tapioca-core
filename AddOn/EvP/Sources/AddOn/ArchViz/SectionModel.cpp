// ArchViz/SectionModel -- see the header.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/SectionModel.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/HudConsole.hpp" // the Debug tab's console: what the user checks when something fails
#include "ArchViz/ExtractionStorySlices.hpp"
#include "ArchViz/OverlayController.hpp"
#include "ArchViz/OverlayLayers.hpp"
#include "ArchViz/SelectionMetadata.hpp"
#include "ArchViz/MassingModel.hpp"
#include "ArchViz/SlabSliceSource.hpp"
#include "ArchViz/SurfaceSwitch.hpp"
#include "Metadata/MetadataStorage.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>

namespace geomsrv {
namespace archviz {
namespace sectionmodel {

namespace meta = metadata;

namespace {

std::mutex g_mutex; // the published section
hudsection::Section g_published;
bool g_asked = false;
std::atomic<bool> g_pending { false }; // a publish posted and not yet run

} // namespace

Reading Read (const std::vector<std::string>& guids)
{
    Reading out;
    if (guids.empty ())
        return out;
    const auto model = massingmodel::Read ();
    std::vector<std::string> selected;
    for (const auto& guid : guids)
        if (std::find (model.guids[3].begin (), model.guids[3].end (), guid) != model.guids[3].end ())
            selected.push_back (guid);
    const auto members = massingbuildings::Members (model.buildingSlabs, selected);
    if (members.empty ())
        return out;
    if (members.size () > kMostSlabs) {
        out.section.note = "Building section exceeds its source budget; no partial building preview.";
        return out;
    }
    const auto& taken = members;
    out.slabs = taken; // Keep retained floor picks while current source bodies are pending.
    const ProjectStoreys storeys = ReadStoreys ();
    const auto bodies = slabbodies::Latest ();
    const slabsource::Reading slabs = slabsource::ReadMassing (taken, bodies.get ());
    if (!slabs.skipped.empty ()) {
        out.section.note = slabs.skipped.front ().reason + " No partial building section displayed.";
        return out;
    }
    if (slabs.slabs.empty ())
        return out;
    meta::ProjectSchema schema;
    bool stored = false;
    std::string error;
    if (!meta::storage::ReadSchema (schema, stored, error)) {
        out.section.known = true;
        out.section.note = error;
        return out;
    }
    // Cut at the project's storeys: a floor is a storey, and the floor domain's positions are
    // their numbers.
    const slabslices::Rule rule;
    std::vector<hudsection::Slab> masses;
    std::string first;
    for (const slabslices::Slab& slab : slabs.slabs) {
        hudsection::Slab mass;
        mass.guid = slab.guid;
        const slabslices::Summary summary =
            slabslices::SliceBody (slab, bodies->meshes.at (slab.guid), rule, storeys, out.slices, true);
        if (!summary.problem.empty ()) {
            out.slices.clear ();
            out.section.note = summary.problem;
            return out;
        }
        mass.floors = summary.floors;
        std::vector<double> bases;
        for (const slabslices::Floor& floor : summary.floors)
            bases.push_back (floor.base);
        mass.storeys = slabslices::StoreysAt (storeys, bases);
        bool present = false;
        if (!meta::storage::Read (slab.guid, mass.meta, present, error) && first.empty ())
            first = error;
        masses.push_back (std::move (mass));
    }
    for (const auto& slice : out.slices)
        for (const auto& chain : slice.chains)
            if (!chain.closed) {
                out.slices.clear ();
                out.section.note = "Massing source has an open cross-section; no partial building section displayed.";
                return out;
            }
    out.section = hudsection::Build (masses, storeys, schema);
    out.section.note = first;
    return out;
}

void Show (const std::vector<storysliceoverlay::Slice>& slices, const hudsection::Run& run, uint32_t rgba)
{
    std::vector<storysliceoverlay::Slice> picked;
    for (const storysliceoverlay::Slice& slice : slices)
        if (run.Has (slice.storey))
            picked.push_back (slice);
    if (picked.empty ()) {
        Hide ();
        return;
    }
    // The storey slices' look, in the HUD's accent: what the user picked, not what is there.
    storysliceoverlay::Controls controls;
    controls.views = overlaylayers::Views::ThreeD;
    controls.fillRgba = (rgba & 0xFFFFFF00u) | 0x59u;
    controls.outlineRgba = rgba;
    controls.outlineWidthPixels = 2.5f;
    controls.label = false;
    storysliceoverlay::Built built = storysliceoverlay::BuildLayer (picked, controls);
    built.layer.name = kLayerName;
    const std::string refused = overlaylayers::Validate (built.layer);
    if (built.slices == 0 || !refused.empty ()) {
        if (!refused.empty ()) {
            ArchVizLog ("SECTION      the picked floors NOT DRAWN: " + refused);
            hudconsole::Warning ("Section", "the picked floors are not drawn: " + refused);
        }
        Hide ();
        return;
    }
    overlaylayers::Set (std::move (built.layer));
    overlaycontrol::PublishLayers ();
}

void Hide ()
{
    if (overlaylayers::Clear (kLayerName))
        overlaycontrol::PublishLayers ();
}

hudsection::Section Published ()
{
    bool ask = false;
    hudsection::Section section;
    {
        std::lock_guard<std::mutex> lock (g_mutex);
        ask = !g_asked;
        g_asked = true;
        section = g_published;
    }
    if (ask)
        selectionmetadata::Later (&Publish);
    return section;
}

void Publish ()
{
    Reading reading = Read (selectionmetadata::SelectedGuids ());
    std::lock_guard<std::mutex> lock (g_mutex);
    g_published = std::move (reading.section);
    g_asked = true;
}

void SelectionChanged ()
{
    if (!surfaceswitch::ViewerOpen () || g_pending.exchange (true))
        return;
    selectionmetadata::Later ([] () {
        g_pending = false;
        Publish ();
    });
}

void Forget ()
{
    std::lock_guard<std::mutex> lock (g_mutex);
    g_published = hudsection::Section {};
    g_asked = false;
}

bool Shown ()
{
    for (const std::shared_ptr<const overlaylayers::Layer>& layer : overlaylayers::Layers ())
        if (layer != nullptr && layer->name == kLayerName)
            return true;
    return false;
}

} // namespace sectionmodel
} // namespace archviz
} // namespace geomsrv
