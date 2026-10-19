#ifndef TAPIOCA_GEOMETRY_RUST_TERRAIN_HPP
#define TAPIOCA_GEOMETRY_RUST_TERRAIN_HPP

#include <cstdint>
#include <string>

namespace geomsrv {
struct Mesh;
struct Snapshot;
} // namespace geomsrv

namespace tapioca::geometry {

// Service facade over the Rust geometry ABI. This is the S1 geometry-only
// legacy ingress gate: it validates the shared geomsrv arrays without changing
// or copying them. Attributed views and authored identity are the next gate;
// this header does not claim D14 identity validation, does not validate material
// or surface catalogs, and contains no terrain math. Catalogs stay C++ owned.
//
// Bulk or unbounded validation should run on a worker. Keep synchronous calls
// to inputs whose measured budget is small enough for the calling thread.

// Read the Rust aggregate ABI version and require Tapioca's supported version.
// On failure outVersion is unchanged (including a version mismatch). error is
// cleared on success and receives the ABI diagnostic or mismatch on failure.
bool ReadRustAbiVersion (uint32_t& outVersion, std::string& error);

// Validate one legacy mesh. Empty geometry is valid. Optional sidecars may be
// absent or full; source body/polygon, when present, must both be full and
// positive. Degenerate triangles are accepted because this is not a topology,
// manifold, winding or area gate. The mesh is never modified.
bool ValidateMesh (const geomsrv::Mesh& mesh, std::string& error);

// Validate snapshot mesh geometry and nonempty GUID uniqueness. Blank GUIDs may
// repeat. Builds only call-scoped mesh descriptors over the existing arrays;
// geometry is never copied. Material/surface catalogs are left untouched and
// are not validated in this increment.
bool ValidateSnapshot (const geomsrv::Snapshot& snapshot, std::string& error);

} // namespace tapioca::geometry

#endif
