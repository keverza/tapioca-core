#ifndef TAPIOCA_GEOMETRY_RUST_POLYGON_HPP
#define TAPIOCA_GEOMETRY_RUST_POLYGON_HPP

#include "GeometryEngine.hpp"
#include "GeometryIdentity.hpp"

#include <string>

namespace tapioca::geometry {

struct AttributedPolygon {
    geomsrv::engine::Polygon polygon;
    ProvenanceRequest provenance;
};

// S2b value conversion, not a Boolean/offset/validity gate. Finite empty or >=3
// stored vertices; exactly N vertex/N implicit closing-edge/one face mappings
// (all zero for empty), domains mask 7, no patches. Repeated/explicit closing
// vertices, order, winding and every coordinate bit survive without repair.
// Rust validates all attribution, then returns copied coordinates. The whole
// immutable C++ provenance request is retained unchanged, including native links,
// IDs, parent ordering and historical support; no new identity/table is allocated.
// Whole result publishes only on success; source may alias prior result. Worker
// by default until measured. Does not certify geometric ancestry or hole roles.
bool CopyAttributedPolygon (const AttributedPolygon& source, AttributedPolygon& result, std::string& error);

} // namespace tapioca::geometry
#endif
