#include "SunStudy/SunStudyReuse.hpp"

#include "SunStudy/SunStudyRoles.hpp"

#if defined(TAPIOCA_GEOMETRY_RUST)
#include "SunStudy/SunStudyRustStudy.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <map>

namespace evp::sunstudy {
namespace {

using MeshMap = std::map<std::string, size_t>;

bool IndexMeshes (const geomsrv::Snapshot& snapshot, MeshMap& index)
{
    for (size_t mesh = 0; mesh < snapshot.meshes.size (); ++mesh) {
        if (snapshot.meshes[mesh].vertices.size () % 3 != 0)
            return false;
        for (const double coordinate : snapshot.meshes[mesh].vertices) {
            if (!std::isfinite (coordinate))
                return false;
        }
        const auto guid = CanonicalGuid (snapshot.meshes[mesh].guid);
        if (guid.empty () || !index.emplace (guid, mesh).second)
            return false; // ambiguous identity: recompute, never guess
    }
    return true;
}

} // namespace

void SetSampleMeshes (StudyRecord& record)
{
#if defined(TAPIOCA_GEOMETRY_RUST)
    // The Rust core keeps its own map for its own reuse; the record's stays
    // filled below for every C++ reader.
    if (record.rustCore != nullptr && record.snapshot != nullptr) {
        std::string error;
        record.rustCore->SetSampleMeshes (*record.snapshot, error);
    }
#endif
    if (!record.IsPatchDomain ()) {
        record.sampleMeshes = record.sampleGrid.groups;
        return;
    }
    if (record.snapshot == nullptr)
        return;
    MeshMap meshes;
    if (!IndexMeshes (*record.snapshot, meshes))
        return;
    record.sampleMeshes.assign (record.positions.size () / 3, UINT32_MAX);
    for (const auto& span : record.patchGrid.spans) {
        const auto mesh = meshes.find (CanonicalGuid (span.key.element));
        if (mesh == meshes.end () || span.first > record.sampleMeshes.size () ||
            span.count > record.sampleMeshes.size () - span.first) {
            record.sampleMeshes.clear ();
            return;
        }
        std::fill_n (record.sampleMeshes.begin () + span.first, span.count, static_cast<uint32_t> (mesh->second));
    }
}

size_t ReuseUnaffectedSamples (const StudyRecord& source, StudyRecord& target, const std::atomic<bool>& cancelled)
{
#if defined(TAPIOCA_GEOMETRY_RUST)
    // The plan and the seeding are the core's (RustStudyCore::ReuseFrom); a
    // record without one has nothing to seed.
    if (target.rustCore == nullptr || source.rustCore == nullptr || source.snapshot == nullptr ||
        target.snapshot == nullptr)
        return 0;
    rust::RustStudyCore::ReuseInputs inputs;
    inputs.sourceSnapshot = source.snapshot.get ();
    inputs.sourceRoles = &source.elementRoles;
    inputs.targetSnapshot = target.snapshot.get ();
    inputs.targetRoles = &target.elementRoles;
    inputs.sourceDefaultRayBounds = source.defaultRayBounds;
    inputs.cancelled = &cancelled;
    size_t reused = 0, reusedSteps = 0;
    std::string error;
    if (!target.rustCore->ReuseFrom (*source.rustCore, inputs, reused, reusedSteps, error) || reusedSteps == 0)
        return 0;
    target.reusedSamples = reused;
    target.reusedSampleSteps = reusedSteps;
    return reused;
#else
    (void) source;
    (void) target;
    (void) cancelled;
    return 0;
#endif
}

} // namespace evp::sunstudy
