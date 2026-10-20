#ifndef TAPIOCA_GEOMETRY_MESH_EXTRACTION_HPP
#define TAPIOCA_GEOMETRY_MESH_EXTRACTION_HPP

#include "VertexWeld.hpp"
#include "WireframeEdges.hpp"

#include <span>

namespace geomsrv {

// SDK-free values at the convex-polygon boundary of GeometryExtractor. Source
// vertices are body-local, zero-based; body/polygon sidecars remain ModelerAPI's
// one-based indices. These are capture correspondence, not authored D14 IDs.
struct ExtractionCorner {
    int32_t vertex;
    double nx, ny, nz;
};

struct ExtractionFace {
    int32_t body, polygon, material, surfaceAttribute;
    const std::vector<WireEdgeKey>& visibleEdges; // Sorted, unique SOURCE-body keys.
};

struct ExtractionCounts {
    size_t degenerateNormals = 0, emptyFanTriangles = 0;
};

// Preserve the extractor's first-three-corner face fallback and >1e-12 normal
// length rule. All source indices are checked before indexing. On failure the
// local corner scratch/counter may have changed, but nothing has been emitted.
// SDK acquisition and the existing non-finite SDK behavior are unchanged.
bool NormalizeExtractionCorners (std::span<const double> bodyPositions, std::vector<ExtractionCorner>& corners,
                                 size_t& degenerateNormals);

// Actual C++ production fan/weld/sidecar oracle. First corner is emitted even
// when every fan piece is roundoff-zero; other corners only for retained pieces,
// in (0,k,k+1) order. Caller resets the welder once per body and retains bounds
// spanning ALL acquired body vertices, including un-emitted vertices.
void AppendExtractedConvex (Mesh& mesh, VertexWelder& welder, std::span<const double> bodyPositions,
                            std::vector<ExtractionCorner>& corners, const ExtractionFace& face,
                            ExtractionCounts& counts);

} // namespace geomsrv
#endif
