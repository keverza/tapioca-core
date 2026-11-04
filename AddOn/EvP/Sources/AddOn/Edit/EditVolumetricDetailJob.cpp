#include "Edit/EditVolumetricDetailJob.hpp"

#include "Geometry/GeometryRustTerrain.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <mutex>

namespace tapioca::geometry {
namespace {

constexpr double kAnchorSampleStepMetres = 0.25; // Gradient step; height is sampled on the exact TIN.
constexpr size_t kCancellationPollVertices = 256;

struct PublicationLease {
    uint64_t generation = 0;
    uint64_t owner = 0;
};

std::atomic<uint64_t> s_nextPublisherOwner { 1 };
std::mutex s_publicationMutex;
std::map<std::string, PublicationLease> s_publicationLeases;

uint64_t NextPublisherOwner ()
{
    return s_nextPublisherOwner.fetch_add (1, std::memory_order_relaxed);
}

uint64_t ClaimPublication (Id detail, uint64_t owner)
{
    std::lock_guard<std::mutex> lock (s_publicationMutex);
    auto& lease = s_publicationLeases["volumetric/" + std::to_string (detail)];
    if (lease.generation == std::numeric_limits<uint64_t>::max ())
        return 0;
    ++lease.generation;
    lease.owner = owner;
    return lease.generation;
}

bool PublicationCurrent (Id detail, uint64_t generation, uint64_t owner)
{
    std::lock_guard<std::mutex> lock (s_publicationMutex);
    const auto found = s_publicationLeases.find ("volumetric/" + std::to_string (detail));
    return found != s_publicationLeases.end () && found->second.generation == generation &&
           found->second.owner == owner;
}

void HashBytes (uint64_t& hash, const void* data, size_t size)
{
    const auto* bytes = static_cast<const uint8_t*> (data);
    for (size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
}

void HashNumber (uint64_t& hash, double value)
{
    uint64_t bits = 0;
    static_assert (sizeof bits == sizeof value);
    std::memcpy (&bits, &value, sizeof bits);
    HashBytes (hash, &bits, sizeof bits);
}

const VolumetricDetail* FindDetail (const geomsrv::edit::Document& doc, Id id)
{
    if (!doc.landscape)
        return nullptr;
    const auto found = std::find_if (doc.landscape->volumetricDetails.begin (), doc.landscape->volumetricDetails.end (),
                                     [=] (const VolumetricDetail& detail) { return detail.id == id; });
    return found == doc.landscape->volumetricDetails.end () ? nullptr : &*found;
}

bool TerrainSessionMatches (const geomsrv::edit::Document& doc, const PublishedTerrain& terrain)
{
    return doc.landscape && terrain.session == doc.landscape->sessionUuid &&
           terrain.sourceTerrainGuid == doc.landscape->sourceTerrainGuid;
}

VolumetricDetailOutcome Outcome (const VolumetricDetailJob& job)
{
    VolumetricDetailOutcome outcome;
    outcome.generation = job.generation;
    outcome.publicationGeneration = job.publicationGeneration;
    outcome.session = job.session;
    outcome.detailId = job.detail.id;
    outcome.revision = job.detail.revision;
    outcome.definitionHash = job.definitionHash;
    outcome.anchor = job.detail.anchor;
    outcome.voxelSize = job.voxelSize;
    outcome.level = job.level;
    return outcome;
}

bool Cancelled (const std::shared_ptr<std::atomic_bool>& cancel)
{
    return cancel && cancel->load (std::memory_order_relaxed);
}

VolumetricDetailOutcome CancelOutcome (const VolumetricDetailJob& job)
{
    auto outcome = Outcome (job);
    outcome.cancelled = true;
    outcome.error = "VolumetricDetailCancelled: the request was cancelled";
    return outcome;
}

bool ValidMesh (const VolumetricDetailOutcome& outcome)
{
    if (!outcome.mesh || !outcome.mesh->valid)
        return false;
    const auto& mesh = *outcome.mesh;
    const size_t vertices = mesh.VertexCount ();
    const size_t triangles = mesh.TriangleCount ();
    if (triangles == 0 || mesh.positions.size () % 3 != 0 || mesh.triangles.size () % 3 != 0 ||
        mesh.triMaterial.size () != triangles || mesh.vertexSource.size () != vertices ||
        mesh.triangleSource.size () != triangles ||
        (!mesh.normals.empty () && mesh.normals.size () != mesh.positions.size ()) ||
        outcome.diagnostics.triangles != triangles || outcome.diagnostics.degenerateTriangles != 0)
        return false;
    if (!std::all_of (mesh.positions.begin (), mesh.positions.end (), [] (double value) { return std::isfinite (value); }) ||
        !std::all_of (mesh.normals.begin (), mesh.normals.end (), [] (float value) { return std::isfinite (value); }) ||
        !std::all_of (mesh.triMaterial.begin (), mesh.triMaterial.end (), [] (int32_t material) { return material > 0; }))
        return false;
    geomsrv::Aabb bounds;
    for (size_t index = 0; index < mesh.positions.size (); index += 3)
        bounds.Expand (mesh.positions[index], mesh.positions[index + 1], mesh.positions[index + 2]);
    if (!(bounds == mesh.bounds))
        return false;
    return std::all_of (mesh.triangles.begin (), mesh.triangles.end (),
                        [=] (uint32_t index) { return index < vertices; }) &&
           outcome.contentHash == HashPublishedMesh (mesh) && mesh.revision == outcome.revision &&
           mesh.definitionHash == outcome.definitionHash && mesh.object == "volumetric/" + std::to_string (outcome.detailId) &&
           mesh.family == kVolumetricDetailMeshFamily &&
           mesh.sourceAnchorRevision == (outcome.anchor ? outcome.anchor->terrainRevision : 0);
}

} // namespace

uint64_t HashVolumetricTerrainGeometry (const PublishedTerrain& terrain)
{
    uint64_t hash = 1469598103934665603ull;
    const uint64_t pointCount = terrain.points.size ();
    const uint64_t triangleCount = terrain.triangles.size ();
    HashBytes (hash, &pointCount, sizeof pointCount);
    for (double value : terrain.points)
        HashNumber (hash, value);
    HashBytes (hash, &triangleCount, sizeof triangleCount);
    for (uint32_t value : terrain.triangles)
        HashBytes (hash, &value, sizeof value);
    return hash;
}

bool VolumetricAnchorStale (const VolumetricDetail& detail, const PublishedTerrain* terrain)
{
    if (!detail.anchor)
        return false;
    return terrain == nullptr || terrain->revision != detail.anchor->terrainRevision ||
           HashVolumetricTerrainGeometry (*terrain) != detail.anchor->terrainHash;
}

VolumetricDetailOutcome RunVolumetricDetail (const VolumetricDetailJob& job)
{
    auto outcome = Outcome (job);
    try {
        if (Cancelled (job.cancel))
            return CancelOutcome (job);
        std::string validationError;
        if (job.definitionHash != HashVolumetricDetail (job.detail) ||
            !ValidateVolumetricDetail (job.detail, validationError)) {
            outcome.error = "VolumetricDetailInvalidJob: " + validationError;
            return outcome;
        }
        SdfBuildResult result;
        auto options = job.options;
        options.cancel = job.cancel.get ();
        std::string buildError;
        if (!BuildSdf (job.detail, job.voxelSize, options, result, buildError)) {
            outcome.error = std::move (buildError);
            return outcome;
        }
        if (Cancelled (job.cancel))
            return CancelOutcome (job);

        auto& mesh = result.mesh;
        mesh.bounds = {};
        const Vec3 translation = job.detail.anchor ? job.translation : Vec3 {};
        for (size_t index = 0; index < mesh.positions.size (); index += 3) {
            if ((index / 3) % kCancellationPollVertices == 0 && Cancelled (job.cancel))
                return CancelOutcome (job);
            const double x = mesh.positions[index] + translation.x;
            const double y = mesh.positions[index + 1] + translation.y;
            const double z = mesh.positions[index + 2] + translation.z;
            if (!std::isfinite (x) || !std::isfinite (y) || !std::isfinite (z)) {
                outcome.error = "VolumetricDetailInvalidPlacement: translated position is not finite";
                return outcome;
            }
            mesh.positions[index] = x;
            mesh.positions[index + 1] = y;
            mesh.positions[index + 2] = z;
            mesh.bounds.Expand (x, y, z);
        }
        // Translation is exact in the durable f64 model, but a large finite
        // offset can still round distinct vertices together. Never publish a
        // collapsed triangle with the pre-translation zero-degeneracy stats.
        for (size_t triangle = 0; triangle < mesh.TriangleCount (); ++triangle) {
            if (triangle % kCancellationPollVertices == 0 && Cancelled (job.cancel))
                return CancelOutcome (job);
            const size_t offset = triangle * 3;
            const uint32_t ia = mesh.triangles[offset];
            const uint32_t ib = mesh.triangles[offset + 1];
            const uint32_t ic = mesh.triangles[offset + 2];
            if (ia >= mesh.VertexCount () || ib >= mesh.VertexCount () || ic >= mesh.VertexCount ()) {
                outcome.error = "VolumetricDetailInvalidOutput: translated triangle index is out of range";
                return outcome;
            }
            const auto point = [&mesh] (uint32_t index) {
                const size_t at = static_cast<size_t> (index) * 3;
                return Vec3 { mesh.positions[at], mesh.positions[at + 1], mesh.positions[at + 2] };
            };
            const Vec3 a = point (ia), b = point (ib), c = point (ic);
            const Vec3 ab { b.x - a.x, b.y - a.y, b.z - a.z };
            const Vec3 ac { c.x - a.x, c.y - a.y, c.z - a.z };
            const Vec3 normal { ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z,
                                ab.x * ac.y - ab.y * ac.x };
            const double areaSquared = normal.x * normal.x + normal.y * normal.y + normal.z * normal.z;
            if (!std::isfinite (areaSquared) || areaSquared <= 0) {
                outcome.error = "VolumetricDetailInvalidPlacement: translation collapses a triangle";
                return outcome;
            }
        }
        result.contentHash = HashPublishedMesh (mesh);
        outcome.mesh = std::make_shared<const PublishedMesh> (std::move (result.mesh));
        outcome.contentHash = result.contentHash;
        outcome.diagnostics = result.diagnostics;
        outcome.built = true;
        return outcome;
    }
    catch (const std::exception& exception) {
        outcome = Outcome (job);
        outcome.error = std::string ("VolumetricDetailJob: ") + exception.what ();
        return outcome;
    }
}

std::future<VolumetricDetailOutcome> LaunchVolumetricDetail (VolumetricDetailJob job)
{
    return std::async (std::launch::async, [job = std::move (job)] { return RunVolumetricDetail (job); });
}

VolumetricDetailPublisher::VolumetricDetailPublisher () : owner (NextPublisherOwner ())
{
}

bool VolumetricDetailPublisher::Prepare (const geomsrv::edit::Document& doc, const VolumetricDetailRequest& request,
                                         VolumetricDetailJob& job, std::string& error)
{
    const auto* detail = FindDetail (doc, request.detail);
    if (detail == nullptr || (detailId != kNoId && detailId != request.detail)) {
        error = "VolumetricDetailNotFound: the requested detail is not in this publisher's session";
        return false;
    }
    if (latest == std::numeric_limits<uint64_t>::max ()) {
        error = "VolumetricDetailGenerationExhausted: no further request generation is available";
        return false;
    }
    if (request.level != VolumetricBuildLevel::Preview && request.level != VolumetricBuildLevel::Final) {
        error = "VolumetricDetailInvalidLevel: preview or final is required";
        return false;
    }
    if (request.options.strategy != SdfSamplingStrategy::Dense && request.options.strategy != SdfSamplingStrategy::Sparse) {
        error = "VolumetricDetailInvalidStrategy: dense or sparse sampling is required";
        return false;
    }
    std::string validationError;
    if (!ValidateVolumetricDetail (*detail, validationError)) {
        error = "VolumetricDetailInvalidJob: " + validationError;
        return false;
    }

    VolumetricDetailJob candidate;
    candidate.session = doc.landscape->sessionUuid;
    candidate.detail = *detail;
    candidate.definitionHash = HashVolumetricDetail (*detail);
    candidate.level = request.level;
    candidate.voxelSize = detail->voxelSize * (request.level == VolumetricBuildLevel::Preview
                                                   ? kVolumetricPreviewVoxelScale
                                                   : 1.0);
    if (!std::isfinite (candidate.voxelSize) || candidate.voxelSize <= 0) {
        error = "VolumetricDetailInvalidVoxelSize: the requested level has no finite positive voxel size";
        return false;
    }
    candidate.options = request.options;
    candidate.options.cancel = nullptr;
    candidate.cancel = std::make_shared<std::atomic_bool> (false);
    candidate.options.cancel = candidate.cancel.get ();

    if (detail->anchor) {
        const auto& anchor = *detail->anchor;
        if (!request.terrain || !TerrainSessionMatches (doc, *request.terrain) ||
            VolumetricAnchorStale (*detail, request.terrain.get ())) {
            error = "VolumetricAnchorStale: the detail requires its authored published terrain revision and content";
            return false;
        }
        const std::array<double, 2> query { anchor.x, anchor.y };
        std::vector<TerrainSample> samples;
        TerrainFieldSummary summary;
        if (!SampleTerrain ({ request.terrain->points, request.terrain->triangles }, query, kAnchorSampleStepMetres,
                            samples, summary, error))
            return false;
        if (samples.size () != 1 || !samples.front ().onSurface) {
            error = "VolumetricAnchorOutside: the authored XY anchor is outside the published terrain";
            return false;
        }
        candidate.translation = { anchor.x, anchor.y, samples.front ().height + anchor.zOffset };
        if (!std::isfinite (candidate.translation.x) || !std::isfinite (candidate.translation.y) ||
            !std::isfinite (candidate.translation.z)) {
            error = "VolumetricAnchorOutside: the resolved terrain placement is not finite";
            return false;
        }
    }

    candidate.generation = latest + 1;
    candidate.publicationGeneration = ClaimPublication (request.detail, owner);
    if (candidate.publicationGeneration == 0) {
        error = "VolumetricDetailGenerationExhausted: the global publication generation is exhausted";
        return false;
    }
    if (cancel)
        cancel->store (true, std::memory_order_relaxed);
    latest = candidate.generation;
    detailId = request.detail;
    cancel = candidate.cancel;
    latestOutcome.reset ();
    job = std::move (candidate);
    error.clear ();
    return true;
}

bool VolumetricDetailPublisher::Publish (const geomsrv::edit::Document& current,
                                         std::shared_ptr<const PublishedTerrain> terrain,
                                         VolumetricDetailOutcome outcome, std::string& error)
{
    if (outcome.generation != latest) {
        error = "VolumetricDetailSuperseded: a later request replaced this completion";
        return false;
    }
    if (outcome.cancelled || Cancelled (cancel)) {
        error = "VolumetricDetailCancelled: cancellation published no geometry";
        return false;
    }
    if (!PublicationCurrent (detailId, outcome.publicationGeneration, owner)) {
        error = "VolumetricDetailSuperseded: another publisher owns this detail's latest request";
        return false;
    }
    const auto* detail = FindDetail (current, detailId);
    if (detail == nullptr || outcome.detailId != detailId || outcome.session != current.landscape->sessionUuid ||
        outcome.revision != detail->revision || outcome.definitionHash != HashVolumetricDetail (*detail) ||
        outcome.anchor != detail->anchor) {
        error = "VolumetricDetailStale: the detail changed while the request was evaluated";
        return false;
    }
    const double expectedVoxel = detail->voxelSize * (outcome.level == VolumetricBuildLevel::Preview
                                                          ? kVolumetricPreviewVoxelScale
                                                          : 1.0);
    if ((outcome.level != VolumetricBuildLevel::Preview && outcome.level != VolumetricBuildLevel::Final) ||
        outcome.voxelSize != expectedVoxel) {
        error = "VolumetricDetailStale: the completion's level or voxel size does not match its request";
        return false;
    }
    if (detail->anchor && (!terrain || !TerrainSessionMatches (current, *terrain) ||
                           VolumetricAnchorStale (*detail, terrain.get ()))) {
        error = "VolumetricAnchorStale: the published terrain changed while the request was evaluated";
        return false;
    }
    if (!outcome.built) {
        latestOutcome = std::move (outcome);
        error.clear ();
        return true;
    }
    if (!ValidMesh (outcome)) {
        error = "VolumetricDetailInvalidOutput: the completed mesh, identity, diagnostics or hash is invalid";
        return false;
    }
    latestOutcome = outcome;
    published = outcome;
    EditorMeshStore::Get ().Publish (outcome.mesh);
    error.clear ();
    return true;
}

void VolumetricDetailPublisher::Cancel ()
{
    if (cancel)
        cancel->store (true, std::memory_order_relaxed);
}

} // namespace tapioca::geometry
