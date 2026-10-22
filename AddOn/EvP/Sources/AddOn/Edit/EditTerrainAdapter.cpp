#include "Edit/EditTerrainAdapter.hpp"
#include "Edit/EditTerrainAdapterInternal.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditLandscape.hpp"
#include <algorithm>
#include <cmath>
#include <exception>
#include <map>
#include <set>

namespace tapioca::geometry {
namespace terrain_detail {

bool ValidateInput (const geomsrv::edit::Document& doc, const EditTerrainInput& input, std::string& error)
{
    const auto& provenance = input.provenance;
    if (!doc.landscape || input.revision != doc.revision || provenance.expectedRevision != doc.revision ||
        provenance.expectedSession != doc.landscape->sessionUuid) {
        error = "ProvenanceStale: terrain input does not belong to this landscape session/revision";
        return false;
    }
    if (std::any_of (input.mesh.points.begin (), input.mesh.points.end (),
                     [] (double value) { return !std::isfinite (value); }) ||
        std::any_of (input.mesh.triangles.begin (), input.mesh.triangles.end (),
                     [&] (uint32_t index) { return index >= input.mesh.points.size () / 3; })) {
        error = "TerrainShape: terrain requires finite coordinates and valid triangle indices";
        return false;
    }
    if (input.sourceTerrainGuid != doc.landscape->sourceTerrainGuid) {
        error = "ProvenanceStale: terrain capture names a different baseline";
        return false;
    }
    if (input.mesh.points.size () % 3 != 0 || input.mesh.triangles.size () % 3 != 0 ||
        provenance.domains != kMeshProvenanceDomains ||
        provenance.counts != EntityCounts { input.mesh.points.size () / 3, 0, input.mesh.triangles.size () / 3, 0 }) {
        error = "ProvenanceUnattributable: terrain requires exact vertex and face attribution";
        return false;
    }
    // The Geometry facade validates the entire table, including native links,
    // historical support and its independently supplied expected framing.
    ProvenanceDiagnostic diagnostic = ProvenanceDiagnostic::None;
    if (!ValidateProvenance (provenance, diagnostic, error))
        return false;
    if (diagnostic != ProvenanceDiagnostic::None) {
        if (error.empty ())
            error = "ProvenanceUnattributable: terrain attribution refused";
        return false;
    }
    return true;
}

bool ValidateOperations (const geomsrv::edit::Document& doc, const ProvenanceRequest& provenance,
                         std::span<const OperationId> operations, std::string& error)
{
    if (!doc.landscape) {
        error = "ProvenanceStale: terrain operation has no landscape session";
        return false;
    }
    std::vector<Id> order;
    if (!ValidateLandscapeDag (*doc.landscape, order, error))
        return false;
    std::map<Id, size_t> rank;
    for (size_t i = 0; i < order.size (); ++i)
        rank.emplace (order[i], i);
    std::set<OperationId> catalog;
    for (const auto& operation : provenance.table.operations)
        catalog.insert (operation.id);
    std::set<OperationId> seen;
    size_t previous = 0;
    for (OperationId operation : operations) {
        const auto found = std::find_if (doc.landscape->operations.begin (), doc.landscape->operations.end (),
                                         [&] (const auto& op) { return op.id == operation; });
        const auto position = rank.find (operation);
        if (operation == 0 || !seen.insert (operation).second || !catalog.count (operation) ||
            found == doc.landscape->operations.end () || !found->enabled || position == rank.end () ||
            (seen.size () > 1 && position->second <= previous)) {
            error = "ProvenanceUnattributable: terrain edits require unique enabled DAG operations in evaluation order";
            return false;
        }
        previous = position->second;
    }
    return true;
}
} // namespace terrain_detail
namespace {

bool Evaluate (const geomsrv::edit::Document& doc, const EditTerrainRequest& request, EditTerrainResult& candidate,
               std::string& error)
{
    if (!terrain_detail::ValidateInput (doc, request.input, error))
        return false;
    std::vector<TerrainEdit> edits;
    std::vector<OperationId> operations;
    for (const auto& operation : request.operations) {
        edits.push_back (operation.parameters);
        operations.push_back (operation.operation);
    }
    if (!terrain_detail::ValidateOperations (doc, request.input.provenance, operations, error))
        return false;
    if ((!request.operations.empty () || request.smoothing) && request.property.empty ()) {
        error = "PropertyLineOutside: terrain deformation requires a property line";
        return false;
    }
    TerrainEditResult edited;
    const auto& input = request.input;
    if (!EditTerrain ({ input.mesh, edits, request.surfaces, request.property, request.protectedRings,
                        request.smoothing, input.provenance },
                      edited, error))
        return false;
    if (edited.heights.size () != input.mesh.points.size () / 3 || edited.owners.size () != edited.heights.size () ||
        edited.reports.size () != operations.size ()) {
        error = "TerrainOutputShape: terrain adapter received incomplete edit output";
        return false;
    }
    candidate.revision = input.revision;
    candidate.sourceTerrainGuid = input.sourceTerrainGuid;
    candidate.points.assign (input.mesh.points.begin (), input.mesh.points.end ());
    candidate.triangles.assign (input.mesh.triangles.begin (), input.mesh.triangles.end ());
    candidate.inputProvenance = std::move (edited.inputProvenance);
    for (size_t i = 0; i < edited.heights.size (); ++i) {
        candidate.points[i * 3 + 2] = edited.heights[i];
        candidate.vertexOperations.push_back (
            edited.owners[i] ? std::optional<OperationId> (operations.at (*edited.owners[i])) : std::nullopt);
    }
    for (const auto& face : edited.faces) {
        if (face.face >= input.mesh.triangles.size () / 3) {
            error = "TerrainOutputShape: terrain adapter received an unknown face";
            return false;
        }
        candidate.faces.push_back ({ face.face, operations.at (face.edit) });
    }
    for (size_t i = 0; i < edited.reports.size (); ++i)
        candidate.reports.push_back ({ operations.at (i), edited.reports[i] });
    for (const auto& clip : edited.clips) {
        if (clip.vertex >= input.mesh.points.size () / 3) {
            error = "TerrainOutputShape: terrain adapter received an unknown clipped vertex";
            return false;
        }
        candidate.clips.push_back ({ operations.at (clip.edit), clip.vertex, clip.kind, clip.distance, clip.withheld });
    }
    if (!SampleTerrain ({ candidate.points, candidate.triangles }, request.queriesXY, request.sampleStep,
                        candidate.samples, candidate.summary, error))
        return false;
    if (request.volumes) {
        TerrainVolumeRequest volume;
        volume.existing = input.mesh;
        volume.proposed = TerrainVolumeRequest::Proposed::Edited;
        volume.heights = edited.heights;
        volume.owners = edited.owners;
        volume.cell = request.volumes->cell;
        volume.subdivisions = request.volumes->subdivisions;
        volume.property = request.property;
        TerrainVolumeReport measured;
        if (!MeasureTerrainVolumes (volume, measured, error))
            return false;
        EditTerrainVolumes report;
        report.site = measured.site;
        report.context = measured.context;
        report.cells = measured.cells;
        for (const auto& operation : measured.edits)
            report.operations.push_back (
                { operations.at (operation.edit), operation.cut, operation.fill, operation.net, operation.area });
        candidate.volumes = std::move (report);
    }
    return true;
}
} // namespace

bool EvaluateEditTerrain (const geomsrv::edit::Document& doc, const EditTerrainRequest& request,
                          EditTerrainResult& result, std::string& error)
{
    try {
        EditTerrainResult candidate;
        if (!Evaluate (doc, request, candidate, error))
            return false;
        result = std::move (candidate);
        error.clear ();
        return true;
    }
    catch (const std::exception& exception) {
        error = std::string ("TerrainAdapterRefused: ") + exception.what ();
        return false;
    }
}
} // namespace tapioca::geometry
