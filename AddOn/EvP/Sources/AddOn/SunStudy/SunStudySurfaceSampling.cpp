#include "SunStudy/SunStudySurfaceSampling.hpp"

#include "SunStudy/SunStudyRoles.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <cmath>
#include <map>

namespace evp::sunstudy {
namespace {

using MeshIndex = std::map<std::string, size_t>;

bool IndexMeshes (const geomsrv::Snapshot& snapshot, MeshIndex& index)
{
    for (size_t mesh = 0; mesh < snapshot.meshes.size (); ++mesh) {
        const auto guid = CanonicalGuid (snapshot.meshes[mesh].guid);
        if (guid.empty () || !index.emplace (guid, mesh).second)
            return false;
    }
    return true;
}

bool Fits (size_t first, size_t count, size_t size)
{
    return first <= size && count <= size - first;
}

template <class T> void AppendRange (std::vector<T>& into, const std::vector<T>& from, size_t first, size_t count)
{
    into.insert (into.end (), from.begin () + first, from.begin () + first + count);
}

SamplerOptions TriangleOptions (const SurfaceSamplingOptions& options, size_t faceOffset = 0)
{
    SamplerOptions result;
    result.spacing = options.spacing;
    result.normalOffset = options.normalOffset;
    result.jitter = options.jitter;
    result.jitterFaceOffset = static_cast<uint32_t> (faceOffset);
    result.maxSamples = options.maxSamples;
    result.wantLayouts = true;
    return result;
}

PatchSamplerOptions PatchOptions (const SurfaceSamplingOptions& options)
{
    PatchSamplerOptions result;
    result.spacing = options.spacing;
    result.normalOffset = options.normalOffset;
    result.maxSamples = options.maxSamples;
    return result;
}

bool Compatible (const StudyRecord& previous, const SurfaceSamplingOptions& options)
{
    const auto& old = previous.samplingLayout.options;
    if (previous.snapshot == nullptr || !previous.samplingLayout.valid || previous.domain != options.domain ||
        previous.samplingLayout.meshes.size () != previous.snapshot->meshes.size () || old.domain != options.domain ||
        old.spacing != options.spacing || old.normalOffset != options.normalOffset || old.jitter != options.jitter)
        return false;
    // Metadata must partition the source grid in snapshot order. A plausible
    // in-bounds range alone can still copy a DIFFERENT mesh's samples.
    size_t faces = 0, samples = 0, patches = 0;
    const size_t count = previous.IsPatchDomain () ? previous.patchGrid.Count () : previous.sampleGrid.Count ();
    for (size_t mesh = 0; mesh < previous.samplingLayout.meshes.size (); ++mesh) {
        const auto& span = previous.samplingLayout.meshes[mesh];
        if (span.firstFace != faces || span.firstSample != samples || span.firstPatch != patches ||
            span.faceCount != previous.snapshot->meshes[mesh].triangles.size () / 3 ||
            (!span.sampleFaces.empty () && span.sampleFaces.size () != span.faceCount) ||
            !Fits (samples, span.sampleCount, count) || (!span.sampled && span.sampleCount != 0) ||
            !Fits (patches, span.patchCount, previous.patchGrid.spans.size ()))
            return false;
        faces += span.faceCount;
        samples += span.sampleCount;
        patches += span.patchCount;
    }
    return samples == count && patches == previous.patchGrid.spans.size () &&
           faces == (previous.IsPatchDomain () ? previous.patchGrid.patchOfTriangle.size ()
                                               : previous.sampleGrid.layouts.size ());
}

bool CanCopy (const StudyRecord& previous, size_t mesh, const SurfaceMeshSampling& span)
{
    if (span.faceCount != previous.snapshot->meshes[mesh].triangles.size () / 3)
        return false;
    if (!previous.IsPatchDomain ()) {
        const auto& grid = previous.sampleGrid;
        const size_t count = grid.Count ();
        if (!grid.valid || grid.positions.size () != count * 3 || grid.normals.size () != count * 3 ||
            grid.areas.size () != count || grid.groups.size () != count || grid.cellColumns.size () != count ||
            grid.cellRows.size () != count || !Fits (span.firstSample, span.sampleCount, count) ||
            !Fits (span.firstFace, span.faceCount, grid.layouts.size ()))
            return false;
        for (size_t sample = span.firstSample; sample < span.firstSample + span.sampleCount; ++sample) {
            if (grid.groups[sample] != mesh || grid.faces[sample] < span.firstFace ||
                grid.faces[sample] - span.firstFace >= span.faceCount)
                return false;
            if (!span.sampleFaces.empty () && span.sampleFaces[grid.faces[sample] - span.firstFace] == 0)
                return false;
        }
    }
    else {
        const auto& grid = previous.patchGrid;
        const size_t count = grid.Count ();
        if (!grid.valid || grid.positions.size () != count * 3 || grid.normals.size () != count * 3 ||
            grid.spanOf.size () != count || grid.cellColumns.size () != count || grid.cellRows.size () != count ||
            !Fits (span.firstSample, span.sampleCount, count) ||
            !Fits (span.firstFace, span.faceCount, grid.patchOfTriangle.size ()) ||
            !Fits (span.firstPatch, span.patchCount, grid.spans.size ()))
            return false;
        size_t cursor = span.firstSample;
        for (size_t patch = span.firstPatch; patch < span.firstPatch + span.patchCount; ++patch) {
            const auto& item = grid.spans[patch];
            if (item.first != cursor || !Fits (cursor - span.firstSample, item.count, span.sampleCount) ||
                item.key.element != previous.snapshot->meshes[mesh].guid)
                return false;
            for (size_t sample = cursor; sample < cursor + item.count; ++sample) {
                if (grid.spanOf[sample] != patch)
                    return false;
            }
            cursor += item.count;
        }
        if (cursor != span.firstSample + span.sampleCount)
            return false;
        for (size_t sample = span.firstSample; sample < cursor; ++sample) {
            if (grid.spanOf[sample] < span.firstPatch || grid.spanOf[sample] - span.firstPatch >= span.patchCount)
                return false;
        }
        for (size_t face = span.firstFace; face < span.firstFace + span.faceCount; ++face) {
            const auto patch = grid.patchOfTriangle[face];
            if (patch != PatchSampleGrid::kNoPatch &&
                (patch < span.firstPatch || patch - span.firstPatch >= span.patchCount))
                return false;
            if (patch != PatchSampleGrid::kNoPatch && !span.sampleFaces.empty () &&
                span.sampleFaces[face - span.firstFace] == 0)
                return false;
        }
    }
    return true;
}

void AppendTriangles (SampleGrid& into, const SampleGrid& from, const SurfaceMeshSampling& span, size_t firstFace,
                      uint32_t group)
{
    AppendRange (into.positions, from.positions, span.firstSample * 3, span.sampleCount * 3);
    AppendRange (into.normals, from.normals, span.firstSample * 3, span.sampleCount * 3);
    AppendRange (into.areas, from.areas, span.firstSample, span.sampleCount);
    AppendRange (into.cellColumns, from.cellColumns, span.firstSample, span.sampleCount);
    AppendRange (into.cellRows, from.cellRows, span.firstSample, span.sampleCount);
    AppendRange (into.layouts, from.layouts, span.firstFace, span.faceCount);
    for (size_t sample = span.firstSample; sample < span.firstSample + span.sampleCount; ++sample)
        into.faces.push_back (static_cast<uint32_t> (firstFace + from.faces[sample] - span.firstFace));
    into.groups.insert (into.groups.end (), span.sampleCount, group);
    into.degenerateFaces += span.degenerate;
    into.undersizedFaces += span.undersized;
    into.excludedFaces += span.excluded;
}

void AppendPatches (PatchSampleGrid& into, const PatchSampleGrid& from, const SurfaceMeshSampling& span,
                    const std::string& guid)
{
    const size_t firstSample = into.Count ();
    const size_t firstPatch = into.spans.size ();
    AppendRange (into.positions, from.positions, span.firstSample * 3, span.sampleCount * 3);
    AppendRange (into.normals, from.normals, span.firstSample * 3, span.sampleCount * 3);
    AppendRange (into.areas, from.areas, span.firstSample, span.sampleCount);
    AppendRange (into.cellColumns, from.cellColumns, span.firstSample, span.sampleCount);
    AppendRange (into.cellRows, from.cellRows, span.firstSample, span.sampleCount);
    for (size_t sample = span.firstSample; sample < span.firstSample + span.sampleCount; ++sample)
        into.spanOf.push_back (static_cast<uint32_t> (firstPatch + from.spanOf[sample] - span.firstPatch));
    for (size_t patch = span.firstPatch; patch < span.firstPatch + span.patchCount; ++patch) {
        auto item = from.spans[patch];
        item.first = firstSample + item.first - span.firstSample;
        item.key.element = guid;
        into.spans.push_back (std::move (item));
    }
    for (size_t face = span.firstFace; face < span.firstFace + span.faceCount; ++face) {
        const auto patch = from.patchOfTriangle[face];
        into.patchOfTriangle.push_back (
            patch == PatchSampleGrid::kNoPatch ? patch : static_cast<uint32_t> (firstPatch + patch - span.firstPatch));
    }
    into.degenerateFaces += span.degenerate;
    into.centroidPatches += span.undersized;
    into.excludedPatches += span.excluded;
}

// Ambiguous GUIDs cannot index a cache. Keep the original whole-scene sampler
// as a conservative fallback, including its global collision discriminators.
SurfaceSamplingResult BuildFreshScene (const geomsrv::Snapshot& snapshot, const std::vector<uint8_t>& sampleMask,
                                       const SurfaceSamplingOptions& options,
                                       const std::vector<std::vector<uint8_t>>& faceMasks)
{
    SurfaceSamplingResult result;
    std::vector<double> vertices;
    std::vector<uint32_t> triangles, groups;
    std::vector<std::string> elements;
    std::vector<uint8_t> faces;
    for (size_t mesh = 0; mesh < snapshot.meshes.size (); ++mesh) {
        const auto& item = snapshot.meshes[mesh];
        const auto base = static_cast<uint32_t> (vertices.size () / 3);
        vertices.insert (vertices.end (), item.vertices.begin (), item.vertices.end ());
        for (auto vertex : item.triangles)
            triangles.push_back (base + vertex);
        groups.resize (triangles.size () / 3, static_cast<uint32_t> (mesh));
        elements.push_back (item.guid);
        if (!faceMasks.empty ()) {
            if (faceMasks[mesh].empty ())
                faces.insert (faces.end (), item.TriangleCount (), 1);
            else
                faces.insert (faces.end (), faceMasks[mesh].begin (), faceMasks[mesh].end ());
        }
    }
    const auto oriented = OrientOutward (vertices.data (), vertices.size () / 3, triangles.data (),
                                         triangles.size () / 3, groups.data (), result.winding);
    if (options.domain == SamplingDomain::SurfacePatch) {
        auto settings = PatchOptions (options);
        settings.sampleGroup = &sampleMask;
        settings.sampleFace = faceMasks.empty () ? nullptr : &faces;
        result.patches = BuildPatchSampleGrid (vertices.data (), vertices.size () / 3, oriented.data (),
                                               oriented.size () / 3, groups.data (), elements, settings);
        result.valid = result.patches.valid;
        result.generatedSamples = result.patches.Count ();
    }
    else {
        auto settings = TriangleOptions (options);
        settings.sampleGroup = &sampleMask;
        settings.sampleFace = faceMasks.empty () ? nullptr : &faces;
        result.triangles = BuildSampleGrid (vertices.data (), vertices.size () / 3, oriented.data (),
                                            oriented.size () / 3, groups.data (), settings);
        result.valid = result.triangles.valid;
        result.generatedSamples = result.triangles.Count ();
    }
    result.rebuiltMeshes = snapshot.meshes.size ();
    return result.valid ? std::move (result) : SurfaceSamplingResult {};
}

} // namespace

SurfaceSamplingResult BuildSurfaceSampling (const geomsrv::Snapshot& snapshot, const std::vector<uint8_t>& sampleMask,
                                            const SurfaceSamplingOptions& options, const StudyRecord* previous,
                                            const std::function<bool ()>& isCancelled,
                                            const std::vector<std::vector<uint8_t>>& faceMasks)
{
    if (!(options.spacing > 0.0) || !std::isfinite (options.spacing) || !std::isfinite (options.normalOffset) ||
        !std::isfinite (options.jitter) || sampleMask.size () != snapshot.meshes.size () ||
        (!faceMasks.empty () && faceMasks.size () != snapshot.meshes.size ()))
        return {};
    for (size_t mesh = 0; mesh < faceMasks.size (); ++mesh) {
        if (!faceMasks[mesh].empty () && faceMasks[mesh].size () != snapshot.meshes[mesh].TriangleCount ())
            return {};
    }
    size_t faceCount = 0, vertexCount = 0;
    for (const auto& mesh : snapshot.meshes) {
        if (isCancelled && isCancelled ())
            return {};
        if (mesh.vertices.size () % 3 != 0 || mesh.triangles.size () % 3 != 0)
            return {};
        for (const auto coordinate : mesh.vertices) {
            if (!std::isfinite (coordinate))
                return {};
        }
        for (const auto vertex : mesh.triangles) {
            if (vertex >= mesh.vertices.size () / 3)
                return {};
        }
        faceCount += mesh.triangles.size () / 3;
        vertexCount += mesh.vertices.size () / 3;
        if (faceCount >= UINT32_MAX || vertexCount > UINT32_MAX)
            return {};
    }
    if (faceCount == 0 || vertexCount == 0)
        return {};
    MeshIndex current, old;
    if (!IndexMeshes (snapshot, current)) {
        auto result = BuildFreshScene (snapshot, sampleMask, options, faceMasks);
        return (isCancelled && isCancelled ()) ? SurfaceSamplingResult {} : std::move (result);
    }
    const bool canReuse =
        previous != nullptr && Compatible (*previous, options) && IndexMeshes (*previous->snapshot, old);
    SurfaceSamplingResult result;
    result.layout.options = options;
    result.layout.meshes.reserve (snapshot.meshes.size ());
    if (options.domain == SamplingDomain::SurfacePatch)
        result.patches.patchOfTriangle.reserve (faceCount);
    else
        result.triangles.layouts.reserve (faceCount);
    size_t firstFace = 0;
    for (size_t mesh = 0; mesh < snapshot.meshes.size (); ++mesh) {
        if (isCancelled && isCancelled ())
            return {};
        const auto& item = snapshot.meshes[mesh];
        SurfaceMeshSampling span;
        span.firstFace = firstFace;
        span.faceCount = item.triangles.size () / 3;
        span.firstSample = result.triangles.Count () + result.patches.Count ();
        span.firstPatch = result.patches.spans.size ();
        span.sampled = sampleMask[mesh] != 0;
        if (!faceMasks.empty ())
            span.sampleFaces = faceMasks[mesh];
        const auto found = old.find (CanonicalGuid (item.guid));
        bool copied = false;
        if (canReuse && found != old.end ()) {
            const auto& oldMesh = previous->snapshot->meshes[found->second];
            const auto& oldSpan = previous->samplingLayout.meshes[found->second];
            const bool sameJitterSeed = options.domain == SamplingDomain::SurfacePatch || options.jitter <= 0.0 ||
                                        oldSpan.firstFace == firstFace;
            if (item.vertices == oldMesh.vertices && item.triangles == oldMesh.triangles &&
                oldSpan.sampled == span.sampled && oldSpan.sampleFaces == span.sampleFaces && sameJitterSeed &&
                CanCopy (*previous, found->second, oldSpan)) {
                if (oldSpan.sampleCount > options.maxSamples - span.firstSample)
                    return {};
                if (options.domain == SamplingDomain::SurfacePatch)
                    AppendPatches (result.patches, previous->patchGrid, oldSpan, item.guid);
                else
                    AppendTriangles (result.triangles, previous->sampleGrid, oldSpan, firstFace,
                                     static_cast<uint32_t> (mesh));
                span.sampleCount = oldSpan.sampleCount;
                span.patchCount = oldSpan.patchCount;
                span.degenerate = oldSpan.degenerate;
                span.undersized = oldSpan.undersized;
                span.excluded = oldSpan.excluded;
                span.winding = oldSpan.winding;
                ++result.reusedMeshes;
                result.reusedSamples += span.sampleCount;
                copied = true;
            }
        }
        if (!copied) {
            const auto oriented = OrientOutward (item.vertices.data (), item.vertices.size () / 3,
                                                 item.triangles.data (), span.faceCount, nullptr, span.winding);
            auto remaining = options;
            remaining.maxSamples -= span.firstSample;
            SurfaceMeshSampling local;
            local.faceCount = span.faceCount;
            const std::vector<uint8_t> mask { static_cast<uint8_t> (span.sampled) };
            if (options.domain == SamplingDomain::SurfacePatch) {
                auto settings = PatchOptions (remaining);
                settings.sampleGroup = &mask;
                settings.sampleFace = span.sampleFaces.empty () ? nullptr : &span.sampleFaces;
                // A local group 0 is still THIS element's GUID, never its index.
                auto grid = BuildPatchSampleGrid (item.vertices.data (), item.vertices.size () / 3, oriented.data (),
                                                  span.faceCount, nullptr, { item.guid }, settings);
                if (grid.sampleLimitExceeded)
                    return {};
                if (grid.patchOfTriangle.empty ())
                    grid.patchOfTriangle.assign (span.faceCount, PatchSampleGrid::kNoPatch);
                local.sampleCount = grid.Count ();
                local.patchCount = grid.spans.size ();
                local.degenerate = grid.degenerateFaces;
                local.undersized = grid.centroidPatches;
                local.excluded = grid.excludedPatches;
                AppendPatches (result.patches, grid, local, item.guid);
            }
            else {
                auto settings = TriangleOptions (remaining, firstFace);
                settings.sampleGroup = &mask;
                settings.sampleFace = span.sampleFaces.empty () ? nullptr : &span.sampleFaces;
                const std::vector<uint32_t> groups (span.faceCount, 0u);
                auto grid = BuildSampleGrid (item.vertices.data (), item.vertices.size () / 3, oriented.data (),
                                             span.faceCount, groups.data (), settings);
                if (span.faceCount != 0 && !grid.valid)
                    return {};
                local.sampleCount = grid.Count ();
                local.degenerate = grid.degenerateFaces;
                local.undersized = grid.undersizedFaces;
                local.excluded = grid.excludedFaces;
                AppendTriangles (result.triangles, grid, local, firstFace, static_cast<uint32_t> (mesh));
            }
            span.sampleCount = local.sampleCount;
            span.patchCount = local.patchCount;
            span.degenerate = local.degenerate;
            span.undersized = local.undersized;
            span.excluded = local.excluded;
            ++result.rebuiltMeshes;
            result.generatedSamples += span.sampleCount;
        }
        result.winding.groups += span.winding.groups;
        result.winding.closed += span.winding.closed;
        result.winding.flipped += span.winding.flipped;
        result.winding.flippedTriangles += span.winding.flippedTriangles;
        result.layout.meshes.push_back (span);
        firstFace += span.faceCount;
    }
    if (isCancelled && isCancelled ())
        return {};
    result.triangles.valid = options.domain == SamplingDomain::TriangleLegacy;
    result.patches.valid = !result.patches.spans.empty ();
    result.valid = result.triangles.valid || result.patches.valid;
    result.layout.valid = result.valid;
    return result.valid ? std::move (result) : SurfaceSamplingResult {};
}

} // namespace evp::sunstudy
