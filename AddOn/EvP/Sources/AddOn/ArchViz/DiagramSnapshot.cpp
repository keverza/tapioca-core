#include "ArchViz/DiagramSnapshot.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace geomsrv::archviz::diagram {
namespace {
std::vector<massingbuildings::Record> RecordsOf (const massingslices::Result& result)
{
    std::vector<massingbuildings::Record> records;
    for (const auto& surface : result.buildingSurfaces)
        records.push_back (surface.record);
    return records;
}
// Slab GUID -> building key.
std::map<std::string, std::string> Owners (const std::vector<massingbuildings::Group>& groups)
{
    std::map<std::string, std::string> owner;
    for (const auto& group : groups)
        for (const auto& guid : group.guids)
            owner.emplace (guid, group.key);
    return owner;
}
void Fail (SnapshotStatus& status, SnapshotIssue issue)
{
    if (std::find (status.failures.begin (), status.failures.end (), issue) == status.failures.end ())
        status.failures.push_back (issue);
}

// One storey of one building: the rows that make it.
struct Floor {
    int story = 0;
    double z = 0, top = 0;
    std::vector<const massingslices::Row*> rows;
};
std::vector<Floor> FloorsOf (const std::vector<const massingslices::Row*>& rows, StoreyDomain domain)
{
    std::vector<const massingslices::Row*> sorted = rows;
    std::stable_sort (sorted.begin (), sorted.end (), [] (const auto* a, const auto* b) {
        if (a->z != b->z)
            return a->z < b->z;
        if (a->story != b->story)
            return a->story < b->story;
        return a->guid < b->guid;
    });
    std::vector<Floor> floors;
    for (const auto* row : sorted) {
        Floor* into = nullptr;
        for (auto& floor : floors) {
            // Project stories are the storey; the massing's own floors cluster by elevation.
            const bool same = domain == StoreyDomain::ProjectStories ? floor.story == row->story
                                                                     : std::abs (floor.z - row->z) <= kStoreyTolerance;
            if (same) {
                into = &floor;
                break;
            }
        }
        if (!into) {
            floors.push_back ({ row->story, row->z, row->z, {} });
            into = &floors.back ();
        }
        into->z = std::min (into->z, row->z);
        into->top = std::max (into->top, row->z + std::max (0.0, row->floorHeight));
        into->rows.push_back (row);
    }
    std::stable_sort (floors.begin (), floors.end (),
                      [] (const Floor& a, const Floor& b) { return a.z != b.z ? a.z < b.z : a.story < b.story; });
    return floors;
}
std::map<std::string, std::vector<const massingslices::Row*>>
RowsByBuilding (const massingslices::Result& floors, const std::map<std::string, std::string>& owner,
                std::set<std::string>* unknown)
{
    std::map<std::string, std::vector<const massingslices::Row*>> byBuilding;
    for (const auto& row : floors.rows) {
        auto found = owner.find (row.guid);
        if (found == owner.end ()) {
            if (unknown)
                unknown->insert (row.guid);
            continue;
        }
        byBuilding[found->second].push_back (&row);
    }
    return byBuilding;
}
} // namespace

const char* Describe (SnapshotIssue issue)
{
    switch (issue) {
        case SnapshotIssue::MissingSlices:
            return "no published massing result";
        case SnapshotIssue::IncompleteSlices:
            return "massing sources were skipped; a partial building is never drawn";
        case SnapshotIssue::UnknownBuildingKey:
            return "a massing floor belongs to no building";
        case SnapshotIssue::StoreyDomainMismatch:
            return "one storey of one building lies at two elevations";
    }
    return "unknown issue";
}

DiagramInputSnapshot Capture (std::shared_ptr<const massingslices::Result> slices, ProjectStoreys storeys,
                              RevisionSet revisions)
{
    DiagramInputSnapshot snapshot;
    snapshot.slices = std::move (slices);
    snapshot.storeys = std::move (storeys);
    snapshot.revisions = revisions;
    SnapshotStatus& status = snapshot.status;
    if (!snapshot.slices) {
        Fail (status, SnapshotIssue::MissingSlices);
        return snapshot;
    }
    if (!snapshot.slices->complete)
        Fail (status, SnapshotIssue::IncompleteSlices);
    if (snapshot.slices->followsStories) {
        snapshot.floors = snapshot.slices;
        snapshot.domain = StoreyDomain::ProjectStories;
    }
    else if (snapshot.slices->stories) {
        snapshot.floors = snapshot.slices->stories;
        snapshot.domain = StoreyDomain::ProjectStories;
        if (!snapshot.floors->complete)
            Fail (status, SnapshotIssue::IncompleteSlices);
    }
    else {
        snapshot.floors = snapshot.slices;
        snapshot.domain = StoreyDomain::MassingFloors;
        status.warnings.push_back ("floors are the massing's own heights, not project stories: storey identity is "
                                   "positional");
    }
    auto records = RecordsOf (*snapshot.slices);
    if (records.empty ())
        records = RecordsOf (*snapshot.floors);
    snapshot.groups = massingbuildings::Groups (records);
    std::sort (snapshot.groups.begin (), snapshot.groups.end (),
               [] (const auto& a, const auto& b) { return a.key < b.key; });

    std::set<std::string> affected, unknown;
    const auto byBuilding = RowsByBuilding (*snapshot.floors, Owners (snapshot.groups), &unknown);
    if (!unknown.empty ()) {
        Fail (status, SnapshotIssue::UnknownBuildingKey);
        affected.insert (unknown.begin (), unknown.end ());
    }
    if (snapshot.domain == StoreyDomain::ProjectStories) {
        const std::set<int> known (snapshot.storeys.indices.begin (), snapshot.storeys.indices.end ());
        std::set<int> missing;
        for (const auto& [key, rows] : byBuilding)
            for (const auto& floor : FloorsOf (rows, snapshot.domain)) {
                double lo = std::numeric_limits<double>::infinity (), hi = -lo;
                for (const auto* row : floor.rows) {
                    lo = std::min (lo, row->z);
                    hi = std::max (hi, row->z);
                }
                if (hi - lo > kStoreyTolerance) {
                    Fail (status, SnapshotIssue::StoreyDomainMismatch);
                    affected.insert (key);
                }
                if (!known.count (floor.story))
                    missing.insert (floor.story);
            }
        if (snapshot.storeys.floorIds.size () != snapshot.storeys.indices.size ())
            status.warnings.push_back ("project stories carry no floorId: storey identity is positional");
        for (int story : missing)
            status.warnings.push_back ("story " + std::to_string (story) + " is not a project story");
    }
    status.affected.assign (affected.begin (), affected.end ());
    status.compatible = status.failures.empty ();
    return snapshot;
}

BuiltScene BuildScene (const DiagramInputSnapshot& snapshot, StoreyBindings& bindings)
{
    BuiltScene built;
    built.revisions = snapshot.revisions;
    if (!snapshot.status.compatible || !snapshot.floors) {
        for (auto issue : snapshot.status.failures)
            built.notes.push_back (Describe (issue));
        return built;
    }
    std::map<int, int> floorIdOf;
    if (snapshot.storeys.floorIds.size () == snapshot.storeys.indices.size ())
        for (size_t i = 0; i < snapshot.storeys.indices.size (); ++i)
            floorIdOf[snapshot.storeys.indices[i]] = snapshot.storeys.floorIds[i];

    auto scene = std::make_shared<Scene> ();
    std::set<std::string> represented, present;
    const auto byBuilding = RowsByBuilding (*snapshot.floors, Owners (snapshot.groups), nullptr);
    for (const auto& group : snapshot.groups) {
        auto rows = byBuilding.find (group.key);
        if (rows == byBuilding.end ())
            continue;
        present.insert (group.key);
        const auto floors = FloorsOf (rows->second, snapshot.domain);
        std::vector<StoreySource> sources;
        for (size_t i = 0; i < floors.size (); ++i) {
            StoreySource source;
            source.sourceStory = floors[i].story;
            source.z = floors[i].z;
            source.displayOrder = int (i);
            auto id = floorIdOf.find (floors[i].story);
            if (snapshot.domain == StoreyDomain::ProjectStories && id != floorIdOf.end ())
                source.persistentId = StoreyPersistentId (id->second);
            sources.push_back (std::move (source));
        }
        auto& previous = bindings.byBuilding[group.key];
        BindResult bound = BindStoreys (group.key, previous, sources, bindings.nextSerial);
        previous = bound.bindings;
        built.dropped.insert (built.dropped.end (), bound.dropped.begin (), bound.dropped.end ());
        for (auto& note : bound.notes)
            built.notes.push_back (group.key + ": " + note);

        Entity building;
        building.id = { Level::Building, group.key };
        building.source.buildingKey = group.key;
        building.source.sourceGuid = group.guids.empty () ? std::string () : group.guids.front ();
        building.planned = false;
        bool anyBuilding = false, anyXY = false;
        Vec3 xyLo {}, xyHi {};
        std::vector<Entity> storeys;
        std::vector<bool> hasGeometry;
        for (size_t i = 0; i < floors.size (); ++i) {
            const Floor& floor = floors[i];
            overlaylayers::Mesh mesh;
            for (const auto* row : floor.rows) {
                represented.insert (row->guid);
                const auto& chains = row->rawChains.empty () ? row->chains : row->rawChains;
                if (chains.empty ())
                    continue;
                std::string error;
                if (!massingslices::ExtrudeChains (chains, row->z, row->z + std::max (0.0, row->floorHeight), mesh,
                                                   error))
                    built.notes.push_back (group.key + " story " + std::to_string (floor.story) +
                                           ": shell not triangulated (" + error + ")");
            }
            Entity storey;
            storey.id = bound.bindings[i].id;
            storey.parent = building.id;
            storey.source.buildingKey = group.key;
            storey.source.sourceGuid = floor.rows.front ()->guid;
            storey.source.sourceStory = floor.story;
            storey.planned = false;
            built.colours[storey.id] = floor.rows.front ()->rgba;
            {
                StoreySummary summary;
                summary.buildingKey = group.key;
                summary.buildingId = group.id;
                summary.sourceStory = floor.story;
                summary.z = floor.z;
                summary.top = floor.top;
                for (size_t s = 0; s < snapshot.storeys.indices.size () && s < snapshot.storeys.names.size (); ++s)
                    if (snapshot.domain == StoreyDomain::ProjectStories && snapshot.storeys.indices[s] == floor.story)
                        summary.storyName = snapshot.storeys.names[s];
                std::set<std::string> slabs;
                for (const auto* row : floor.rows) {
                    summary.rawArea += row->rawArea;
                    summary.allowedArea += row->allowedArea;
                    if (!row->function.empty () && std::find (summary.functions.begin (), summary.functions.end (),
                                                              row->function) == summary.functions.end ())
                        summary.functions.push_back (row->function);
                    slabs.insert (row->guid);
                }
                summary.slabs.assign (slabs.begin (), slabs.end ());
                built.storeys[storey.id] = std::move (summary);
            }
            storey.boundsMin = { 0, 0, floor.z };
            storey.boundsMax = { 0, 0, floor.top };
            const bool geometry = mesh.points.size () >= 3;
            if (geometry) {
                for (int k = 0; k < 2; ++k) {
                    storey.boundsMin[k] = std::numeric_limits<double>::infinity ();
                    storey.boundsMax[k] = -std::numeric_limits<double>::infinity ();
                }
                for (size_t p = 0; p + 2 < mesh.points.size (); p += 3)
                    for (size_t k = 0; k < 3; ++k) {
                        storey.boundsMin[k] = std::min (storey.boundsMin[k], mesh.points[p + k]);
                        storey.boundsMax[k] = std::max (storey.boundsMax[k], mesh.points[p + k]);
                    }
                for (int k = 0; k < 2; ++k) {
                    xyLo[k] = anyXY ? std::min (xyLo[k], storey.boundsMin[k]) : storey.boundsMin[k];
                    xyHi[k] = anyXY ? std::max (xyHi[k], storey.boundsMax[k]) : storey.boundsMax[k];
                }
                anyXY = true;
                PickMesh shell;
                shell.id = storey.id;
                Vec3 pivot;
                for (int k = 0; k < 3; ++k)
                    pivot[k] = 0.5 * (storey.boundsMin[k] + storey.boundsMax[k]);
                shell.positions.reserve (mesh.points.size ());
                for (size_t p = 0; p < mesh.points.size (); ++p)
                    shell.positions.push_back (mesh.points[p] - pivot[p % 3]);
                shell.indices = std::move (mesh.indices);
                built.shells.push_back (std::move (shell));
            }
            else {
                built.notes.push_back (group.key + " story " + std::to_string (floor.story) +
                                       ": no slice geometry; shown as its building's footprint");
            }
            hasGeometry.push_back (geometry);
            storeys.push_back (std::move (storey));
        }
        // A storey without geometry still appears, as the building's footprint at its levels.
        for (size_t i = 0; i < storeys.size (); ++i) {
            if (!hasGeometry[i] && anyXY)
                for (int k = 0; k < 2; ++k) {
                    storeys[i].boundsMin[k] = xyLo[k];
                    storeys[i].boundsMax[k] = xyHi[k];
                }
            for (int k = 0; k < 3; ++k) {
                building.boundsMin[k] =
                    anyBuilding ? std::min (building.boundsMin[k], storeys[i].boundsMin[k]) : storeys[i].boundsMin[k];
                building.boundsMax[k] =
                    anyBuilding ? std::max (building.boundsMax[k], storeys[i].boundsMax[k]) : storeys[i].boundsMax[k];
            }
            anyBuilding = true;
        }
        scene->entities.push_back (std::move (building));
        for (auto& storey : storeys)
            scene->entities.push_back (std::move (storey));
    }
    // Buildings that vanished drop every binding they had.
    for (auto it = bindings.byBuilding.begin (); it != bindings.byBuilding.end ();) {
        if (present.count (it->first)) {
            ++it;
            continue;
        }
        for (const auto& binding : it->second)
            built.dropped.push_back (binding.id);
        it = bindings.byBuilding.erase (it);
    }
    std::sort (built.dropped.begin (), built.dropped.end ());
    std::sort (built.shells.begin (), built.shells.end (),
               [] (const PickMesh& a, const PickMesh& b) { return a.id < b.id; });
    built.representedGuids.assign (represented.begin (), represented.end ());
    for (const auto& warning : snapshot.status.warnings)
        scene->notes.push_back (warning);
    scene->notes.insert (scene->notes.end (), built.notes.begin (), built.notes.end ());
    built.scene = std::move (scene);
    return built;
}
} // namespace geomsrv::archviz::diagram
