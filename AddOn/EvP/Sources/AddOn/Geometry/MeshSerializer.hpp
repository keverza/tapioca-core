#ifndef GEOMETRYSERVER_MESHSERIALIZER_HPP
#define GEOMETRYSERVER_MESHSERIALIZER_HPP

#include "Mesh.hpp"
#include <string>

// msgpack serialization. Numeric arrays are packed as raw bin blobs (native
// little-endian) so the Python client can numpy.frombuffer them zero-copy.
namespace geomsrv {

// Whole snapshot -> msgpack bytes: {snapshotId, meshes:[...]}
std::string SerializeSnapshot (const Snapshot& snap);

// Single mesh -> msgpack bytes (same per-mesh schema, wrapped in a 1-key map).
std::string SerializeMesh (const Mesh& mesh);

} // namespace geomsrv

#endif
