#include "Geometry/GeometryRustMeshPrep.hpp"
#include "GeometryRustFfi.h"

#include <algorithm>
#include <limits>

namespace tapioca::geometry {
namespace {
bool Failed (TapiocaStatus status, const std::array<char, 256>& buffer, std::string& error)
{
    if (status == TAPIOCA_OK)
        return false;
    const auto end = std::find (buffer.begin (), buffer.end (), '\0');
    error = end != buffer.begin () ? std::string (buffer.begin (), end)
                                   : "Tapioca Rust mesh preparation failed with status " + std::to_string (status);
    return true;
}
} // namespace

bool WeldCorners (std::span<const WeldCorner> corners, std::span<const uint64_t> bodyOffsets, WeldedCorners& result,
                  std::string& error)
{
    if (corners.size () > std::numeric_limits<uint32_t>::max () ||
        corners.size () > std::numeric_limits<size_t>::max () / 3) {
        error = "Weld corner count exceeds index/scalar capacity";
        return false;
    }
    // Explicit POD conversion, never type-pun std::array or owned records.
    std::vector<TapiocaWeldCorner> records;
    records.reserve (corners.size ());
    for (const auto& corner : corners) {
        TapiocaWeldCorner record {};
        record.sourceVertex = corner.sourceVertex;
        std::copy (corner.position.begin (), corner.position.end (), record.position);
        std::copy (corner.normal.begin (), corner.normal.end (), record.normal);
        records.push_back (record);
    }
    const TapiocaWeldRequest input { { records.data (), records.size () },
                                     { bodyOffsets.data (), bodyOffsets.size () } };
    WeldedCorners candidate;
    candidate.positions.resize (corners.size () * 3);
    candidate.normals.resize (candidate.positions.size ());
    candidate.cornerVertices.resize (corners.size ());
    size_t count = 0;
    const TapiocaWeldOutput output { &count,
                                     candidate.positions.data (),
                                     candidate.normals.data (),
                                     candidate.positions.size (),
                                     candidate.cornerVertices.data (),
                                     candidate.cornerVertices.size () };
    std::array<char, 256> buffer {};
    if (Failed (tapioca_weld_corners (&input, &output, buffer.data (), buffer.size ()), buffer, error))
        return false;
    if (count > corners.size () || std::any_of (candidate.cornerVertices.begin (), candidate.cornerVertices.end (),
                                                [count] (uint32_t vertex) { return vertex >= count; })) {
        error = "Rust weld returned an invalid output count/remap";
        return false;
    }
    candidate.positions.resize (count * 3);
    candidate.normals.resize (count * 3);
    result = std::move (candidate);
    error.clear ();
    return true;
}

bool AnalyzeMesh (const MeshAnalysisRequest& request, MeshAnalysis& result, std::string& error)
{
    std::vector<TapiocaEdgeRange> ranges;
    ranges.reserve (request.edgeRanges.size ());
    for (const auto& range : request.edgeRanges)
        ranges.push_back ({ range.offset, range.count });
    const TapiocaMeshAnalysisRequest input { { request.positions.data (), request.positions.size () },
                                             { request.normals.data (), request.normals.size () },
                                             { request.triangles.data (), request.triangles.size () },
                                             { request.visibleEdges.data (), request.visibleEdges.size () },
                                             { ranges.data (), ranges.size () } };
    MeshAnalysis candidate;
    candidate.smoothNormals.resize (request.positions.size ());
    candidate.roundoffZero.resize (request.triangles.size () / 3);
    candidate.wireMasks.resize (candidate.roundoffZero.size ());
    TapiocaMeshAnalysisSummary summary {};
    const TapiocaMeshAnalysisOutput output { &summary,
                                             candidate.smoothNormals.data (),
                                             candidate.smoothNormals.size (),
                                             candidate.roundoffZero.data (),
                                             candidate.wireMasks.data (),
                                             candidate.roundoffZero.size () };
    std::array<char, 256> buffer {};
    if (Failed (tapioca_analyze_mesh (&input, &output, buffer.data (), buffer.size ()), buffer, error))
        return false;
    std::copy (std::begin (summary.bounds.mn), std::end (summary.bounds.mn), candidate.bounds.mn);
    std::copy (std::begin (summary.bounds.mx), std::end (summary.bounds.mx), candidate.bounds.mx);
    candidate.normalsUsable = summary.normalsUsable != 0;
    result = std::move (candidate);
    error.clear ();
    return true;
}
} // namespace tapioca::geometry
