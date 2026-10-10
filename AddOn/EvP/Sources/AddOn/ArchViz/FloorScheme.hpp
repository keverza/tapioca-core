#ifndef EVP_ARCHVIZ_FLOORSCHEME_HPP
#define EVP_ARCHVIZ_FLOORSCHEME_HPP

// Typology floor scheme: one floor of the whole massing, simplified to a skeleton of
// wings, planned on that simplification and then moulded to the real outline.
//
//   T0 input       the union of every counted contour at one elevation (holes kept)
//   T1 skeleton    wings = inscribed oriented rectangles; axes meet at junctions; a name
//   T2 access      per wing: centre corridor, corridor on one side, or just a stair core
//   T3 circulation corridor runs (straight or one L, never branching), sections, cores
//   T4 bands       rectangles between corridor and facade, cut by cores and end caps
//   T5 division    flats from room widths (living, bedrooms, walls), programme counts
//   T6 rooms       living (in the corner of corner flats), bedrooms, hall, bath, storage
//   T7 mould       what the simplification left out joins a flat or stays unassigned
//
// Hard rules are checked, never weighed (`Check`). User edits are pins in world XY: the
// generator may move a pin by at most `kPinTolerance` to keep a hard rule, otherwise it
// keeps the pin and reports the conflict. Pure C++/Clipper2: no ImGui, no Archicad.
#include "ArchViz/FloorProgramme.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::archviz::floorscheme {
struct Vec {
    double x = 0, y = 0;
    bool operator== (const Vec&) const = default;
};
using Ring = std::vector<Vec>; // closed implicitly, counter-clockwise for areas

constexpr double kPinTolerance = 3.0;
// Room widths along the facade (STR 2.02.01 Table 9 minimums; preferred slightly larger).
constexpr double kLivingMin = 3.3, kBedroomMin = 2.6, kBathMin = 1.7, kWcMin = 1.5, kStorageMin = 1.0;
constexpr double kHallMin = 1.4, kInnerWall = 0.1, kPartyWall = 0.2;

// Rows: sections whose stair has a flat on each side and two behind (pinned; Auto weighs them
// with the other section plans).
enum class Access : uint8_t { Auto, Centre, OneSide, CoreOnly, Rows };
enum class RoomKind : uint8_t { Living, Bedroom, Alcove, Hall, Bath, Wc, Storage };

struct Options {
    double corridor = 1.8;
    double coreWidth = 4.5, coreDepth = 4.2; // along / across the corridor
    double stairWindow = 1.5;                // facade a stair needs
    double maxDeadEnd = 25.0;                // corridor from its stair
    double sectionArea = 500.0;              // floor per stair, outline area x grossFactor
    double sectionSlack = 10.0;              // tolerated above sectionArea (510 m2 is fine)
    double grossFactor = 0.78;               // Massing's gross coefficient
    double minWingDepth = 5.0, maxWingDepth = 22.0, minWingLength = 6.0;
    double centreDepth = 13.0; // a wing at least this deep gets a centre corridor, else `shallow`
    // Shallower wings: Auto plans them both as sections round stairs and with a corridor on one
    // side, and keeps the floor that scores better (net area less `stairCost` per stair).
    Access shallow = Access::Auto;
    double stairCost = 40.0; // m2 of flats a stair and its lift are worth when typologies compete
    double windowGap = 1.6;  // wall between the two windows of a 1.5R (living and alcove)
    double maxFlat = 90.0;   // net m2 above which a flat is divided (user, 2026-10-10: more 2R
                             // flats beat a few over 100 m2); a programme type may ask for more
    double minCap = 5.0, maxCap = 8.0;
    bool cullCorners = true;  // an L's outer corner facing north is notched out of the massing
                              // (user, 2026-10-10: a rational plan beats a deep dark corner flat)
    bool biteCorners = false; // an L's inner corner facing north is bitten out for a lit stair
                              // (user sketch: "possible place"; costs a flat, so asked for)
    double north = 0;         // project north, radians anticlockwise from world +y
    // Neighbouring buildings' floor at this elevation (world rings): a wall against it is a party
    // wall -- thick, blind, never a facade for a window (user, 2026-10-10).
    std::vector<Ring> party;
    double raster = 0.3; // skeleton search grid
};

// User guidance; every point is world XY.
struct Pins {
    struct Core {
        Vec centre;
        double width = 0, depth = 0; // 0: Options
        bool operator== (const Core&) const = default;
    };
    struct AccessAt {
        Vec at;
        Access access = Access::Auto;
        bool operator== (const AccessAt&) const = default;
    };
    struct Wall {
        Vec at; // a party wall between flats, on its band
        bool operator== (const Wall&) const = default;
    };
    struct Rooms {
        Vec at;
        double rooms = 2; // the flat under `at` gets this room count
        bool operator== (const Rooms&) const = default;
    };
    struct End {
        Vec at; // a corridor end moves here (corridor length)
        bool operator== (const End&) const = default;
    };
    struct Count {
        Vec at;
        int flats = 1; // flats in the band, or the span between pinned walls, under `at`
        bool operator== (const Count&) const = default;
    };
    std::vector<Core> cores;
    std::vector<AccessAt> access;
    std::vector<Wall> walls;
    std::vector<Rooms> rooms;
    std::vector<End> ends;
    std::vector<Count> counts;
    bool operator== (const Pins&) const = default;
};

struct Wing {
    Ring rect; // the inscribed rectangle
    Vec a, b;  // axis from end to end (centre line)
    double length = 0, depth = 0;
    int parent = -1;  // wing it grows out of
    char joint = 0;   // 'L' corner, 'T' middle, 'S' step (same direction), 0 root
    double angle = 0; // degrees between this axis and its parent's
    Access access = Access::Auto;
};
struct Corridor {
    std::vector<Vec> axis; // two points (straight) or three (one L turn)
    Ring shape;
    int section = -1;
};
struct Core {
    Ring shape;
    Vec centre;
    double width = 0, depth = 0;
    double window = 0; // facade length it touches
    int section = -1;
    bool pinned = false;
};
struct Room {
    Ring shape;
    RoomKind kind = RoomKind::Living;
    double width = 0, depth = 0;
};
struct Opening {
    Vec a, b;                         // on the room's wall
    RoomKind kind = RoomKind::Living; // the room it lights or enters
};
struct Flat {
    Ring shape;
    size_t type = 0;  // programme type
    double rooms = 0; // that type's room count
    double gross = 0, net = 0, frontage = 0, depth = 0;
    int band = -1; // -1 for an end cap
    int section = -1;
    Vec axis { 1, 0 };    // along its band: party walls run across it
    bool corner = false;  // facade on two sides
    bool cap = false;     // owns a corridor end
    bool through = false; // facades on opposite sides
    bool inRange = true;  // net area within the type's programme range
    bool manual = false;  // walls at an angle (a slanted end, a junction): rooms left to the user
    std::vector<Room> roomList;
    std::vector<Opening> windows; // one per living room, bedroom and alcove, on its facade
    Opening door;                 // entrance, from the corridor, landing or lobby into the hall
};
struct Band {
    Ring shape;
    Vec a, b; // cut line direction: flats are cut perpendicular to a->b
    double depth = 0;
    int section = -1;
    int flats = 0;
};
struct Piece {
    Ring shape;
    std::string reason;
};
struct Section {
    int run = -1;
    double gross = 0;
    char access = 'C'; // 'C' centre, 'O' one side, 'S' just a stair core
};
struct Diagnostic {
    enum Level : uint8_t { Info, Warning, Error } level = Info;
    std::string code, text;
    Vec at;
};
struct Scheme {
    std::vector<Ring> outline; // T0, outer rings counter-clockwise, holes clockwise
    std::string typology;      // T1, e.g. "L 90", "U", "O", "T", "+", "X 60"
    std::vector<Wing> wings;
    std::vector<Section> sections;
    std::vector<Corridor> corridors;
    std::vector<Ring> lobbies; // circulation at an L turn beside the core
    std::vector<Core> cores;
    std::vector<Band> bands;
    std::vector<Flat> flats;
    std::vector<Piece> unassigned;         // floor no flat, corridor or core takes, and why
    std::vector<Piece> culled;             // massing the typology leaves out: a change to suggest to the
                                           // massing (`outline` and `gross` are what remains)
    std::vector<std::array<Vec, 2>> party; // outline against a neighbouring building (Options::party)
    std::vector<Diagnostic> diagnostics;
    std::vector<int> targets, counts; // programme flat counts, wanted and made
    double gross = 0, net = 0, circulation = 0;
    bool ok = false; // no error diagnostic
};

// Frontage of a flat with `rooms` rooms on the facade, party wall included:
// living + (n - 1) x (wall + bedroom) + party wall; a half room is a sleeping alcove.
struct Frontage {
    double min = 0, pref = 0, max = 0;
};
Frontage RoomFrontage (double rooms);
// Width of a through flat (facades on both sides, rooms in two rows).
Frontage ThroughFrontage (double rooms);
// Net area of a band flat: gross minus a party wall across the band and the facade wall.
double NetArea (double frontage, double depth);
// Largest net area a flat may have: Options::maxFlat, or the programme's largest type if larger.
double MaxFlat (const floorprogramme::Programme& programme, const Options& options);
// Generate one floor. `outline` is any set of world rings (several buildings may touch or
// overlap: they are planned as one massing); `programme` is the whole massing's.
Scheme Generate (const std::vector<Ring>& outline, const floorprogramme::Programme& programme, const Pins& pins = {},
                 const Options& options = {});
// Every hard rule on a finished scheme; appends Error diagnostics and returns their count.
size_t Check (Scheme& scheme, const Options& options = {});
double Area (const Ring& ring); // signed, counter-clockwise positive
} // namespace geomsrv::archviz::floorscheme
#endif
