#ifndef TAPIOCA_EDIT_VOLUMETRICDETAILJOB_HPP
#define TAPIOCA_EDIT_VOLUMETRICDETAILJOB_HPP

// #140 SDF-2: immutable worker values and last-good derived mesh publication.
// C++ owns cancellation, placement and publication; Rust remains host-free.
// No worker or completion mutates the document, terrain, history or native store.

#include "Edit/EditDocument.hpp"
#include "Edit/EditMeshStore.hpp"
#include "Edit/EditPublishedTerrain.hpp"
#include "Geometry/GeometryRustSdf.hpp"

#include <atomic>
#include <future>
#include <memory>
#include <optional>

namespace tapioca::geometry {

enum class VolumetricBuildLevel : uint32_t { Preview = 1, Final = 2 };
inline constexpr double kVolumetricPreviewVoxelScale = 2.0;

// Canonical FNV-1a over published TIN points and triangles only. planHash is a
// field-plan identity, not this mesh-content identity.
uint64_t HashVolumetricTerrainGeometry (const PublishedTerrain& terrain);
// An unanchored detail is never terrain-stale. An anchored one requires the
// exact published terrain revision and canonical geometry hash it authored.
bool VolumetricAnchorStale (const VolumetricDetail& detail, const PublishedTerrain* terrain);

struct VolumetricDetailRequest {
    Id detail = kNoId;
    VolumetricBuildLevel level = VolumetricBuildLevel::Final;
    SdfBuildOptions options; // Its cancel pointer is replaced by job-owned state.
    std::shared_ptr<const PublishedTerrain> terrain;
};

struct VolumetricDetailJob {
    uint64_t generation = 0;
    uint64_t publicationGeneration = 0; // Global arbitration for one store key.
    std::array<uint8_t, 16> session {};
    VolumetricDetail detail;
    uint64_t definitionHash = 0;
    double voxelSize = 0.0;
    VolumetricBuildLevel level = VolumetricBuildLevel::Final;
    SdfBuildOptions options;
    Vec3 translation {}; // Resolved only from the anchored published TIN.
    std::shared_ptr<std::atomic_bool> cancel;
};

struct VolumetricDetailOutcome {
    uint64_t generation = 0;
    uint64_t publicationGeneration = 0;
    std::array<uint8_t, 16> session {};
    Id detailId = kNoId;
    uint64_t revision = 0, definitionHash = 0;
    std::optional<VolumetricTerrainAnchor> anchor;
    double voxelSize = 0.0;
    VolumetricBuildLevel level = VolumetricBuildLevel::Final;
    std::shared_ptr<const PublishedMesh> mesh;
    uint64_t contentHash = 0;
    SdfDiagnostics diagnostics;
    bool built = false, cancelled = false;
    std::string error;
};

// Worker side: no shared document/store access and no throw across the future.
VolumetricDetailOutcome RunVolumetricDetail (const VolumetricDetailJob& job);
std::future<VolumetricDetailOutcome> LaunchVolumetricDetail (VolumetricDetailJob job);

// Main-thread, one-detail single-flight coordinator. A failed Prepare preserves
// the current request and publication. Every new successful Prepare supersedes
// and cooperatively cancels the previous request. A global lease also orders
// independent publishers targeting the same derived-store key.
class VolumetricDetailPublisher {
  public:
    VolumetricDetailPublisher ();
    VolumetricDetailPublisher (const VolumetricDetailPublisher&) = delete;
    VolumetricDetailPublisher& operator= (const VolumetricDetailPublisher&) = delete;

    bool Prepare (const geomsrv::edit::Document& doc, const VolumetricDetailRequest& request,
                  VolumetricDetailJob& job, std::string& error);
    // A current refusal becomes diagnostics only. Cancellation, stale/superseded
    // completion, invalid output and anchor changes never replace the last good.
    bool Publish (const geomsrv::edit::Document& current, std::shared_ptr<const PublishedTerrain> terrain,
                  VolumetricDetailOutcome outcome, std::string& error);
    void Cancel ();

    uint64_t Latest () const
    {
        return latest;
    }
    const VolumetricDetailOutcome* LatestOutcome () const
    {
        return latestOutcome ? &*latestOutcome : nullptr;
    }
    const VolumetricDetailOutcome* LastPublished () const
    {
        return published ? &*published : nullptr;
    }

  private:
    Id detailId = kNoId;
    uint64_t owner = 0;
    uint64_t latest = 0;
    std::shared_ptr<std::atomic_bool> cancel;
    std::optional<VolumetricDetailOutcome> latestOutcome;
    std::optional<VolumetricDetailOutcome> published;
};

} // namespace tapioca::geometry

#endif
