#include "Edit/EditLandscape.hpp"
#include "Edit/EditDocument.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>

namespace tapioca::geometry {
namespace edit = geomsrv::edit;

const char* LandscapeDiagnosticCodeName (LandscapeDiagnosticCode code)
{
    switch (code) {
#define LANDSCAPE_CODE(name)                                                                                           \
    case LandscapeDiagnosticCode::name:                                                                                \
        return #name
        LANDSCAPE_CODE (ProvenanceUnattributable);
        LANDSCAPE_CODE (ProvenanceConflict);
        LANDSCAPE_CODE (ProvenanceStale);
        LANDSCAPE_CODE (PatchKeyUnstable);
        LANDSCAPE_CODE (FaceKeyAmbiguous);
        LANDSCAPE_CODE (AmbiguousFace);
        LANDSCAPE_CODE (OrphanedOperation);
        LANDSCAPE_CODE (DagCycle);
        LANDSCAPE_CODE (PropertyLineOutside);
        LANDSCAPE_CODE (PropertyLineArcUnsupported);
        LANDSCAPE_CODE (OffsetCollapsed);
        LANDSCAPE_CODE (OffsetSelfIntersection);
        LANDSCAPE_CODE (ClipEmpty);
#undef LANDSCAPE_CODE
    }
    return "Unknown";
}

bool CanonicalLandscapeGuid (const std::string& text, std::string& canonical)
{
    if (text.size () != 36)
        return false;
    std::string value = text;
    bool nonzero = false;
    for (size_t i = 0; i < value.size (); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-')
                return false;
            continue;
        }
        char& c = value[i];
        if (c >= 'a' && c <= 'f')
            c = char (c - 'a' + 'A');
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')))
            return false;
        nonzero = nonzero || c != '0';
    }
    if (!nonzero)
        return false;
    canonical = std::move (value);
    return true;
}

bool ValidateLandscapeDag (const LandscapeSession& session, std::vector<Id>& order, std::string& error)
{
    order.clear ();
    std::map<Id, const LandscapeOperation*> nodes;
    for (const auto& op : session.operations)
        if (op.id == 0 || !nodes.emplace (op.id, &op).second) {
            error = "ProvenanceConflict: duplicate/absent operation ID";
            return false;
        }
    std::map<Id, size_t> indegree;
    std::map<Id, std::vector<Id>> children;
    std::set<Id> ready;
    for (const auto& [id, op] : nodes) {
        std::set<Id> inputs;
        for (Id input : op->inputs) {
            if (input == 0 || !inputs.insert (input).second) {
                error = "ProvenanceConflict: absent/duplicate operation input";
                return false;
            }
            if (nodes.count (input)) {
                ++indegree[id];
                children[input].push_back (id);
            }
        }
        if (indegree[id] == 0)
            ready.insert (id);
    }
    while (!ready.empty ()) {
        const Id id = *ready.begin ();
        ready.erase (ready.begin ());
        order.push_back (id);
        for (Id child : children[id])
            if (--indegree[child] == 0)
                ready.insert (child);
    }
    if (order.size () != nodes.size ()) {
        error = "DagCycle: operation inputs contain a cycle";
        return false;
    }
    return true;
}

namespace {
bool Guid (const std::string& value)
{
    std::string canonical;
    return value.empty () || (CanonicalLandscapeGuid (value, canonical) && canonical == value);
}
bool Material (const MaterialSpec& m)
{
    return m.preset <= MaterialPreset::Grass && Guid (m.fillGuid) && Guid (m.surfaceGuid) &&
           std::isfinite (m.hatchDirection) && std::isfinite (m.hatchDensity) && m.hatchDensity >= 0.0;
}
template <class T> bool SortedUnique (const std::vector<T>& values)
{
    return std::adjacent_find (values.begin (), values.end (), [] (const T& a, const T& b) { return a >= b; }) ==
           values.end ();
}
bool Mutate (edit::Document& doc, const std::function<bool (edit::Document&, std::string&)>& action, std::string& error)
{
    error.clear ();
    if (!doc.landscape) {
        error = "landscape session is not started";
        return false;
    }
    edit::Document candidate = doc;
    if (!action (candidate, error))
        return false;
    LandscapeResult result;
    if (!RebuildLandscape (candidate, result, error, &doc) || !edit::Validate (candidate, error))
        return false;
    candidate.revision = doc.revision + 1;
    doc = std::move (candidate);
    return true;
}
bool Allocate (edit::Document& doc, Id& id, std::string& error)
{
    if (doc.nextId == 0 || doc.nextId == std::numeric_limits<Id>::max ()) {
        error = "landscape authored ID allocator exhausted";
        return false;
    }
    id = edit::NewId (doc);
    return true;
}
} // namespace

bool ValidateLandscape (const edit::Document& doc, std::string& error)
{
    if (!doc.landscape)
        return true;
    const auto& s = *doc.landscape;
    if (std::all_of (s.sessionUuid.begin (), s.sessionUuid.end (), [] (uint8_t b) { return b == 0; }) ||
        !Guid (s.sourceTerrainGuid)) {
        error = "landscape.sessionUuid/sourceTerrainGuid: non-nil canonical identity required";
        return false;
    }
    if (s.sources.size () > 4096 || s.operations.size () > 4096 || s.objects.size () > 4096 ||
        s.regions.size () > 65536 || s.terrainControls.size () > 4096 || s.entrances.size () > 4096) {
        error = "landscape exceeds its source/operation/object/region budget";
        return false;
    }
    std::set<Id> ids, sources, operations, objects, regions, assignments, patches;
    const auto claim = [&] (Id id) { return id != 0 && id < doc.nextId && ids.insert (id).second; };
    for (const auto& e : doc.entities) {
        if (!claim (e.id)) {
            error = "duplicate document identity";
            return false;
        }
        for (const auto& v : e.vertices)
            if (!claim (v.id)) {
                error = "duplicate vertex identity";
                return false;
            }
        for (const auto& segment : e.segments)
            if (!claim (segment.id)) {
                error = "duplicate segment identity";
                return false;
            }
    }
    for (const auto& p : doc.paths) {
        if (!claim (p.id)) {
            error = "duplicate path identity";
            return false;
        }
        for (const auto& v : p.points)
            if (!claim (v.id)) {
                error = "duplicate path-point identity";
                return false;
            }
        for (const auto& v : p.profile)
            if (!claim (v.id)) {
                error = "duplicate profile identity";
                return false;
            }
        for (const auto& v : p.sections)
            if (!claim (v.id)) {
                error = "duplicate section identity";
                return false;
            }
    }
    size_t terrains = 0;
    for (const auto& v : s.sources) {
        const bool terrain = v.role == SourceRole::Terrain;
        if (!claim (v.id) || v.role > SourceRole::Building || !Guid (v.archicadGuid) ||
            (v.reference && (v.archicadGuid.empty () || !v.entityId)) ||
            (terrain ? (v.entityId != 0 || v.pathId != 0 || !v.arcAngles.empty ())
                     : ((v.entityId == 0) == (v.pathId == 0))) ||
            (v.pathId && v.role != SourceRole::PathCenterline)) {
            error = "landscape.sources: conflicting ID, role, geometry reference or GUID";
            return false;
        }
        if (terrain && !v.deleted && (++terrains > 1 || v.archicadGuid != s.sourceTerrainGuid)) {
            error = "landscape.sources: one live terrain source must carry the session's terrain GUID";
            return false;
        }
        if (!v.deleted && ((v.entityId && edit::Find (doc, v.entityId) == nullptr) ||
                           (v.pathId && edit::FindPath (doc, v.pathId) == nullptr))) {
            error = "landscape.sources: geometry must be captured before evaluation";
            return false;
        }
        sources.insert (v.id);
        if (const auto* entity = edit::Find (doc, v.entityId))
            for (const auto& vertex : entity->vertices)
                if (!std::isfinite (vertex.radius) || vertex.radius < 0 || !edit::Finite (vertex.in) ||
                    !edit::Finite (vertex.out)) {
                    error = "landscape.sources: finite handles and non-negative radius required";
                    return false;
                }
    }
    std::set<std::pair<Id, Id>> controls;
    for (const auto& control : s.terrainControls)
        if (!sources.count (control.source) || control.vertex >= doc.nextId ||
            !controls.emplace (control.source, control.vertex).second) {
            error = "landscape.terrainControls: unknown source, invalid vertex or duplicate control";
            return false;
        }
    for (const auto& entrance : s.entrances) {
        const auto source = std::find_if (s.sources.begin (), s.sources.end (),
                                          [&] (const LandscapeSource& v) { return v.id == entrance.source; });
        if (!claim (entrance.id) || source == s.sources.end () || source->deleted ||
            source->role != SourceRole::Building || !(entrance.along >= 0) || !std::isfinite (entrance.along) ||
            !std::isfinite (entrance.z) || !Guid (entrance.doorGuid)) {
            error = "landscape.entrances: an entrance needs a live building outline, a finite place and height";
            return false;
        }
    }
    for (const auto& v : s.operations) {
        const size_t n = v.inputs.size ();
        const bool unary = v.kind == OperationKind::Stroke || v.kind == OperationKind::Offset;
        if (!claim (v.id) || v.kind > OperationKind::ProtectedZone || !Material (v.material) ||
            !std::isfinite (v.width) || !(v.width > 0.0) || !std::isfinite (v.offset) || (unary && n != 1) ||
            (!unary && v.kind != OperationKind::RegionSelect && n < 2u) ||
            (v.kind == OperationKind::Difference && n != 2)) {
            error = "landscape.operations: invalid ID, kind, arity, material or finite parameter";
            return false;
        }
        operations.insert (v.id);
    }
    for (const auto& op : s.operations)
        for (Id input : op.inputs)
            if (!sources.count (input) && !operations.count (input)) {
                // Deleted sources retain their catalog entry. An unknown or
                // wrong-domain ID is corrupt ancestry, not an orphan tombstone.
                error = "landscape.operations.inputs: unknown source/operation identity";
                return false;
            }
    for (const auto& v : s.objects) {
        if (!claim (v.id) || v.kind > ObjectKind::Edge || !Material (v.baseMaterial) || !std::isfinite (v.thickness) ||
            v.thickness < 0 || !operations.count (v.footprintOperation) ||
            (v.pathId && edit::FindPath (doc, v.pathId) == nullptr) ||
            std::any_of (
                v.maskOperations.begin (), v.maskOperations.end (), [&] (Id id) { return !operations.count (id); })) {
            error = "landscape.objects: invalid identity, footprint, mask, path or material";
            return false;
        }
        objects.insert (v.id);
    }
    const auto descriptor = [&] (const RegionDescriptor& d) {
        if ((d.object && !objects.count (d.object)) || !SortedUnique (d.sources) ||
            std::any_of (d.sources.begin (), d.sources.end (), [&] (Id id) { return !sources.count (id); }) ||
            std::any_of (d.operations.begin (), d.operations.end (), [&] (Id id) { return !operations.count (id); }))
            return false;
        std::set<Id> uniqueOperations (d.operations.begin (), d.operations.end ());
        if (uniqueOperations.size () != d.operations.size ())
            return false;
        for (const auto& cycle : d.boundaries) {
            if (cycle.empty ())
                return false;
            for (const auto& piece : cycle)
                if (!sources.count (piece.source) || piece.segment == 0 || piece.segment >= doc.nextId)
                    return false;
        }
        return !d.sources.empty () && !d.boundaries.empty ();
    };
    for (const auto& v : s.regions) {
        if (!claim (v.id) || !descriptor (v.descriptor) || !SortedUnique (v.predecessors)) {
            error = "landscape.regions: invalid semantic identity or predecessor list";
            return false;
        }
        regions.insert (v.id);
    }
    for (const auto& v : s.regions)
        for (Id id : v.predecessors)
            if (id >= v.id || !regions.count (id)) {
                error = "landscape.regions: invalid predecessor";
                return false;
            }
    for (const auto& v : s.assignments) {
        if (!claim (v.id) || !regions.count (v.region) || (v.mask && !operations.count (v.mask)) ||
            !Material (v.material)) {
            error = "landscape.assignments: invalid identity, region, mask or material";
            return false;
        }
        const auto region =
            std::find_if (s.regions.begin (), s.regions.end (), [&] (const auto& r) { return r.id == v.region; });
        if (v.valid && !region->active) {
            error = "landscape.assignments: inactive region requires explicit resolution";
            return false;
        }
        assignments.insert (v.id);
    }
    for (const auto& v : s.patches) {
        if (!claim (v.key) || !objects.count (v.object) || (v.assignment && !assignments.count (v.assignment)) ||
            v.role.empty () || v.representation.empty () || v.sources.empty () || !SortedUnique (v.sources) ||
            !SortedUnique (v.predecessorRegions) || !SortedUnique (v.predecessorPatches)) {
            error = "landscape.patches: invalid descriptor";
            return false;
        }
        for (Id id : v.sources)
            if (!sources.count (id)) {
                error = "landscape.patches: unknown source";
                return false;
            }
        for (Id id : v.predecessorRegions)
            if (!regions.count (id)) {
                error = "landscape.patches: unknown region predecessor";
                return false;
            }
        patches.insert (v.key);
    }
    for (const auto& v : s.patches)
        for (Id id : v.predecessorPatches)
            if (!patches.count (id) || id >= v.key) {
                error = "landscape.patches: invalid patch predecessor";
                return false;
            }
    std::set<std::tuple<int, Id, std::string, std::string>> bindings;
    for (const auto& b : s.bindings) {
        const auto& catalog = b.kind == BindingKind::Source      ? sources
                              : b.kind == BindingKind::Operation ? operations
                                                                 : objects;
        if (b.kind > BindingKind::Object || !catalog.count (b.logicalId) || b.role.empty () ||
            b.role.find ('\0') != std::string::npos || !Guid (b.metadataUuid)) {
            error = "landscape.bindings: invalid logical owner/role/metadata UUID";
            return false;
        }
        for (const auto& guid : b.nativeGuids)
            if (guid.empty () || !Guid (guid) || !bindings.emplace (int (b.kind), b.logicalId, guid, b.role).second) {
                error = "landscape.bindings: malformed/duplicate native GUID";
                return false;
            }
    }
    for (const auto& op : s.operations)
        for (const auto& face : op.faces)
            if (!descriptor (face)) {
                error = "landscape.operations.faces: invalid semantic selector";
                return false;
            }
    std::vector<Id> order;
    if (!ValidateLandscapeDag (s, order, error))
        return false;
    std::map<Id, std::set<Id>> reachable;
    for (Id id : sources)
        reachable[id].insert (id);
    for (Id id : order) {
        const auto op =
            std::find_if (s.operations.begin (), s.operations.end (), [=] (const auto& o) { return o.id == id; });
        for (Id input : op->inputs)
            reachable[id].insert (reachable[input].begin (), reachable[input].end ());
        for (const auto& face : op->faces)
            if (!std::includes (reachable[id].begin (), reachable[id].end (), face.sources.begin (),
                                face.sources.end ())) {
                error = "landscape.operations.faces: selected source is not reachable from operation inputs";
                return false;
            }
    }
    return true;
}

bool StartLandscape (edit::Document& doc, std::array<uint8_t, 16> uuid, std::string& error)
{
    error.clear ();
    if (doc.landscape) {
        error = "landscape session already exists";
        return false;
    }
    edit::Document candidate = doc;
    candidate.landscape.emplace ();
    candidate.landscape->sessionUuid = uuid;
    if (!ValidateLandscape (candidate, error))
        return false;
    ++candidate.revision;
    doc = std::move (candidate);
    return true;
}

bool SetLandscapeTerrain (edit::Document& doc, const std::string& guid, Id& source, std::string& error)
{
    Id named = 0;
    const bool ok = Mutate (
        doc,
        [&] (edit::Document& d, std::string& e) {
            std::string canonical;
            if (!CanonicalLandscapeGuid (guid, canonical)) {
                e = "invalid terrain GUID";
                return false;
            }
            d.landscape->sourceTerrainGuid = canonical;
            for (auto& s : d.landscape->sources)
                if (s.role == SourceRole::Terrain && !s.deleted) {
                    s.archicadGuid = canonical;
                    named = s.id;
                    return true;
                }
            if (!Allocate (d, named, e))
                return false;
            LandscapeSource terrain;
            terrain.id = named;
            terrain.role = SourceRole::Terrain;
            terrain.archicadGuid = canonical;
            d.landscape->sources.push_back (terrain);
            return true;
        },
        error);
    if (ok)
        source = named;
    return ok;
}

bool SetLandscapeTerrainControl (edit::Document& doc, Id source, Id vertex, bool enabled, std::string& error)
{
    return Mutate (
        doc,
        [=] (edit::Document& d, std::string& e) {
            auto& controls = d.landscape->terrainControls;
            auto found = std::find_if (controls.begin (), controls.end (),
                                       [=] (const auto& c) { return c.source == source && c.vertex == vertex; });
            if ((found != controls.end () && found->enabled == enabled) || (found == controls.end () && !enabled))
                return false;
            if (enabled) {
                const auto input = std::find_if (d.landscape->sources.begin (), d.landscape->sources.end (),
                                                 [=] (const auto& s) { return s.id == source && !s.deleted; });
                const auto* entity = input == d.landscape->sources.end () ? nullptr : edit::Find (d, input->entityId);
                if (!entity || entity->kind != edit::Kind::Polyline ||
                    (vertex ? edit::VertexIndex (*entity, vertex) < 0
                            : (entity->closed || entity->vertices.size () < 2))) {
                    e = "TerrainControl: select a live guide vertex or open polyline";
                    return false;
                }
            }
            if (found == controls.end ())
                controls.push_back ({ source, vertex, enabled });
            else
                found->enabled = enabled;
            return true;
        },
        error);
}

bool AddLandscapeSource (edit::Document& doc, LandscapeSource source, Id& id, std::string& error)
{
    Id allocated = 0;
    const bool ok = Mutate (
        doc,
        [&] (edit::Document& d, std::string& e) {
            if (source.id != 0) {
                e = "new source must not supply an authored ID";
                return false;
            }
            if (!Allocate (d, allocated, e))
                return false;
            source.id = allocated;
            if (!source.archicadGuid.empty () && !CanonicalLandscapeGuid (source.archicadGuid, source.archicadGuid)) {
                e = "invalid source GUID";
                return false;
            }
            d.landscape->sources.push_back (source);
            return true;
        },
        error);
    if (ok)
        id = allocated;
    return ok;
}

bool DeleteLandscapeSource (edit::Document& doc, Id id, std::string& error)
{
    return Mutate (
        doc,
        [=] (edit::Document& d, std::string& e) {
            for (auto& source : d.landscape->sources)
                if (source.id == id && !source.deleted) {
                    source.deleted = true;
                    if (d.landscape->propertyLineSourceId == id)
                        d.landscape->propertyLineSourceId = 0;
                    auto& entrances = d.landscape->entrances;
                    entrances.erase (std::remove_if (entrances.begin (), entrances.end (),
                                                     [id] (const LandscapeEntrance& e) { return e.source == id; }),
                                     entrances.end ());
                    return true;
                }
            e = "source does not exist";
            return false;
        },
        error);
}

bool AddLandscapeOperation (edit::Document& doc, LandscapeOperation operation, Id& id, std::string& error)
{
    Id allocated = 0;
    const bool ok = Mutate (
        doc,
        [&] (edit::Document& d, std::string& e) {
            if (operation.id != 0) {
                e = "new operation must not supply an authored ID";
                return false;
            }
            if (operation.kind == OperationKind::RegionSelect)
                for (const auto& face : operation.faces)
                    for (Id source : face.sources)
                        if (std::find (operation.inputs.begin (), operation.inputs.end (), source) ==
                            operation.inputs.end ())
                            operation.inputs.push_back (source);
            for (Id input : operation.inputs) {
                const bool source = std::any_of (d.landscape->sources.begin (), d.landscape->sources.end (),
                                                 [=] (const auto& s) { return s.id == input && !s.deleted; });
                const bool op = std::any_of (d.landscape->operations.begin (), d.landscape->operations.end (),
                                             [=] (const auto& o) { return o.id == input; });
                if (!source && !op) {
                    e = "new operation input does not exist";
                    return false;
                }
            }
            if (!Allocate (d, allocated, e))
                return false;
            operation.id = allocated;
            d.landscape->operations.push_back (operation);
            return true;
        },
        error);
    if (ok)
        id = allocated;
    return ok;
}

bool SetLandscapeOperationEnabled (edit::Document& doc, Id id, bool enabled, std::string& error)
{
    return Mutate (
        doc,
        [=] (edit::Document& d, std::string& e) {
            for (auto& op : d.landscape->operations)
                if (op.id == id) {
                    op.enabled = enabled;
                    return true;
                }
            e = "operation does not exist";
            return false;
        },
        error);
}

bool AddHardscapeObject (edit::Document& doc, HardscapeObject object, Id& id, std::string& error)
{
    Id allocated = 0;
    const bool ok = Mutate (
        doc,
        [&] (edit::Document& d, std::string& e) {
            if (object.id != 0) {
                e = "new object must not supply an authored ID";
                return false;
            }
            if (!Allocate (d, allocated, e))
                return false;
            object.id = allocated;
            d.landscape->objects.push_back (object);
            return true;
        },
        error);
    if (ok)
        id = allocated;
    return ok;
}

bool SetLandscapePropertyLine (edit::Document& doc, Id source, std::string& error)
{
    return Mutate (
        doc,
        [=] (edit::Document& d, std::string& e) {
            for (auto& s : d.landscape->sources)
                if (s.id == source && !s.deleted && s.role != SourceRole::Terrain) {
                    s.role = SourceRole::PropertyLine;
                    d.landscape->propertyLineSourceId = source;
                    return true;
                }
            e = "property-line source does not exist";
            return false;
        },
        error);
}
bool SetLandscapeSourceRole (edit::Document& doc, Id entity, SourceRole role, std::string& error)
{
    if (role == SourceRole::PathCenterline || role == SourceRole::Terrain) {
        error = "a line cannot be given that role";
        return false;
    }
    if (const auto* line = edit::Find (doc, entity); role == SourceRole::Building && line && !line->closed) {
        error = "a building outline must be a closed line";
        return false;
    }
    return Mutate (
        doc,
        [=] (edit::Document& d, std::string& e) {
            if (edit::Find (d, entity) == nullptr) {
                e = "the line does not exist";
                return false;
            }
            auto& sources = d.landscape->sources;
            auto found = std::find_if (sources.begin (), sources.end (),
                                       [=] (const LandscapeSource& s) { return s.entityId == entity && !s.deleted; });
            if (found == sources.end ()) {
                Id id = 0;
                if (!Allocate (d, id, e))
                    return false;
                LandscapeSource source;
                source.id = id;
                source.entityId = entity;
                sources.push_back (source);
                found = sources.end () - 1;
            }
            const Id previous = d.landscape->propertyLineSourceId;
            if (found->role == SourceRole::Building && role != SourceRole::Building) {
                auto& entrances = d.landscape->entrances;
                const Id id = found->id;
                entrances.erase (std::remove_if (entrances.begin (), entrances.end (),
                                                 [id] (const LandscapeEntrance& e) { return e.source == id; }),
                                 entrances.end ());
            }
            found->role = role;
            if (role == SourceRole::PropertyLine) {
                for (auto& s : sources)
                    if (s.id == previous && s.id != found->id && !s.deleted)
                        s.role = SourceRole::Guide;
                d.landscape->propertyLineSourceId = found->id;
            }
            else if (previous == found->id)
                d.landscape->propertyLineSourceId = 0;
            return true;
        },
        error);
}
} // namespace tapioca::geometry
