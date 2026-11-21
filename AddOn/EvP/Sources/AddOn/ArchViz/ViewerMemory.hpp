#ifndef EVP_ARCHVIZ_VIEWERMEMORY_HPP
#define EVP_ARCHVIZ_VIEWERMEMORY_HPP

// ArchViz/ViewerMemory -- what the viewer holds for a model, in bytes, and how long the ray-traced
// structures took to build (#14: "memory and timing on a large project").
//
// One report, filled by DiligentScene::Stats from what the scene actually allocated (buffer and
// texture descriptions, Mesh::Bytes), so the HUD's Model card and archviz.log quote the same
// numbers. The ray-traced acceleration structures' own size is not exposed by Diligent; their
// build scratch is, and it is the same order, so it is reported under that name.
//
// PURE: no ACAPI, no Diligent -- the offline suite tests the sums and the wording.

#include <cstddef>
#include <cstdint>
#include <string>

namespace geomsrv {
namespace archviz {

struct ViewerMemory {
    uint64_t meshGpuBytes = 0;    // vertex, index and texture-coordinate buffers of every element
    uint64_t meshCpuBytes = 0;    // the meshes the scene keeps for sections and tracing (Mesh::Bytes)
    uint64_t textureGpuBytes = 0; // material images with their mips
    uint32_t textureImages = 0;
    uint64_t tracedBufferBytes = 0;  // positions, normals, texture coordinates, indices, materials
    uint64_t tracedScratchBytes = 0; // BLAS + TLAS build scratch, kept for the next rebuild
    uint64_t tracedTriangles = 0;
    double tracedBuildMs = 0.0; // CPU time to flatten and record the last build; 0 = never built
    double msPerSample = 0.0;   // the measured GPU cost of one traced sample; 0 = not yet

    uint64_t GpuBytes () const
    {
        return meshGpuBytes + textureGpuBytes + tracedBufferBytes + tracedScratchBytes;
    }
};

// Bytes of a 2D texture with `mips` levels (each side halving, never below 1) at `bytesPerTexel`.
// `mips` 0 counts the full chain down to 1x1.
uint64_t TextureGpuBytes (uint32_t width, uint32_t height, uint32_t mips, uint32_t bytesPerTexel);

// "12.3 MB" -- one decimal below 100 MB, none above; bytes under 0.05 MB read "0.0 MB".
std::string FormatMegabytes (uint64_t bytes);

// The Model card's "GPU memory" figure: "412 MB (meshes 120, textures 88, traced 204)". The traced
// part appears only once the structures were built; the CPU side is its own figure.
std::string FormatViewerMemoryFigure (const ViewerMemory& memory);

// The archviz.log line, with the counts and timings the card leaves out.
std::string FormatViewerMemoryLog (const ViewerMemory& memory);

} // namespace archviz
} // namespace geomsrv

#endif
