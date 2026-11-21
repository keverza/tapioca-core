#include "ArchViz/ViewerMemory.hpp"

#include <algorithm>
#include <cstdio>

namespace geomsrv {
namespace archviz {

uint64_t TextureGpuBytes (uint32_t width, uint32_t height, uint32_t mips, uint32_t bytesPerTexel)
{
    if (width == 0 || height == 0)
        return 0;
    uint64_t total = 0;
    uint32_t w = width, h = height;
    for (uint32_t level = 0; mips == 0 || level < mips; ++level) {
        total += uint64_t (w) * h * bytesPerTexel;
        if (w == 1 && h == 1)
            break;
        w = (std::max) (w / 2, 1u);
        h = (std::max) (h / 2, 1u);
    }
    return total;
}

std::string FormatMegabytes (uint64_t bytes)
{
    const double mb = double (bytes) / (1024.0 * 1024.0);
    char text[32];
    std::snprintf (text, sizeof (text), mb < 100.0 ? "%.1f MB" : "%.0f MB", mb);
    return text;
}

namespace {

std::string Number (uint64_t bytes)
{
    std::string mb = FormatMegabytes (bytes);
    return mb.substr (0, mb.size () - 3); // drop " MB"
}

} // namespace

std::string FormatViewerMemoryFigure (const ViewerMemory& memory)
{
    std::string text = "GPU " + FormatMegabytes (memory.GpuBytes ()) + " (meshes " + Number (memory.meshGpuBytes) +
                       ", textures " + Number (memory.textureGpuBytes);
    if (memory.tracedBuildMs > 0.0)
        text += ", traced " + Number (memory.tracedBufferBytes + memory.tracedScratchBytes);
    return text + "), CPU " + FormatMegabytes (memory.meshCpuBytes);
}

std::string FormatViewerMemoryLog (const ViewerMemory& memory)
{
    std::string text = "memory: GPU " + FormatMegabytes (memory.GpuBytes ()) + " -- meshes " +
                       FormatMegabytes (memory.meshGpuBytes) + ", textures " +
                       FormatMegabytes (memory.textureGpuBytes) + " (" + std::to_string (memory.textureImages) +
                       " images)";
    if (memory.tracedBuildMs > 0.0) {
        char timing[96];
        std::snprintf (timing, sizeof (timing), "built in %.0f ms", memory.tracedBuildMs);
        text += ", traced " + FormatMegabytes (memory.tracedBufferBytes) + " buffers + " +
                FormatMegabytes (memory.tracedScratchBytes) + " build scratch (" +
                std::to_string (memory.tracedTriangles) + " triangles, " + timing;
        if (memory.msPerSample > 0.0) {
            std::snprintf (timing, sizeof (timing), ", %.2f ms per sample", memory.msPerSample);
            text += timing;
        }
        text += ")";
    }
    return text + "; CPU meshes " + FormatMegabytes (memory.meshCpuBytes);
}

} // namespace archviz
} // namespace geomsrv
