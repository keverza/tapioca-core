#ifndef EVP_ARCHVIZ_EXTRACTIONENVIRONMENT_HPP
#define EVP_ARCHVIZ_EXTRACTIONENVIRONMENT_HPP

// ArchViz/ExtractionEnvironment — the NON-GEOMETRY half of what the extraction
// pass reads out of Archicad: the model's surface pool, and the project's sun.
//
// It is a separate translation unit because `ExtractionThread.cpp` crossed the
// ~1,000-line cap, and this is the seam that was already there rather than a
// place to cut. Everything else in that file is about SLICING — the gate hops,
// the cursor, the budget, the abandonment rules. These two functions are about
// asking Archicad one question each, and neither has anything to do with a
// slice: the materials are read once at the top of a pass, the sun once
// alongside the model handle.
//
// ⚠️ BOTH ARE MAIN THREAD ONLY. `ReadMaterials` is ModelerAPI and `ReadEnvironment`
// is ACAPI; the extraction WORKER calls neither directly, it posts them through
// MainThreadGate like everything else (CLAUDE.md: never call ACAPI from a worker
// thread).

#include <memory>

namespace ModelerAPI {
class Model;
}

namespace geomsrv {
namespace archviz {

class MaterialTable;
struct EnvironmentUpload;

// The model's surface pool -> MaterialTable. MAIN THREAD (it is ModelerAPI).
//
// ⚠️ 1-BASED, AND THE INDEX IS THE POOL'S, NOT ARCHICAD'S ATTRIBUTE INDEX. The
// numbers it produces are what `Mesh::triMaterial` carries — `AttributeIndex::
// GetIndex` on the polygon's material — so they line up with
// MaterialRange::material by construction. Using an Archicad attribute index
// would produce a table that looks right and matches nothing
// (ModelAppearanceCommands.cpp's warning).
std::unique_ptr<MaterialTable> ReadMaterials (const ModelerAPI::Model& model);

// Archicad's own sun, for this project's place and moment. MAIN THREAD.
// False means the place settings could not be read at all; the scene then keeps
// whatever sun it had rather than going dark.
//
// ⚠️ ARCHICAD COMPUTES THE SUN — do not write a solar-position model (plan §3).
// ⚠️ AND THE *STORED* ANGLES ARE THE ANSWER, not recomputed ones. See the long
// note at the implementation; preferring the recomputed pair silently replaces a
// hand-typed sun with a different one.
bool ReadEnvironment (EnvironmentUpload& out);

}   // namespace archviz
}   // namespace geomsrv

#endif
