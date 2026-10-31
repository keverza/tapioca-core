#include "GeometryRustFfi.h"

static_assert (sizeof (TapiocaStatus) == 4, "C ABI status must be fixed-width");
static_assert (sizeof (void*) != 8 || sizeof (TapiocaMeshView) == 152, "Rust/C++ mesh layout mismatch");
static_assert (sizeof (void*) != 8 || offsetof (TapiocaMeshView, vertices) == 24,
               "Rust/C++ mesh field offset mismatch");
static_assert (sizeof (void*) != 8 || sizeof (TapiocaSnapshotView) == 56, "Rust/C++ snapshot layout mismatch");
static_assert (sizeof (void*) != 8 || offsetof (TapiocaSnapshotView, meshes) == 40,
               "Rust/C++ snapshot field offset mismatch");

static_assert (sizeof (void*) != 8 || sizeof (TapiocaTerrainRequest) == 368, "Terrain request ABI mismatch");
static_assert (sizeof (void*) != 8 || sizeof (TapiocaTerrainSplitRequest) == 344, "Terrain split ABI mismatch");
static_assert (sizeof (void*) != 8 || sizeof (TapiocaTerrainSurfaceRequest) == 312, "Terrain surface ABI mismatch");
static_assert (sizeof (void*) != 8 || offsetof (TapiocaTerrainSurfaceRequest, provenance) == 32,
               "Surface ABI mismatch");
static_assert (sizeof (TapiocaTerrainMapping) == 32, "Terrain mapping ABI mismatch");
static_assert (sizeof (void*) != 8 || offsetof (TapiocaTerrainOutput, queryOnly) == 56, "Terrain output ABI mismatch");

// S3b.3 records; pinned against the Rust layout tests.
static_assert (sizeof (TapiocaGradingTerm) == 16 && sizeof (TapiocaGradingRow) == 24, "Grading pool ABI mismatch");
static_assert (sizeof (TapiocaGradingFixed) == 16 && sizeof (TapiocaGradingBound) == 24, "Grading pool ABI mismatch");
static_assert (sizeof (TapiocaGradingDifference) == 24 && sizeof (TapiocaGradingSummary) == 24, "Grading ABI mismatch");
static_assert (sizeof (void*) != 8 || sizeof (TapiocaGradingRequest) == 128, "Grading request ABI mismatch");
static_assert (sizeof (void*) != 8 || sizeof (TapiocaGradingOutput) == 24, "Grading output ABI mismatch");
static_assert (sizeof (TapiocaTerrainSample) == 72 && sizeof (TapiocaTerrainFieldSummary) == 64, "Sample ABI mismatch");
static_assert (sizeof (void*) != 8 || sizeof (TapiocaTerrainSampleRequest) == 56, "Sample request ABI mismatch");
static_assert (sizeof (TapiocaTerrainSlope) == 24 && sizeof (TapiocaTerrainDaylight) == 40, "Slope ABI mismatch");
static_assert (sizeof (TapiocaSlopeRequest) == 40 && offsetof (TapiocaSlopeRequest, targetKind) == 32,
               "Inspector slope conversion ABI mismatch");
static_assert (sizeof (TapiocaTerrainEdit) == 208 && sizeof (TapiocaTerrainRing) == 8, "Edit ABI mismatch");
static_assert (sizeof (TapiocaTerrainFaceOwner) == 8 && sizeof (TapiocaTerrainEditReport) == 16, "Edit row mismatch");
static_assert (sizeof (void*) != 8 || sizeof (TapiocaTerrainEditRequest) == 400, "Edit request ABI mismatch");
static_assert (sizeof (void*) != 8 || sizeof (TapiocaTerrainEditOutput) == 72, "Edit output ABI mismatch");
static_assert (sizeof (TapiocaTerrainVolumes) == 40 && sizeof (TapiocaTerrainEditVolumes) == 40, "Volume ABI mismatch");
static_assert (sizeof (void*) != 8 || sizeof (TapiocaTerrainVolumeRequest) == 176, "Volume request ABI mismatch");
static_assert (sizeof (void*) != 8 || sizeof (TapiocaTerrainVolumeOutput) == 48, "Volume output ABI mismatch");
static_assert (sizeof (TapiocaTerrainStation) == 160 && offsetof (TapiocaTerrainStation, rounding) == 152 &&
                   sizeof (TapiocaTerrainHybridLine) == 16,
               "Band ABI mismatch");
static_assert (sizeof (TapiocaTerrainHybridVolumes) == 40 && sizeof (TapiocaTerrainHybridSide) == 24,
               "Band ABI mismatch");
static_assert (sizeof (void*) != 8 || sizeof (TapiocaTerrainHybridRequest) == 360, "Band request ABI mismatch");
static_assert (sizeof (void*) != 8 || offsetof (TapiocaTerrainHybridRequest, provenance) == 80,
               "Band request mismatch");
static_assert (sizeof (void*) != 8 || sizeof (TapiocaTerrainHybridCounts) == 32, "Band counts ABI mismatch");
static_assert (sizeof (void*) != 8 || sizeof (TapiocaTerrainHybridOutput) == 144, "Band output ABI mismatch");
static_assert (sizeof (void*) != 8 || offsetof (TapiocaTerrainHybridOutput, queryOnly) == 136, "Band output mismatch");
static_assert (sizeof (TapiocaTerrainConflict) == 48 && sizeof (TapiocaTerrainClip) == 32, "Conflict ABI mismatch");
static_assert (sizeof (TapiocaTerrainHybridDiagnostic) == 80, "Fixed-control diagnostic ABI mismatch");
static_assert (sizeof (void*) != 8 || sizeof (TapiocaTerrainHybridLockedRequest) == 408,
               "Fixed-control request ABI mismatch");
static_assert (sizeof (void*) != 8 || offsetof (TapiocaTerrainHybridLockedRequest, linePoints) == 360,
               "Fixed-control request offset mismatch");
static_assert (sizeof (void*) != 8 || sizeof (TapiocaTerrainHybridDiagnosticOutput) == 32,
               "Fixed-control diagnostic output ABI mismatch");
static_assert (sizeof (void*) != 8 ||
                   (sizeof (TapiocaTerrainConflictOutput) == 32 && sizeof (TapiocaTerrainClipOutput) == 32),
               "Conflict output mismatch");

extern "C" int TapiocaCppHeaderCheck (void)
{
    uint32_t version = 0;
    return tapioca_version (&version, nullptr, 0) == TAPIOCA_OK && version == TAPIOCA_ABI_VERSION ? 0 : 1;
}
