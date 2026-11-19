#ifndef EVP_ARCHVIZ_DIAGRAMPLANS_HPP
#define EVP_ARCHVIZ_DIAGRAMPLANS_HPP
// The planned floors an exploded diagram draws at its Units and Rooms detail (#201 M2).
//
// Plain values, no planner or HUD types: the Massing workflow's cell complex
// (BuildingTopology.hpp, OverlayHudFloorPlans.cpp) is copied out on the main thread, then
// travels with the DiagramInputSnapshot to the worker. Rings are clear geometry -- inside
// the walls -- so neighbouring flats never share a face. Colours are decided where the
// programme is known, never re-derived here.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::archviz::diagram {
// How much of the building a storey shows: its shell, its flats (with the circulation and
// stairs beside them), its rooms, or its walls (not drawn yet). A storey with no planned
// floor shows its shell at every level.
enum class Detail : uint8_t { Topology, Units, Rooms, Walls };

using PlanPoint = std::array<double, 2>;
using PlanRing = std::vector<PlanPoint>; // counter-clockwise outer, clockwise holes

// One flat or one cell of a planned floor. `rings` is one region: its outer rings and holes.
struct PlannedSpace {
    uint64_t id = 0; // the complex's stable unit or cell id (Reconcile)
    std::vector<PlanRing> rings;
    uint32_t rgba = 0; // 0xRRGGBBAA; 0 draws the neutral grey
};

struct PlannedFloor {
    int story = 0;
    double z = 0, height = 0;
    std::vector<PlannedSpace> units; // flats, then the circulation and stairs no flat holds
    std::vector<PlannedSpace> rooms; // every cell: rooms, corridors, lobbies, stairs
};

struct PlannedBuilding {
    std::string key; // massingbuildings::Group.key
    std::vector<PlannedFloor> floors;
};

struct FloorPlans {
    std::vector<PlannedBuilding> buildings; // sorted by key
    uint64_t revision = 0;                  // the planner's revision they were copied at
};
} // namespace geomsrv::archviz::diagram
#endif
