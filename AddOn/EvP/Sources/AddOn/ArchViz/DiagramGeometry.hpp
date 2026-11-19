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
#include <optional>

namespace geomsrv::archviz::diagram {
// ⚠️ DiagramLayer's input layout is this struct field for field: position, normal, rgba.
struct DiagramVertex {
    float position[3]; // relative to the node's pivot
    float normal[3];   // the face normal: shells are flat-shaded
    uint32_t rgba;     // 0xRRGGBBAA
};
static_assert (sizeof (DiagramVertex) == 28, "DiagramLayer's input layout assumes 28-byte vertices");

// ⚠️ DiagramLayer's LINE input layout is this struct field for field: position, end.
// A line vertex belongs to one of two nodes: `end` 0 is placed by the draw's first
// translation, 1 by its second. An outline uses only the first; a corner connector spans
// two storeys, so its ends move apart as the gap grows. Both are static per publication.
struct DiagramLineVertex {
    float position[3]; // relative to the pivot of the node that places this end
    float end = 0;     // 0 or 1
};
static_assert (sizeof (DiagramLineVertex) == 16, "DiagramLayer's line layout assumes 16-byte vertices");

struct DiagramMesh {
    EntityId id;                      // the storey for its shell; the flat or room for a space
    EntityId node;                    // the plan node that places it: always the storey
    Detail detail = Detail::Topology; // Topology: the storey shell; Units/Rooms: one planned space
    uint32_t firstIndex = 0, indexCount = 0;
    uint32_t firstLine = 0, lineCount = 0; // its outline: line-list vertices in `lines`
};

// Dashed lines from the corners of a storey down to the storey below it in the same
// building, so an exploded stack still reads as one building.
struct DiagramConnector {
    EntityId upper, lower;
    uint32_t firstLine = 0, lineCount = 0;
};

struct DiagramGeometry {
    std::vector<DiagramVertex> vertices;
    std::vector<uint32_t> indices;            // absolute into `vertices`
    std::vector<DiagramMesh> meshes;          // sorted by (node, detail, id)
    std::vector<DiagramLineVertex> lines;     // line list: outlines, then connectors
    std::vector<DiagramConnector> connectors; // in plan order of the upper storey
    size_t Bytes () const
    {
        return vertices.size () * sizeof (DiagramVertex) + indices.size () * sizeof (uint32_t) +
               lines.size () * sizeof (DiagramLineVertex);
    }
    // The meshes a storey has at one detail: its shell, or one per planned space.
    struct Range {
        uint32_t first = 0, count = 0;
        Detail detail = Detail::Topology;
    };
    Range Of (const EntityId& node, Detail detail) const;
    // What a storey shows at `detail`: Rooms, else Units, else its shell. Empty when none.
    Range Shown (const EntityId& node, Detail detail) const;
    // The first mesh of Of (node, detail); -1 when none.
    int Find (const EntityId& node, Detail detail = Detail::Topology) const;
};

// Fallback colour for a storey with no function colour: the massing's neutral grey.
constexpr uint32_t kDiagramNeutralRgba = 0x9AA0A6FF;

// A shell edge is drawn when it bounds the shell or its faces meet at more than this.
// Coplanar triangulation diagonals and the facets of a curved wall stay undrawn.
constexpr double kOutlineCreaseDegrees = 20.0;

DiagramGeometry BuildGeometry (const BuiltScene& built);

// Adds a connector per pair of consecutive stacked storeys of one parent: one line from
// each vertical outline edge of the upper storey (its corners) down to the lower storey's
// top. Needs the plan for the stacking order; geometry only, so it is uploaded once.
void AddConnectors (DiagramGeometry& geometry, const LayoutPlan& plan);

struct DiagramDraw {
    uint32_t mesh = 0;                   // index into DiagramGeometry::meshes
    std::array<float, 3> translation {}; // pivot + offset - origin
    bool selected = false;
};
// One draw per mesh a visible node shows at `detail` (DiagramGeometry::Shown), in plan
// order. A draw is selected when its own id or its storey's is in the frame's selection. `origin` is the render origin
// the viewProj is expressed against; the viewer draws world metres, so it passes zero.
std::vector<DiagramDraw> DrawsOf (const DiagramGeometry& geometry, const LayoutPlan& plan, const Frame& frame,
                                  const Vec3& origin, Detail detail = Detail::Topology);

struct DiagramLineDraw {
    uint32_t firstLine = 0, lineCount = 0;
    std::array<float, 3> first {}, second {}; // translations for end 0 and end 1
    float dashOrigin = 0;                     // world z where a dash starts (the upper end)
    bool dashed = false;
};
// Outlines of the visible meshes, then the connectors whose two storeys are both visible.
// A connector shorter than a centimetre (an assembled stack) is left out.
std::vector<DiagramLineDraw> LineDrawsOf (const DiagramGeometry& geometry, const LayoutPlan& plan, const Frame& frame,
                                          const Vec3& origin, bool outlines, bool connectors,
                                          Detail detail = Detail::Topology);

// What a click hits (§5.8): the bodies each visible storey shows at `detail` -- its shell,
// or its planned flats or rooms -- tested exactly. The hit's id is the storey for a shell
// and the space for a flat or room.
std::optional<PickHit> PickShown (const Frame& frame, const LayoutPlan& plan, const DiagramGeometry& geometry,
                                  const std::vector<PickMesh>& shells, const std::vector<DetailMesh>& details,
                                  Detail detail, const Vec3& origin, const Vec3& direction);
} // namespace geomsrv::archviz::diagram
#endif
