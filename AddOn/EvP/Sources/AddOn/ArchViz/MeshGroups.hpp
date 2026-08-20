#ifndef EVP_ARCHVIZ_MESHGROUPS_HPP
#define EVP_ARCHVIZ_MESHGROUPS_HPP

// Per-triangle materials -> contiguous, per-material draw ranges.
//
// ⚠️ THIS IS PHASE 6'S CHIEF RISK AND IT FAILS SILENTLY. `Mesh::triMaterial`
// carries ONE MATERIAL INDEX PER TRIANGLE; a draw call has exactly one material.
// So the index buffer has to be RE-ORDERED by material and submitted as ranges.
// Skip it and every surface of an element renders in the first material's
// colour — which looks like a styling choice, not a bug (plan §6.3). The
// three.js side hit exactly this and its handoff note says the same thing word
// for word.
//
// It is a separate file from SceneCache for one reason: it is the only part of
// the geometry upload path that can be tested with no Archicad, no DevKit and no
// GPU, and CLAUDE.md requires that test in the same commit. It therefore
// includes NOTHING but the standard library — no bgfx, no bx, no ACAPI — so
// `tests/cpp` can compile the real source rather than a copy.
//
// The algorithm is a port of `Commands/ModelViewer/scenegen.py::material_groups`,
// deliberately, so the two renderers cannot disagree about what a group is.
// `test_meshgroups.cpp` mirrors `test_scenegen.py`'s cases.

#include <cstdint>
#include <vector>

namespace geomsrv {
namespace archviz {

// One draw call's worth of the reordered index buffer.
struct MaterialRange {
    int32_t  material   = 0;
    // ⚠️ INDEX-BUFFER UNITS, NOT TRIANGLES — 3 per triangle. bgfx's
    // setIndexBuffer takes a first-index and a count in indices, so storing
    // triangles here would put a factor of three at every call site, which is
    // where it would eventually be forgotten.
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
};

// Reorder `triangles` (3 vertex indices per triangle) so that all triangles
// sharing a material are contiguous, and emit one range per material run.
//
// STABLE: triangles keep their original relative order inside a group, so the
// output is diffable between runs and a fixture stays valid.
//
// ⚠️ RUNS, NOT UNIQUE MATERIALS. A sort by material makes each material appear
// exactly once — but this function does NOT require its input to be sorted and
// does not merge non-adjacent equal materials after the sort, because the sort
// already made them adjacent. If it is ever called on a pre-ordered buffer the
// caller must accept one range per RUN.
//
// Empty in -> empty out, both vectors cleared. `triMaterial` shorter than the
// triangle count is treated as material 0 for the remainder rather than read out
// of bounds: a truncated material array is a bad extraction, and drawing it in
// one colour is recoverable where a crash is not.
void BuildMaterialGroups (const std::vector<uint32_t>& triangles,
                          const std::vector<int32_t>&  triMaterial,
                          std::vector<uint32_t>&       outIndices,
                          std::vector<MaterialRange>&  outRanges);

}   // namespace archviz
}   // namespace geomsrv

#endif
