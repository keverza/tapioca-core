#include "Geometry/GeometryRustSdf.hpp"

#include "GeometryRustFfi.h"

#include <algorithm>
#include <array>
#include <limits>
#include <numeric>
#include <vector>

namespace tapioca::geometry {
namespace {

std::string Failure (TapiocaStatus status, const std::array<char, 256>& buffer)
{
    size_t length = 0;
    while (length < buffer.size () && buffer[length] != '\0')
        ++length;
    return length != 0 ? std::string (buffer.data (), length)
                       : "Tapioca Rust SDF call failed with status " + std::to_string (status);
}

void CopyVec (Vec3 value, double target[3])
{
    target[0] = value.x;
    target[1] = value.y;
    target[2] = value.z;
}

void CopyFrame (const VolumetricFrame& frame, TapiocaSdfEntry& raw)
{
    CopyVec (frame.origin, raw.a);
    for (size_t axis = 0; axis < frame.axes.size (); ++axis)
        CopyVec (frame.axes[axis], raw.axes + axis * 3);
}

bool ProfileRange (const VolumetricPrimitive& primitive, std::vector<double>& pool, TapiocaSdfEntry& raw,
                   std::string& error)
{
    if (pool.size () > UINT32_MAX || primitive.profile.size () > (UINT32_MAX - pool.size ()) / 2) {
        error = "SdfProfileRange: profile pool exceeds 32-bit scalar ranges";
        return false;
    }
    raw.profileOffset = static_cast<uint32_t> (pool.size ());
    raw.profileCount = static_cast<uint32_t> (primitive.profile.size () * 2);
    for (const auto& point : primitive.profile)
        pool.insert (pool.end (), point.begin (), point.end ());
    return true;
}

bool ConvertOperation (const VolumetricOperation& operation, std::vector<TapiocaSdfEntry>& entries,
                       std::vector<double>& profiles, std::string& error)
{
    TapiocaSdfEntry raw {};
    raw.id = operation.id;
    raw.kind = static_cast<uint32_t> (operation.primitive.kind);
    raw.operation = static_cast<uint32_t> (operation.operation);
    raw.profile = static_cast<uint32_t> (operation.profile);
    raw.enabled = operation.enabled ? 1 : 0;
    raw.blendRadius = operation.blendRadius;
    raw.materialId = operation.materialId;
    const auto& primitive = operation.primitive;
    switch (primitive.kind) {
        case VolumetricPrimitiveKind::Sphere:
            CopyVec (primitive.centerOrA, raw.a);
            raw.radius = primitive.radius;
            break;
        case VolumetricPrimitiveKind::RoundedBox:
            CopyFrame (primitive.frame, raw);
            CopyVec (primitive.half, raw.half);
            raw.radius = primitive.radius;
            break;
        case VolumetricPrimitiveKind::Capsule:
            CopyVec (primitive.centerOrA, raw.a);
            CopyVec (primitive.b, raw.b);
            raw.radius = primitive.radius;
            break;
        case VolumetricPrimitiveKind::Extrude:
            CopyFrame (primitive.frame, raw);
            raw.halfHeight = primitive.halfHeight;
            if (!ProfileRange (primitive, profiles, raw, error))
                return false;
            break;
        default:
            error = "SdfEntryKind: unknown volumetric primitive";
            return false;
    }
    entries.push_back (raw);
    return true;
}

bool Convert (const VolumetricDetail& detail, double voxelSize, const SdfBuildOptions& options,
              std::vector<TapiocaSdfEntry>& entries, std::vector<double>& profiles, TapiocaSdfRequest& raw,
              std::string& error)
{
    if (detail.id == kNoId) {
        error = "InvalidId: volumetric detail needs a durable nonzero ID";
        return false;
    }
    entries.reserve (detail.entries.size ());
    for (const auto& entry : detail.entries) {
        if (const auto* operation = std::get_if<VolumetricOperation> (&entry)) {
            if (!ConvertOperation (*operation, entries, profiles, error))
                return false;
        }
        else {
            const auto& group = std::get<VolumetricGroup> (entry);
            TapiocaSdfEntry converted {};
            converted.id = group.id;
            converted.kind = 1;
            converted.profile = static_cast<uint32_t> (group.profile);
            converted.enabled = group.enabled ? 1 : 0;
            converted.isolated = group.isolated ? 1 : 0;
            converted.blendRadius = group.blendRadius;
            entries.push_back (converted);
        }
    }
    raw.entries = { entries.data (), entries.size () };
    raw.profilePoints = { profiles.data (), profiles.size () };
    raw.voxelSize = voxelSize;
    raw.soloOperation = options.soloOperation;
    raw.maxDensePoints = options.maxDensePoints;
    raw.maxSparseStoredPoints = options.maxSparseStoredPoints;
    raw.maxTriangles = options.maxTriangles;
    raw.strategy = static_cast<uint32_t> (options.strategy);
    return true;
}

extern "C" uint8_t PollCancellation (void* context) noexcept
{
    const auto* cancel = static_cast<const std::atomic_bool*> (context);
    return cancel != nullptr && cancel->load (std::memory_order_acquire) ? 1 : 0;
}

SdfDiagnostics Diagnostics (const TapiocaSdfSummary& summary)
{
    SdfDiagnostics diagnostics;
    diagnostics.totalPoints = summary.totalPoints;
    diagnostics.evaluatedPoints = summary.evaluatedPoints;
    diagnostics.storedSparsePoints = summary.storedSparsePoints;
    diagnostics.regions = summary.regions;
    diagnostics.skippedRegions = summary.skippedRegions;
    diagnostics.cells = summary.cells;
    diagnostics.activeCells = summary.activeCells;
    diagnostics.triangles = summary.triangles;
    diagnostics.degenerateTriangles = summary.degenerateTriangles;
    diagnostics.lipschitz = summary.lipschitz;
    std::copy (std::begin (summary.actualSpacing), std::end (summary.actualSpacing),
               diagnostics.actualSpacing.begin ());
    diagnostics.strategy = static_cast<SdfSamplingStrategy> (summary.strategy);
    diagnostics.slabCells = summary.slabCells;
    return diagnostics;
}

bool CountsValid (const TapiocaSdfCounts& counts, std::string& error)
{
    if (counts.positions % 3 != 0 || counts.normals != counts.positions || counts.triangles % 3 != 0 ||
        counts.materials != counts.triangles / 3) {
        error = "SdfOutputShape: query returned inconsistent scalar counts";
        return false;
    }
    return true;
}

} // namespace

bool BuildSdf (const VolumetricDetail& detail, double voxelSize, const SdfBuildOptions& options,
               SdfBuildResult& result, std::string& error)
{
    std::vector<TapiocaSdfEntry> entries;
    std::vector<double> profiles;
    TapiocaSdfRequest request {};
    if (!Convert (detail, voxelSize, options, entries, profiles, request, error))
        return false;
    if (options.cancel != nullptr) {
        // The callback reads but never writes the borrowed atomic.
        request.cancel.context = const_cast<std::atomic_bool*> (options.cancel);
        request.cancel.isCancelled = &PollCancellation;
    }

    std::array<char, 256> buffer {};
    TapiocaSdfCounts counts {};
    TapiocaSdfSummary summary {};
    TapiocaSdfOutput output {};
    output.counts = &counts;
    output.summary = &summary;
    output.queryOnly = 1;
    auto status = tapioca_sdf_build (&request, &output, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = Failure (status, buffer);
        return false;
    }
    if (!CountsValid (counts, error))
        return false;
    const auto expectedCounts = counts;
    const auto expectedHash = summary.contentHash;

    SdfBuildResult candidate;
    candidate.mesh.positions.resize (counts.positions);
    candidate.mesh.normals.resize (counts.normals);
    candidate.mesh.triangles.resize (counts.triangles);
    candidate.mesh.triMaterial.resize (counts.materials);
    output.positions = candidate.mesh.positions.data ();
    output.positionCapacity = candidate.mesh.positions.size ();
    output.normals = candidate.mesh.normals.data ();
    output.normalCapacity = candidate.mesh.normals.size ();
    output.triangles = candidate.mesh.triangles.data ();
    output.triangleCapacity = candidate.mesh.triangles.size ();
    output.materials = candidate.mesh.triMaterial.data ();
    output.materialCapacity = candidate.mesh.triMaterial.size ();
    output.queryOnly = 0;
    status = tapioca_sdf_build (&request, &output, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = Failure (status, buffer);
        return false;
    }
    if (counts.positions != expectedCounts.positions || counts.normals != expectedCounts.normals ||
        counts.triangles != expectedCounts.triangles || counts.materials != expectedCounts.materials ||
        summary.contentHash != expectedHash || !CountsValid (counts, error)) {
        error = "SdfOutputShape: query/fill counts or content hash disagree";
        return false;
    }

    candidate.contentHash = summary.contentHash;
    candidate.diagnostics = Diagnostics (summary);
    auto& mesh = candidate.mesh;
    mesh.object = "volumetric/" + std::to_string (detail.id);
    mesh.family = kVolumetricDetailMeshFamily;
    mesh.revision = detail.revision;
    mesh.definitionHash = HashVolumetricDetail (detail);
    mesh.sourceAnchorRevision = detail.anchor ? detail.anchor->terrainRevision : 0;
    mesh.vertexSource.resize (mesh.VertexCount ());
    mesh.triangleSource.resize (mesh.TriangleCount ());
    std::iota (mesh.vertexSource.begin (), mesh.vertexSource.end (), 0);
    std::iota (mesh.triangleSource.begin (), mesh.triangleSource.end (), 0);
    for (size_t index = 0; index + 2 < mesh.positions.size (); index += 3)
        mesh.bounds.Expand (mesh.positions[index], mesh.positions[index + 1], mesh.positions[index + 2]);
    mesh.valid = true;
    if (HashPublishedMesh (mesh) != candidate.contentHash) {
        error = "SdfHashMismatch: Rust and C++ published-mesh content hashes disagree";
        return false;
    }
    result = std::move (candidate);
    error.clear ();
    return true;
}

} // namespace tapioca::geometry
