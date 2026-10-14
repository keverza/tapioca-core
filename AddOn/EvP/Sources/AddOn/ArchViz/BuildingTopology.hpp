#ifndef EVP_ARCHVIZ_BUILDINGTOPOLOGY_HPP
#define EVP_ARCHVIZ_BUILDINGTOPOLOGY_HPP

// One building as a cell complex: the planned floors (FloorScheme.hpp) cut into cells that never
// overlap -- rooms, circulation, stairs, unassigned floor -- and the walls between them as faces
// each cell shares with at most one other (user, 2026-10-10: "do not represent apartments and
// their rooms as overlapping cells"). A flat, a floor and the building are not cells: they are
// sets of cells, so their geometry is always the union of their children and cannot disagree.
//
//   cells      solids: a footprint on wall centre lines raised from the floor's level to the next
//              floor's (`z0`, `z1`); a flat whose rooms are unknown or do not tile it is one cell
//   faces      vertical: one per stretch of wall, its height the storey's; horizontal: one per
//              patch of slab two stacked cells share, or a cell's roof, ground or underside. Each
//              has the cells either side (or what lies beyond), its class and thickness; a wall
//              or slab is a face, never a cell
//   apertures  the generator's windows and entrance doors, on the face they open
//   units      flats: their cells, type and rooms; floors: their cells and units
//   context    cells around a part of a floor planned on its own (a unit: the corridor its door
//              opens onto, the flats beside it); its walls are classified against them, nothing
//              else counts them
//   ids        units and cells keep their id while a rebuild finds them again (`Reconcile`); a
//              split or merge gives new ids that name what they came from. Faces are rebuilt.
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
constexpr double kPatch = 0.01;     // m2: a smaller patch of slab is a touch, not a face
constexpr double kStacked = 0.05;   // m: a floor this near the top of the one below stands on it

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
    Ground,    // below the lowest floor
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
    // Horizontal (user, 2026-10-10: a 0.3 m slab below each floor's level):
    Slab,      // between a cell and the one above it
    Shaft,     // between a stack stair and itself one floor up: its opening, no slab
    Roof,      // a cell's top to open air
    Ground,    // the lowest floor's bottom
    Underside, // a cell's bottom over open air (an overhang)
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
    double z0 = 0, z1 = 0; // its solid: the floor's level to the next floor's
    std::vector<int> faces;
    bool context = false;              // around the planned part (FloorInput::context): in no unit, floor or check
    uint64_t id = 0;                   // kept across rebuilds (`Reconcile`); 0 for context
    std::vector<uint64_t> derivedFrom; // the ids it replaced when it is new (a split, a merge)
};
struct Face {
    int floor = -1;
    bool horizontal = false;
    Vec a, b;                  // vertical: along the boundary of `cells[0]` (counter-clockwise for it)
    double z0 = 0, z1 = 0;     // vertical: from the floor's level to the next; horizontal: its level twice
    std::vector<Ring> region;  // horizontal: the patch, outer rings counter-clockwise, holes clockwise
    int cells[2] = { -1, -1 }; // cells[1] is -1 when `beyond` is not Cell; horizontal: [0] below, [1] above
    Beyond beyond = Beyond::Cell;
    FaceClass kind = FaceClass::Unknown;
    double thickness = 0; // the whole wall or slab
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
    uint64_t id = 0; // kept across rebuilds (`Reconcile`)
    std::vector<uint64_t> derivedFrom;
};
struct Floor {
    int story = 0;
    double z = 0, height = 0;
    std::vector<Ring> outline; // the scheme's: outer rings counter-clockwise, holes clockwise
    std::vector<int> cells, units;
    std::vector<int> context; // cells around the planned part, not in `cells`
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

// Floor around a part of a floor planned on its own, as a cell of `kind` on wall centre lines:
// the corridor or stair a unit's door opens onto, a flat or unassigned floor beside it.
struct Context {
    CellKind kind = CellKind::Corridor;
    Ring shape;
};
// One planned floor of the building, lowest first.
struct FloorInput {
    int story = 0;
    double z = 0, height = 0;
    const floorscheme::Scheme* scheme = nullptr;
    std::vector<Ring> party; // other buildings' floor at this elevation (FloorInput::party of the planner)
    std::vector<Context> context;
};
// `stack`: the building's stairs (Options::holdCores pins); each Core cell is matched to one.
Complex Build (const std::vector<FloorInput>& floors, const std::vector<floorscheme::Pins::Core>& stack = {});
// What lies beyond `unit`'s outline, read off its faces: facade to open air or a courtyard, a
// corridor, a lobby or a stair, blind to anything else -- for laying its rooms again
// (floorscheme::PlanRooms).
std::vector<floorscheme::Side> Sides (const Complex& complex, int unit);
// The rest of `unit`'s floor as context: to plan that unit on its own among what surrounds it.
std::vector<Context> Around (const Complex& complex, int unit);

// The wall a face is: Facade and BuildingParty walls stand inside the outline (the whole thickness,
// half of a party wall, is the cell's); the others are centred on the face. A slab or roof lies
// in the top of the cell below it; a ground slab or an underside below its cell.
double Thickness (FaceClass kind);
// How much of face `face`'s wall lies inside cell `cell` (0 when the cell is not on it).
double Share (const Complex& complex, int face, int cell);
// Union of `cells` on wall centre lines: a flat, a floor, the building's footprint.
std::vector<Ring> Union (const Complex& complex, const std::vector<int>& cells);

// Cells as solids, the way the reference cell complexes answer them: a cell's volume and a face's
// area; the faces a set of cells shares with everything else (its envelope: facade, party walls,
// roofs, slabs to other cells) and the faces between two of its cells; the faces two cells share;
// the cells across any face of one, beside it or above and below.
double Volume (const Complex& complex, int cell);
double FaceArea (const Complex& complex, int face);
std::vector<int> Boundary (const Complex& complex, const std::vector<int>& cells);
std::vector<int> Internal (const Complex& complex, const std::vector<int>& cells);
std::vector<int> Shared (const Complex& complex, int a, int b);
std::vector<int> Neighbours (const Complex& complex, int cell);

// BuildingTopologyIds.cpp: `after`'s units and cells take the ids of `before`'s they are (the
// same story, the same kind of space, at least kSame of their joint area common); the others get
// new ids from `next`, naming in `derivedFrom` what they overlap (a split, a merge, a moved
// wall that took most of a flat). With an empty `before` every id is new, in order, so planning
// the same input twice gives the same ids.
constexpr double kSame = 0.6;
void Reconcile (const Complex& before, Complex& after, uint64_t& next);
int UnitById (const Complex& complex, uint64_t id); // -1 when none
int CellById (const Complex& complex, uint64_t id);

// BuildingTopologyExport.cpp: the complex as JSON (tapioca.cell-complex, version 1) -- floors,
// cells as solids, vertical and horizontal faces with their classes, apertures, units, ids and
// the three graphs -- for an offline viewer or a fixture. World metres. `indent` 0: one line.
std::string ToJson (const Complex& complex, size_t indent = 1);

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
    int face = -1;          // the face it crosses; -1 for a stair between floors with no shaft face
    int aperture = -1;      // the door or window that makes it, if one does
    double length = 0;      // the shared wall (Adjacency, Exterior); 0 across a slab
    double area = 0;        // the shared face
};
struct Graph {
    std::vector<Link> links;
    std::vector<std::vector<int>> at; // each cell's links
};
// Cells that share a wall longer than kContact (corner touches are no wall), or a slab patch of
// more than kPatch with the cell above.
Graph Adjacency (const Complex& complex);
// Ways through: entrance doors, open faces between corridors and lobbies, a stair's side onto
// circulation, and each stack stair to itself on the next floor up (through its shaft face).
Graph Access (const Complex& complex);
// Cells with facade to open air or a courtyard (a neighbouring building is blind), with their
// windows. Walls only: a roof is no facade for a window.
Graph Exterior (const Complex& complex);
// Cells reachable from `from` over `graph`, `from` included.
std::vector<char> Reachable (const Complex& complex, const Graph& graph, const std::vector<int>& from);

struct UnitFigures {
    double clearArea = 0; // inside its walls
    double frontage = 0;  // facade to open air or a courtyard
    double partyWall = 0; // shared with other flats
    int windows = 0;
    bool entrance = false; // its door is on a wall it shares with circulation or a stair
    // From that door to a stair of the stack, or to context circulation (it leads to one beyond
    // the planned part).
    bool reachesStair = false;
};
UnitFigures Figures (const Complex& complex, int unit);
// The rules a plan must keep, from the graphs: every flat has an entrance and reaches a stair;
// every living room, bedroom and alcove has facade; every stack stair stands on every floor.
// Error-level findings (unit.no_entrance, unit.unreachable, room.no_daylight, core.broken_stack).
std::vector<Diagnostic> Check (const Complex& complex);
} // namespace geomsrv::archviz::buildingtopology
#endif
