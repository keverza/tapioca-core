#ifndef EVP_ARCHVIZ_WIREFRAMEEDGES_HPP
#define EVP_ARCHVIZ_WIREFRAMEEDGES_HPP

#include <cstdint>
#include <vector>

namespace geomsrv {

using WireEdgeKey = uint64_t;

WireEdgeKey MakeWireEdgeKey (uint32_t vertexA, uint32_t vertexB);

// Bits use triangle-domain order: bit 0 is edge v1-v2, bit 1 is v2-v0,
// and bit 2 is v0-v1. This maps directly to the zero component of barycentrics.
uint8_t BuildTriangleWireEdgeMask (const uint32_t vertices[3], const std::vector<WireEdgeKey>& visibleEdges);

} // namespace geomsrv

#endif
