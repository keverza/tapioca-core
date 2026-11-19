// RenderBench/ObjScene -- a Wavefront OBJ/MTL scene turned into what the viewer's extraction
// produces: one ElementUpload per material, and a MaterialTable in Archicad's channels.
//
// ⚠️ THE MATERIALS GO THROUGH ARCHICAD'S CHANNELS, NOT AROUND THEM. Archicad carries a shine
// FACTOR of at most 100 (a Blinn-Phong exponent), a 0..1 specular amount, a specular colour and an
// opacity; it has no roughness, no metalness and no IOR. An MTL's Ns is the same exponent, so it
// is clamped exactly as Archicad would store it. The bench then renders what Archicad COULD send,
// which is the only picture whose faults are the renderer's and not the test's.

#ifndef TAPIOCA_RENDERBENCH_OBJSCENE_HPP
#define TAPIOCA_RENDERBENCH_OBJSCENE_HPP

#include "ArchViz/MaterialTable.hpp"
#include "ArchViz/SceneCmdQueue.hpp"

#include <memory>
#include <string>
#include <vector>

namespace renderbench {

struct ObjScene {
    std::vector<std::unique_ptr<geomsrv::archviz::ElementUpload>> elements;
    std::unique_ptr<geomsrv::archviz::MaterialTable> materials;
    std::vector<std::string> materialNames; // by pool index
    float boundsMin[3] = { 0.0f, 0.0f, 0.0f };
    float boundsMax[3] = { 0.0f, 0.0f, 0.0f };
    size_t triangles = 0;
    size_t emissive = 0; // MTL Ke surfaces: drawn as their Kd; the viewer has no emitters
    size_t textured = 0; // #252: materials whose map_Kd image loaded
};

// `yUp`: the file is Y-up (every scene in McGuire's archive is), and is rotated to the viewer's
// Z-up by (x, y, z) -> (x, -z, y), a ROTATION (determinant +1), so windings and normals survive.
// `scale` multiplies every position (metres per file unit).
bool LoadObjScene (const std::string& path, bool yUp, float scale, ObjScene& out, std::string& error);

} // namespace renderbench

#endif
