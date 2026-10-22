#ifndef TAPIOCA_EDIT_TERRAIN_CAPTURE_HPP
#define TAPIOCA_EDIT_TERRAIN_CAPTURE_HPP

#include "Edit/EditTerrainDesignJob.hpp"

namespace geomsrv {
struct Mesh;
}

namespace tapioca::geometry {

struct TerrainCaptureOptions {
    // The HardscapeObject that owns the captured rows; its footprint operation
    // attributes them. Mixed ownership is refused downstream, never guessed.
    ObjectId owner = 0;
    // Caller-owned transient table tokens, NOT authored IDs: the capture table
    // and the native mesh it copies (its one support table). Nonzero, distinct.
    uint64_t table = 0, nativeTable = 0;
};

// Detaches the base terrain from a captured snapshot mesh for the worker. The
// mesh must be the one whose GUID is the session's terrain, and the session must
// name it with its Terrain source (SetLandscapeTerrain). The catalog holds every
// live landscape source (with a native binding for each native GUID), every DAG
// operation with its ordered parents, and the owner; each mesh vertex and face
// is an exact copy of the native mesh's, citing the terrain source and the
// owner's footprint operation. Validated before it is returned; the capture is
// unchanged on refusal. Main-thread safe: reads the document and mesh only.
bool CaptureLandscapeTerrain (const geomsrv::edit::Document& doc, const geomsrv::Mesh& mesh,
                              const TerrainCaptureOptions& options, TerrainCapture& capture, std::string& error);
// Worker-side native solid -> compact attributed surface. New table token,
// explicit Rust support only; exact/merged native vertex and exact face rows.
// Retains the complete original capture as `native`; no C++ surface algorithm.
bool PrepareTerrainSurface (std::shared_ptr<const TerrainCapture> input, uint64_t table, TerrainCapture& surface,
                            std::string& error);

} // namespace tapioca::geometry
#endif
