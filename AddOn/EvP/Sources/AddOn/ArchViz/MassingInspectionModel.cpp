#include "ArchViz/MassingInspectionModel.hpp"
#include "ArchViz/MassingSlicesModel.hpp"
#include "ArchViz/MassingModel.hpp"
#include "ArchViz/OverlayController.hpp"
#include "ArchViz/OverlayInput.hpp"
#include <algorithm>

namespace geomsrv::archviz::massinginspectionmodel {
namespace {
bool s_unique = false;
bool s_markLargeFloors = false;
massingareas::Coefficients s_coefficients;
std::string s_building, s_floorBuilding, s_note;
std::string s_uniqueNote, s_buildingNote, s_floorNote;
std::string s_largeFloorNote;
std::shared_ptr<const massingslices::Result> s_snapshot;
bool s_uniqueDirty = true, s_buildingDirty = true, s_floorDirty = true;
bool s_largeFloorsDirty = true;
hudsection::Run s_floors;
void Publish ()
{
    overlaycontrol::PublishLayers ();
    overlayinput::RequestLayout (overlayinput::View::Plan);
    overlayinput::RequestLayout (overlayinput::View::ThreeD);
}
} // namespace
void Refresh ()
{
    using namespace massingbuildings;
    const auto result = massingslicesmodel::Read ();
    if (result != s_snapshot)
        s_uniqueDirty = s_buildingDirty = s_floorDirty = s_largeFloorsDirty = true;
    s_snapshot = result;
    if (s_uniqueDirty) {
        overlaylayers::Clear (kUniqueLayer);
        s_uniqueNote.clear ();
    }
    if (s_buildingDirty) {
        overlaylayers::Clear (kSelectedLayer);
        s_buildingNote.clear ();
    }
    if (s_floorDirty) {
        overlaylayers::Clear (kFloorsLayer);
        s_floorNote.clear ();
    }
    if (s_largeFloorsDirty) {
        overlaylayers::Clear (massingslices::kLargeFloorsLayer);
        s_largeFloorNote.clear ();
    }
    if (!result)
        return;
    const auto model = massingmodel::Read ();
    auto surfaces = result->buildingSurfaces;
    std::vector<std::string> seeds;
    for (const auto& surface : surfaces)
        seeds.push_back (surface.record.guid);
    seeds.insert (seeds.end (), model.guids[3].begin (), model.guids[3].end ());
    const auto selected = massingslicesmodel::SelectedGuids ();
    seeds.insert (seeds.end (), selected.begin (), selected.end ());
    const auto expected = Members (model.buildingSlabs, seeds);
    for (const auto& record : model.buildingSlabs)
        if (std::find (expected.begin (), expected.end (), record.guid) != expected.end () &&
            std::none_of (surfaces.begin (), surfaces.end (),
                          [&] (const auto& s) { return s.record.guid == record.guid; }))
            surfaces.push_back ({ record, nullptr });
    const auto selectedMembers = Members (model.buildingSlabs, selected);
    const auto allowed = [&] (const std::string& key) {
        for (const auto& group : Groups (model.buildingSlabs))
            if (group.key == key)
                return std::any_of (group.guids.begin (), group.guids.end (), [&] (const auto& guid) {
                    return std::find (selectedMembers.begin (), selectedMembers.end (), guid) != selectedMembers.end ();
                });
        return false;
    };
    const auto show = [&] (const char* name, const std::string& key, std::string& note) {
        overlaylayers::Layer layer;
        std::string error;
        if (!model.known || !Inspect (surfaces, key, layer, error))
            note = model.known ? error : "Building identity index is incomplete; no partial inspection displayed.";
        else if (!layer.meshes.empty ()) {
            layer.name = name;
            overlaylayers::Set (std::move (layer));
        }
    };
    if (s_uniqueDirty && s_unique)
        show (kUniqueLayer, {}, s_uniqueNote);
    if (s_buildingDirty && !s_building.empty () && allowed (s_building))
        show (kSelectedLayer, s_building, s_buildingNote);
    const auto previews = Previews (result->section, model.buildingSlabs, {}, selected);
    const auto complete = std::find_if (previews.begin (), previews.end (), [&] (const auto& preview) {
        return preview.building.key == s_floorBuilding && preview.section.known;
    });
    if (s_floorDirty && !s_floorBuilding.empty () && !s_floors.Empty () && allowed (s_floorBuilding) &&
        complete != previews.end ()) {
        overlaylayers::Layer layer;
        std::string error;
        if (massingslices::FloorHighlight (*result, s_floorBuilding, s_floors, layer, error)) {
            if (!layer.meshes.empty ())
                overlaylayers::Set (std::move (layer));
        }
        else
            s_floorNote = error;
    }
    if (s_largeFloorsDirty && s_markLargeFloors) {
        overlaylayers::Layer layer;
        if (!model.known)
            s_largeFloorNote = "Building identity index is incomplete; no partial large-floor marks displayed.";
        else if (massingslices::LargeFloorHighlight (*result, model.buildingSlabs, s_coefficients, layer,
                                                     s_largeFloorNote)) {
            if (!layer.meshes.empty ())
                overlaylayers::Set (std::move (layer));
        }
    }
    s_uniqueDirty = s_buildingDirty = s_floorDirty = s_largeFloorsDirty = false;
    s_note = s_uniqueNote;
    for (const auto& note : { s_buildingNote, s_floorNote, s_largeFloorNote })
        if (!note.empty () && note != s_note)
            s_note += (s_note.empty () ? "" : " ") + note;
}
void Follow (bool unique, const std::string& building, const std::string& floorBuilding, const hudsection::Run& floors,
             bool markLargeFloors, const massingareas::Coefficients& coefficients)
{
    const bool grossChanged = coefficients.grossFactor != s_coefficients.grossFactor;
    if (unique == s_unique && building == s_building && floorBuilding == s_floorBuilding && floors == s_floors &&
        markLargeFloors == s_markLargeFloors && !grossChanged)
        return;
    s_uniqueDirty = s_uniqueDirty || unique != s_unique;
    s_buildingDirty = s_buildingDirty || building != s_building;
    s_floorDirty = s_floorDirty || floorBuilding != s_floorBuilding || floors != s_floors;
    s_largeFloorsDirty = s_largeFloorsDirty || markLargeFloors != s_markLargeFloors || grossChanged;
    s_unique = unique;
    s_building = building;
    s_floorBuilding = floorBuilding;
    s_floors = floors;
    s_markLargeFloors = markLargeFloors;
    s_coefficients = coefficients;
    Refresh ();
    Publish ();
}
std::string Note ()
{
    return s_note;
}
void Forget ()
{
    s_unique = false;
    s_markLargeFloors = false;
    s_coefficients = {};
    s_building.clear ();
    s_floorBuilding.clear ();
    s_floors = {};
    s_note.clear ();
    s_uniqueNote.clear ();
    s_buildingNote.clear ();
    s_floorNote.clear ();
    s_largeFloorNote.clear ();
    s_snapshot.reset ();
    s_uniqueDirty = s_buildingDirty = s_floorDirty = s_largeFloorsDirty = true;
    overlaylayers::Clear (massingbuildings::kUniqueLayer);
    overlaylayers::Clear (massingbuildings::kSelectedLayer);
    overlaylayers::Clear (massingbuildings::kFloorsLayer);
    overlaylayers::Clear (massingslices::kLargeFloorsLayer);
}
} // namespace geomsrv::archviz::massinginspectionmodel
