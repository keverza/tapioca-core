#ifndef EVP_ARCHVIZ_ARCHVIZVERTEX_HPP
#define EVP_ARCHVIZ_ARCHVIZVERTEX_HPP

// The one vertex format ArchViz's mesh shader reads.
//
// ⚠️ THIS STRUCT AND Shaders/varying.def.sc ARE ONE CONTRACT. shaderc stamps a
// bgfx::VertexLayout into every compiled blob from the varying file; if the two
// disagree the shader does not fail, it reads the wrong bytes — which renders as
// geometry that is subtly, confidently wrong. Change one, change the other, and
// re-run tools/shaderc/Build-Shaders.ps1.
//
// Fields the plan's §6.7 lists but this does NOT have yet, deliberately:
//   u,v        — arrives with textures, in Phase 6
//   bc[3]      — barycentrics for the wireframe overlay, and §6.7 already
//                prefers `drawVertexPullingWireframe` over them precisely
//                because our vertices are pre-split by VertexWeld and
//                re-splitting would inflate a large model for a toggle most
//                users leave off. It may never be needed.
// A field costs memory on every vertex of a 12,000-element project; none goes in
// before the pass that reads it.

#include <cstdint>

namespace geomsrv {
namespace archviz {

struct ArchVizVertex {
    // ⚠️ WORLD-SPACE METRES, Z-UP, converted from nothing. Archicad is Z-up and
    // geomsrv::ExtractElement emits world space (Mesh.hpp says so); the camera
    // is given an up vector of {0,0,1} instead of the scene being rotated
    // (plan §6.4). The axis swap (x,y,z)->(x,z,y) that looks equivalent is a
    // MIRROR — determinant -1 — and reverses every triangle's winding against
    // its normals. That has shipped in this repo once.
    float    x, y, z;
    // Archicad's TRUE per-corner normal, as VertexWeld produced it. Not
    // averaged: averaging shades a flat box like a sphere, which also shipped
    // once (plan §3).
    float    nx, ny, nz;
    // Material colour resolved at extraction time, ABGR as bgfx packs it.
    uint32_t abgr;
};

static_assert (sizeof (ArchVizVertex) == 28, "the shader's layout assumes a packed 28-byte vertex");

}   // namespace archviz
}   // namespace geomsrv

#endif
