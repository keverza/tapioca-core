#include "ArchViz/VolumetricOverlay.hpp"

#include "ArchViz/MeshGroups.hpp"
#include "Edit/EditMeshStats.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <set>

namespace tapioca::geometry {
namespace {
namespace ol = geomsrv::archviz::overlaylayers;

using MaterialMap = std::map<int32_t, VolumetricMaterial>;

bool Materials (const std::vector<VolumetricMaterial>& input, MaterialMap& materials, std::string& error)
{
    for (const auto& material : input) {
        if (material.id <= 0 || material.name.empty () || !materials.emplace (material.id, material).second) {
            error = "VolumetricMaterialInvalid: IDs and names must be positive/nonempty and IDs unique";
            return false;
        }
    }
    return true;
}

bool BoundsMatch (const PublishedMesh& mesh)
{
    if (mesh.positions.size () % 3 != 0)
        return false;
    geomsrv::Aabb bounds;
    for (size_t index = 0; index < mesh.positions.size (); index += 3)
        bounds.Expand (mesh.positions[index], mesh.positions[index + 1], mesh.positions[index + 2]);
    return bounds == mesh.bounds;
}

bool MeshShape (const VolumetricOverlayMesh& input, std::string& error)
{
    if (input.detail == kNoId || input.mesh == nullptr || !input.mesh->valid) {
        error = "VolumetricOverlayInvalidMesh: detail identity and a valid published mesh are required";
        return false;
    }
    const auto& mesh = *input.mesh;
    const size_t vertices = mesh.VertexCount ();
    const size_t triangles = mesh.TriangleCount ();
    if (mesh.object != "volumetric/" + std::to_string (input.detail) || triangles == 0 ||
        mesh.positions.size () % 3 != 0 || mesh.triangles.size () % 3 != 0 || mesh.triMaterial.size () != triangles ||
        (!mesh.normals.empty () && mesh.normals.size () != mesh.positions.size ())) {
        error = "VolumetricOverlayInvalidMesh: identity or cardinality is malformed";
        return false;
    }
    if (!std::all_of (mesh.positions.begin (), mesh.positions.end (), [] (double value) { return std::isfinite (value); }) ||
        !std::all_of (mesh.normals.begin (), mesh.normals.end (), [] (float value) { return std::isfinite (value); }) ||
        !std::all_of (mesh.triangles.begin (), mesh.triangles.end (), [=] (uint32_t index) { return index < vertices; }) ||
        !std::all_of (mesh.triMaterial.begin (), mesh.triMaterial.end (), [] (int32_t material) { return material > 0; })) {
        error = "VolumetricOverlayInvalidMesh: non-finite geometry, index or material ID";
        return false;
    }
    if (!BoundsMatch (mesh)) {
        error = "VolumetricOverlayInvalidMesh: bounds do not match the complete positions";
        return false;
    }
    return true;
}

ol::PanelItem Row (std::string label, std::string value)
{
    ol::PanelItem row;
    row.kind = ol::ItemKind::Row;
    row.text = std::move (label);
    row.value = std::move (value);
    return row;
}

std::string Number (double value)
{
    char buffer[64];
    const auto result = std::to_chars (buffer, buffer + sizeof buffer, value, std::chars_format::general, 6);
    return result.ec == std::errc {} ? std::string (buffer, result.ptr) : "unavailable";
}

std::string Level (VolumetricBuildLevel level)
{
    return level == VolumetricBuildLevel::Preview ? "Preview" : "Final";
}

ol::Shading DetailShading (const VolumetricDisplayState& display, Id detail)
{
    if (display.only == detail)
        return ol::Shading::Ghost;
    if (display.xray == detail)
        return ol::Shading::Xray;
    return ol::Shading::Lit;
}

bool DiagnosticsMatch (const VolumetricDetailOutcome& outcome, const PublishedMesh& displayed)
{
    if (displayed.object != "volumetric/" + std::to_string (outcome.detailId))
        return false;
    if (!outcome.built)
        return outcome.mesh == nullptr && !outcome.error.empty ();
    return outcome.mesh != nullptr && outcome.revision == displayed.revision &&
           outcome.definitionHash == displayed.definitionHash &&
           outcome.contentHash == HashPublishedMesh (displayed) &&
           HashPublishedMesh (*outcome.mesh) == outcome.contentHash;
}

} // namespace

std::vector<VolumetricMaterial> DefaultVolumetricMaterials ()
{
    return { { 1, 0x8C7B68FFu, "Rock", {} }, { 2, 0xB99B72FFu, "Cave cut", {} } };
}

void ShowDetail (VolumetricDisplayState& state, Id id, geomsrv::archviz::editlayers::Feedback& feedback)
{
    state.xray = id;
    state.only = kNoId;
    state.focus.clear ();
    feedback.focus = nullptr;
}

void OnlyDetail (VolumetricDisplayState& state, Id id, geomsrv::archviz::editlayers::Feedback& feedback)
{
    state.xray = kNoId;
    state.only = id;
    state.focus.assign (1, id);
    feedback.focus = &state.focus;
}

void ClearDetailDisplay (VolumetricDisplayState& state, geomsrv::archviz::editlayers::Feedback& feedback)
{
    state = {};
    feedback.focus = nullptr;
}

bool BuildVolumetricDiagnosticsPanel (const VolumetricDetailOutcome& outcome, const PublishedMesh& mesh,
                                      ol::Panel& panel, std::string& error)
{
    if (!DiagnosticsMatch (outcome, mesh)) {
        error = "VolumetricDiagnosticsMismatch: the outcome does not describe the displayed/last-good mesh";
        return false;
    }
    MeshStats stats;
    if (!ComputeMeshStats (mesh.positions, mesh.triangles, stats, error))
        return false;
    if (!BoundsMatch (mesh)) {
        error = "VolumetricDiagnosticsInvalid: published bounds do not match the positions";
        return false;
    }
    const auto& sampling = outcome.diagnostics;
    ol::Panel candidate;
    candidate.title = "Volumetric detail " + std::to_string (outcome.detailId);
    candidate.tab = "stats";
    candidate.widthPixels = 330;
    candidate.items = {
        Row ("Level", Level (outcome.level) + (outcome.built ? "" : " · refused")),
        Row ("Voxel size", Number (outcome.voxelSize) + " m"),
    };
    if (outcome.built) {
        candidate.items.insert (candidate.items.end (), {
            Row ("Cell size", Number (sampling.actualSpacing[0]) + " / " + Number (sampling.actualSpacing[1]) + " / " +
                                  Number (sampling.actualSpacing[2]) + " m"),
            Row ("Sampling", sampling.strategy == SdfSamplingStrategy::Sparse ? "Sparse 8-sample blocks" : "Dense fallback"),
            Row ("Evaluated / total points", std::to_string (sampling.evaluatedPoints) + " / " +
                                                 std::to_string (sampling.totalPoints)),
            Row ("Stored sparse points", std::to_string (sampling.storedSparsePoints)),
            Row ("Skipped / total regions", std::to_string (sampling.skippedRegions) + " / " +
                                                std::to_string (sampling.regions)),
            Row ("Narrow band", sampling.strategy == SdfSamplingStrategy::Sparse ? "available" : "not used"),
            Row ("Lipschitz bound", Number (sampling.lipschitz)),
            Row ("Active / total cells", std::to_string (sampling.activeCells) + " / " + std::to_string (sampling.cells)),
        });
    }
    else {
        candidate.items.push_back (Row ("Sampling diagnostics", "unavailable (build refused)"));
    }
    candidate.items.insert (candidate.items.end (), {
        Row ("Triangles", std::to_string (mesh.TriangleCount ())),
        Row ("Health", std::string (stats.IsClosed () ? "closed manifold" : "open or non-manifold") + "; " +
                           std::to_string (stats.degenerateTriangleCount) + " degenerate, " +
                           std::to_string (stats.duplicateTriangleCount) + " duplicate, " +
                           std::to_string (stats.interiorTriangleCount) + " interior"),
        Row ("Bounds", Number (mesh.bounds.mn[0]) + ", " + Number (mesh.bounds.mn[1]) + ", " +
                           Number (mesh.bounds.mn[2]) + "  to  " + Number (mesh.bounds.mx[0]) + ", " +
                           Number (mesh.bounds.mx[1]) + ", " + Number (mesh.bounds.mx[2]) + " m"),
        Row ("Mesh topology", std::to_string (stats.vertexCount) + " vertices, " + std::to_string (stats.edgeCount) +
                                  " edges, genus " + std::to_string (stats.genus)),
    });
    if (!outcome.error.empty ())
        candidate.items.push_back (Row ("Reason", outcome.error));
    panel = std::move (candidate);
    error.clear ();
    return true;
}

bool BuildVolumetricOverlay (const VolumetricOverlayInput& input, ol::Layer& layer, std::string& error)
{
    MaterialMap materials;
    if (!Materials (input.materials, materials, error))
        return false;
    std::set<Id> details;
    for (const auto& detail : input.details) {
        if (!MeshShape (detail, error))
            return false;
        if (!details.insert (detail.detail).second) {
            error = "VolumetricOverlayInvalidMesh: duplicate detail identity";
            return false;
        }
        for (const int32_t material : detail.mesh->triMaterial) {
            if (!materials.count (material)) {
                error = "VolumetricMaterialUnmapped: material " + std::to_string (material) + " has no explicit mapping";
                return false;
            }
        }
    }
    if (input.display.only != kNoId && !details.count (input.display.only)) {
        error = "VolumetricOverlaySelection: the Only detail is not present";
        return false;
    }

    ol::Layer candidate;
    candidate.name = "tapioca.volumetric-details";
    candidate.views = ol::Views::ThreeD;
    candidate.lifetime = ol::LayerLifetime::Command;
    candidate.solid = true;
    candidate.graphicsCategory = "Volumetric details";
    for (const auto& detail : input.details) {
        if (input.display.only != kNoId && input.display.only != detail.detail)
            continue;
        const auto& source = *detail.mesh;
        std::vector<uint32_t> indices;
        std::vector<geomsrv::archviz::MaterialRange> ranges;
        geomsrv::archviz::BuildMaterialGroups (source.triangles, source.triMaterial, indices, ranges);
        for (const auto& range : ranges) {
            const auto material = materials.find (range.material);
            if (material == materials.end () || range.firstIndex > indices.size () ||
                range.indexCount > indices.size () - range.firstIndex || range.indexCount % 3 != 0) {
                error = "VolumetricOverlayInvalidGrouping: material grouping is incomplete";
                return false;
            }
            ol::Mesh mesh;
            mesh.points = source.positions;
            mesh.indices.assign (indices.begin () + range.firstIndex,
                                 indices.begin () + range.firstIndex + range.indexCount);
            mesh.normals.reserve (source.normals.size ());
            for (const float normal : source.normals)
                mesh.normals.push_back (normal);
            mesh.rgba = material->second.previewRgba;
            mesh.styled = true;
            mesh.style.shading = DetailShading (input.display, detail.detail);
            mesh.style.cullBack = false;
            mesh.style.behind = input.display.xray == detail.detail ? ol::Behind::Show : ol::Behind::Layer;
            mesh.hoverTitle = material->second.name + " · " + source.object;
            mesh.hoverRows = { { "Material", material->second.name + " (" + std::to_string (range.material) + ")" },
                               { "Triangles", std::to_string (range.indexCount / 3) },
                               { "Definition revision", std::to_string (source.revision) } };
            candidate.meshes.push_back (std::move (mesh));
        }
    }
    if (input.diagnostics != nullptr) {
        const auto detail = std::find_if (input.details.begin (), input.details.end (), [&] (const auto& value) {
            return value.detail == input.diagnostics->detailId;
        });
        if (detail == input.details.end ()) {
            error = "VolumetricDiagnosticsInvalid: the panel needs a mesh from the input detail";
            return false;
        }
        ol::Panel panel;
        if (!BuildVolumetricDiagnosticsPanel (*input.diagnostics, *detail->mesh, panel, error))
            return false;
        candidate.panels.push_back (std::move (panel));
    }
    layer = std::move (candidate);
    error.clear ();
    return true;
}

} // namespace tapioca::geometry
