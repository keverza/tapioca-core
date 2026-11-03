#ifndef TAPIOCA_GEOMETRY_RUST_SDF_HPP
#define TAPIOCA_GEOMETRY_RUST_SDF_HPP

// #140 SDF-1d: owned C++ record -> fixed POD -> Rust field/sampler/MC facade.
// Query/fill recomputes the same deterministic build. No retained Rust handle,
// production dispatch, terrain write, undo scope or native Accept is enabled.
// Worker-classified until measured; preview/final differ only in voxel size.

#include "Edit/EditPublishedMesh.hpp"
#include "Edit/EditVolumetricDetail.hpp"

#include <atomic>
#include <cstdint>
#include <string>

namespace tapioca::geometry {

inline constexpr uint64_t kVolumetricDetailMeshFamily = 3;

enum class SdfSamplingStrategy : uint32_t { Dense = 1, Sparse = 2 };

struct SdfBuildOptions {
    SdfSamplingStrategy strategy = SdfSamplingStrategy::Sparse;
    uint64_t maxDensePoints = 64'000'000;
    uint64_t maxSparseStoredPoints = 8'000'000;
    uint64_t maxTriangles = 2'000'000;
    Id soloOperation = kNoId; // Transient evaluator cutoff; never persisted here.
    const std::atomic_bool* cancel = nullptr; // Borrowed nonthrowing poll state.
    bool operator== (const SdfBuildOptions&) const = default;
};

struct SdfDiagnostics {
    uint64_t totalPoints = 0, evaluatedPoints = 0, storedSparsePoints = 0;
    uint64_t regions = 0, skippedRegions = 0;
    uint64_t cells = 0, activeCells = 0, triangles = 0, degenerateTriangles = 0;
    double lipschitz = 0.0;
    std::array<double, 3> actualSpacing {};
    SdfSamplingStrategy strategy = SdfSamplingStrategy::Sparse;
    uint32_t slabCells = 0;
    bool operator== (const SdfDiagnostics&) const = default;
};

struct SdfBuildResult {
    PublishedMesh mesh;
    uint64_t contentHash = 0;
    SdfDiagnostics diagnostics;
    bool operator== (const SdfBuildResult&) const = default;
};

// Builds into a provisional result. Any conversion, ABI, cancellation, budget,
// shape or hash failure leaves `result` unchanged and returns a named reason.
bool BuildSdf (const VolumetricDetail& detail, double voxelSize, const SdfBuildOptions& options,
               SdfBuildResult& result, std::string& error);

} // namespace tapioca::geometry

#endif
