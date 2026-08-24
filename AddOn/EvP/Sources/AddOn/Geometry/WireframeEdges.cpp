#include "Geometry/WireframeEdges.hpp"

#include <algorithm>

namespace geomsrv {

WireEdgeKey MakeWireEdgeKey (uint32_t vertexA, uint32_t vertexB)
{
    const uint32_t lo = std::min (vertexA, vertexB);
    const uint32_t hi = std::max (vertexA, vertexB);
    return (uint64_t (lo) << 32u) | uint64_t (hi);
}

uint8_t BuildTriangleWireEdgeMask (const uint32_t vertices[3], const std::vector<WireEdgeKey>& visibleEdges)
{
    const WireEdgeKey edges[3] = {
        MakeWireEdgeKey (vertices[1], vertices[2]),
        MakeWireEdgeKey (vertices[2], vertices[0]),
        MakeWireEdgeKey (vertices[0], vertices[1]),
    };

    uint8_t mask = 0;
    for (uint8_t edge = 0; edge < 3; ++edge) {
        if (std::binary_search (visibleEdges.begin (), visibleEdges.end (), edges[edge]))
            mask |= uint8_t (1u << edge);
    }
    return mask;
}

} // namespace geomsrv
