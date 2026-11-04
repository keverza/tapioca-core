#ifndef TAPIOCA_ARCHVIZ_VOLUMETRICOVERLAY_HPP
#define TAPIOCA_ARCHVIZ_VOLUMETRICOVERLAY_HPP

// #140 SDF-2: derived PublishedMesh -> material-split overlay presentation.
// Material IDs are resolved explicitly; no Archicad surface-pool index is guessed.
// Display state is transient and never enters the document or undo history.

#include "ArchViz/EditLayers.hpp"
#include "Edit/EditVolumetricDetailJob.hpp"

namespace tapioca::geometry {

struct VolumetricMaterial {
    int32_t id = 0;
    uint32_t previewRgba = 0; // 0xRRGGBBAA.
    std::string name;
    std::string nativeSurface; // Optional future shared-Accept mapping; not a pool index.
    bool operator== (const VolumetricMaterial&) const = default;
};

std::vector<VolumetricMaterial> DefaultVolumetricMaterials ();

struct VolumetricDisplayState {
    Id xray = kNoId;
    Id only = kNoId;
    std::vector<Id> focus; // Backing storage for editlayers::Feedback::focus.
};

// The state and Feedback must outlive their binding. Show keeps every detail
// visible and Xrays one; Only isolates one as Ghost and sets the existing focus.
void ShowDetail (VolumetricDisplayState& state, Id id, geomsrv::archviz::editlayers::Feedback& feedback);
void OnlyDetail (VolumetricDisplayState& state, Id id, geomsrv::archviz::editlayers::Feedback& feedback);
void ClearDetailDisplay (VolumetricDisplayState& state, geomsrv::archviz::editlayers::Feedback& feedback);

struct VolumetricOverlayMesh {
    Id detail = kNoId;
    std::shared_ptr<const PublishedMesh> mesh;
};

struct VolumetricOverlayInput {
    std::vector<VolumetricOverlayMesh> details;
    std::vector<VolumetricMaterial> materials;
    VolumetricDisplayState display;
    const VolumetricDetailOutcome* diagnostics = nullptr; // Optional single-detail panel.
};

// Total preflight: unknown/duplicate materials, malformed meshes and a missing
// Only target refuse without changing `layer`. Every source triangle appears in
// exactly one material group. The panel reuses #139 MeshStats for health.
bool BuildVolumetricOverlay (const VolumetricOverlayInput& input, geomsrv::archviz::overlaylayers::Layer& layer,
                             std::string& error);
bool BuildVolumetricDiagnosticsPanel (const VolumetricDetailOutcome& outcome, const PublishedMesh& mesh,
                                      geomsrv::archviz::overlaylayers::Panel& panel, std::string& error);

} // namespace tapioca::geometry

#endif
