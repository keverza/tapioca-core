#ifndef EVP_ARCHVIZ_DIAGRAMGEOMETRY_HPP
#define EVP_ARCHVIZ_DIAGRAMGEOMETRY_HPP
// The draw source of the exploded diagram (#201 §5.8): node-local vertex data built once
// per scene, and the per-frame draw list that places it. Pure and tested offline; the GPU
// half is DiagramLayer, which uploads `DiagramGeometry` once and is handed `DiagramDraw`s.
//
// Geometry never moves: a draw carries only its node's translation, so animating t costs
// a constant per draw and zero bytes of upload. M1 poses are translations (Evaluate
// composes offsets only); a rotation would widen DiagramDraw, not the vertices.
#include "ArchViz/DiagramSnapshot.hpp"
#include <array>

namespace geomsrv::archviz::diagram {
// ⚠️ DiagramLayer's input layout is this struct field for field: position, normal, rgba.
struct DiagramVertex {
    float position[3]; // relative to the node's pivot
    float normal[3];   // the face normal: shells are flat-shaded
    uint32_t rgba;     // 0xRRGGBBAA
};
static_assert (sizeof (DiagramVertex) == 28, "DiagramLayer's input layout assumes 28-byte vertices");

struct DiagramMesh {
    EntityId id;
    uint32_t firstIndex = 0, indexCount = 0;
};

struct DiagramGeometry {
    std::vector<DiagramVertex> vertices;
    std::vector<uint32_t> indices;   // absolute into `vertices`
    std::vector<DiagramMesh> meshes; // sorted by id
    size_t Bytes () const
    {
        return vertices.size () * sizeof (DiagramVertex) + indices.size () * sizeof (uint32_t);
    }
    int Find (const EntityId& id) const;
};

// Fallback colour for a storey with no function colour: the massing's neutral grey.
constexpr uint32_t kDiagramNeutralRgba = 0x9AA0A6FF;

DiagramGeometry BuildGeometry (const BuiltScene& built);

struct DiagramDraw {
    uint32_t mesh = 0;                   // index into DiagramGeometry::meshes
    std::array<float, 3> translation {}; // pivot + offset - origin
    bool selected = false;
};
// One draw per visible node that has a mesh, in plan order. `origin` is the render origin
// the viewProj is expressed against; the viewer draws world metres, so it passes zero.
std::vector<DiagramDraw> DrawsOf (const DiagramGeometry& geometry, const LayoutPlan& plan, const Frame& frame,
                                  const Vec3& origin);
} // namespace geomsrv::archviz::diagram
#endif
