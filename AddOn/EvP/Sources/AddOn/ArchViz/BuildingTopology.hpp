#ifndef EVP_ARCHVIZ_BUILDINGTOPOLOGY_HPP
#define EVP_ARCHVIZ_BUILDINGTOPOLOGY_HPP

// One building as a cell complex: the planned floors (FloorScheme.hpp) cut into cells that never
// overlap -- rooms, circulation, stairs, unassigned floor -- and the walls between them as faces
// each cell shares with at most one other (user, 2026-10-10: "do not represent apartments and
// their rooms as overlapping cells"). A flat, a floor and the building are not cells: they are
// sets of cells, so their geometry is always the union of their children and cannot disagree.
//
//   cells      on wall centre lines; a flat whose rooms are unknown or do not tile it is one cell
//   faces      one per stretch of wall, the cells either side (or what lies outside), its class
//              and thickness; a wall is a face, never a cell
//   apertures  the generator's windows and entrance doors, on the face they open
//   units      flats: their cells, type and rooms; floors: their cells and units
//
// Clear (inside the walls) geometry is derived from cells and face thickness (`Clear`), so the
// preview, the areas and a later bake all use one rule. Read-only: built from finished schemes,
// nothing here plans or writes. Pure C++/Clipper2: no ImGui, no Archicad.
#include "ArchViz/FloorScheme.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::archviz::buildingtopology {
using floorscheme::Ring;
using floorscheme::Vec;

constexpr double kContact = 0.05;   // m: shorter shared stretches are a corner touch, not a wall
constexpr double kCollinear = 1e-3; // m: two cells' edges further apart than this are not one wall

enum class CellKind : uint8_t {
    Room,     // a room of a flat (`room` says which)
    FlatRest, // the part of a flat its rooms leave (a recess, a wall niche)
    Flat,     // a flat whose rooms are unknown: the whole flat
    Corridor,
    Lobby,
    Core,       // a stair (on this floor) of the building's stack
    Unassigned, // floor no flat or circulation takes (storage at a corridor end, mould pieces)
};
// What lies beyond a face that has one cell.
enum class Beyond : uint8_t {
    Cell,      // another cell: the face has two
    Outside,   // open air beyond the outline
    Courtyard, // a hole of the floor
    Neighbour, // another building's floor at this elevation
    Gap,       // floor inside the outline no cell takes: the scheme left it unaccounted
};
enum class FaceClass : uint8_t {
    Internal,      // between two rooms of one flat
    UnitParty,     // between two flats
    UnitCorridor,  // a flat and a corridor or lobby
    CoreWall,      // a stair and anything
    Open,          // between corridors and lobbies: no wall
    Partition,     // anything and unassigned floor
    Facade,        // to open air or a courtyard
    BuildingParty, // to another building
    Unknown,       // to a gap
};
enum class ApertureKind : uint8_t { Window, Door };

struct Cell {
    CellKind kind = CellKind::Flat;
    floorscheme::RoomKind room = floorscheme::RoomKind::Living; // Room cells only
    int floor = -1;
    int unit = -1;  // the flat it belongs to (Room, FlatRest, Flat)
    int stack = -1; // Core cells: the building stack stair it is
    Ring shape;     // counter-clockwise, on wall centre lines
    double area = 0;
    std::vector<int> faces;
};
struct Face {
    int floor = -1;
    Vec a, b;                  // along the boundary of `cells[0]` (counter-clockwise for it)
    int cells[2] = { -1, -1 }; // cells[1] is -1 when `beyond` is not Cell
    Beyond beyond = Beyond::Cell;
    FaceClass kind = FaceClass::Unknown;
    double thickness = 0; // the whole wall
    std::vector<int> apertures;
};
struct Aperture {
    ApertureKind kind = ApertureKind::Window;
    int face = -1;
    int cell = -1; // the room it lights, or the flat cell its door enters
    Vec a, b;      // as the generator drew it, on the clear wall
};
struct Unit {
    int floor = -1;
    int flat = -1;   // index in that floor's scheme
    size_t type = 0; // programme type
    double rooms = 0;
    bool roomsKnown = false; // its rooms tile it; else one Flat cell
    std::vector<int> cells;
};
struct Floor {
    int story = 0;
    double z = 0, height = 0;
    std::vector<Ring> outline; // the scheme's: outer rings counter-clockwise, holes clockwise
    std::vector<int> cells, units;
};
struct Diagnostic {
    std::string code, text;
    int floor = -1;
    Vec at;
};
struct Complex {
    std::vector<Floor> floors;
    std::vector<Cell> cells;
    std::vector<Face> faces;
    std::vector<Aperture> apertures;
    std::vector<Unit> units;
    std::vector<Diagnostic> diagnostics; // partition and matching problems; nothing is repaired
};

// One planned floor of the building, lowest first.
struct FloorInput {
    int story = 0;
    double z = 0, height = 0;
    const floorscheme::Scheme* scheme = nullptr;
    std::vector<Ring> party; // other buildings' floor at this elevation (FloorInput::party of the planner)
};
// `stack`: the building's stairs (Options::holdCores pins); each Core cell is matched to one.
Complex Build (const std::vector<FloorInput>& floors, const std::vector<floorscheme::Pins::Core>& stack = {});

// The wall a face is: Facade and BuildingParty walls stand inside the outline (the whole thickness,
// half of a party wall, is the cell's); the others are centred on the face.
double Thickness (FaceClass kind);
// How much of face `face`'s wall lies inside cell `cell` (0 when the cell is not on it).
double Share (const Complex& complex, int face, int cell);
// Union of `cells` on wall centre lines: a flat, a floor, the building's footprint.
std::vector<Ring> Union (const Complex& complex, const std::vector<int>& cells);

// BuildingTopologyClear.cpp: what lies inside the walls (user, 2026-10-10: facade 0.5 m inside the
// massing, partitions 0.2 m, a 0.3 m slab below each floor's level).
struct Span {
    double z0 = 0, z1 = 0;
};
// `cells` united, less each bounding face's share of its wall (faces between two of the cells are
// no wall of the set: a flat is drawn without its internal walls). Outer rings counter-clockwise.
std::vector<Ring> Clear (const Complex& complex, const std::vector<int>& cells);
// A floor's slab: its cells inside the facade and party walls, from kSlab below its level up to it.
std::vector<Ring> Plate (const Complex& complex, int floor);
Span SlabSpan (const Floor& floor);
// Between a floor's slab and the next one's underside: z to z + height - kSlab.
Span ClearSpan (const Floor& floor);

// BuildingTopologyAccess.cpp: three graphs over the cells, rebuilt from the faces, never stored as
// truth. Sharing a wall is not a way through it: only doors, open circulation and stairs are.
struct Link {
    int from = -1, to = -1; // cells; `to` is -1 for a link to open air or a courtyard (Exterior)
    int face = -1;          // the face it crosses; -1 for a stair between floors
    int aperture = -1;      // the door or window that makes it, if one does
    double length = 0;      // the shared wall (Adjacency, Exterior)
};
struct Graph {
    std::vector<Link> links;
    std::vector<std::vector<int>> at; // each cell's links
};
// Cells that share a wall longer than kContact (corner touches are no wall).
Graph Adjacency (const Complex& complex);
// Ways through: entrance doors, open faces between corridors and lobbies, a stair's side onto
// circulation, and each stack stair to itself on the next floor up.
Graph Access (const Complex& complex);
// Cells with facade to open air or a courtyard (a neighbouring building is blind), with their
// windows.
Graph Exterior (const Complex& complex);
// Cells reachable from `from` over `graph`, `from` included.
std::vector<char> Reachable (const Complex& complex, const Graph& graph, const std::vector<int>& from);

struct UnitFigures {
    double clearArea = 0; // inside its walls
    double frontage = 0;  // facade to open air or a courtyard
    double partyWall = 0; // shared with other flats
    int windows = 0;
    bool entrance = false;     // its door is on a wall it shares with circulation or a stair
    bool reachesStair = false; // from that door to a stair of the stack
};
UnitFigures Figures (const Complex& complex, int unit);
// The rules a plan must keep, from the graphs: every flat has an entrance and reaches a stair;
// every living room, bedroom and alcove has facade; every stack stair stands on every floor.
// Error-level findings (unit.no_entrance, unit.unreachable, room.no_daylight, core.broken_stack).
std::vector<Diagnostic> Check (const Complex& complex);
} // namespace geomsrv::archviz::buildingtopology
#endif
