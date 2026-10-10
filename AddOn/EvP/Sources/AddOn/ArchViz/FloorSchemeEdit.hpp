#ifndef EVP_ARCHVIZ_FLOORSCHEMEEDIT_HPP
#define EVP_ARCHVIZ_FLOORSCHEMEEDIT_HPP

// Editing one massing floor's typology scheme (user, 2026-10-10: "a limited drawing program
// strictly following the skeleton"). Every gesture becomes a Design: rectangles added to or cut
// from the floor's silhouette, and pins the generator honours (stairs, party walls, flat counts,
// room counts, corridor ends, access per wing). The scheme is always regenerated from the floor
// and the design, so every hard rule holds after every edit; undo and redo swap designs.
// Pure C++: no ImGui, no Archicad. The HUD runs `Run` off the UI thread.
#include "ArchViz/FloorScheme.hpp"
#include <array>
#include <optional>

namespace geomsrv::archviz::floorscheme::edit {
struct Design {
    std::vector<Ring> added, cut; // world rings drawn onto (or taken from a neighbour) and out of the silhouette
    Pins pins;
    Access shallow = Access::Auto; // fixed once a session has chosen, so edits do not flip it
    std::vector<Vec> locked;       // flats the user locked: their walls and rooms stay pinned
    bool operator== (const Design&) const = default;
};

// The silhouette planned: the floor, plus the added rectangles, less the cut ones.
std::vector<Ring> Outline (const std::vector<Ring>& floor, const Design& design);
Scheme Run (const std::vector<Ring>& floor, const floorprogramme::Programme& programme, const Design& design,
            const Options& options = {});
// --- a building among its neighbours (user, 2026-10-10: "show only the building, but behind the
// scenes the whole floor exists as context") -------------------------------------------------
// Each building's silhouette at one elevation, in the order given: its floor and what it added,
// less what it cut and what another building added over it. Where floors overlap, the earlier
// building keeps the overlap.
std::vector<std::vector<Ring>> Owned (const std::vector<std::vector<Ring>>& floors,
                                      const std::vector<const Design*>& designs);
// Plan a silhouette `Owned` gave (the design's rectangles are already in it); `party` is the
// other buildings' silhouettes at the elevation: walls against them are blind party walls.
Scheme RunOwned (const std::vector<Ring>& owned, const std::vector<Ring>& party,
                 const floorprogramme::Programme& programme, const Design& design, const Options& options = {});
// The strip a party wall sweeps when pushed `distance` along its outward normal (out of the
// building: > 0), clipped to `from` -- the neighbour's silhouette when pushed out, this
// building's when pulled in.
std::vector<Ring> PartyStrip (const std::array<Vec, 2>& wall, double distance, const std::vector<Ring>& from);
// `strip` goes to `gainer`'s silhouette and out of what `loser` had added there.
void Take (Design& gainer, Design& loser, const std::vector<Ring>& strip);

// The access the generator chose for shallow wings, to keep while editing.
Access Chosen (const Scheme& scheme, const Options& options = {});

struct Session {
    std::vector<Ring> floor; // the massing floor as built: every building at this elevation
    floorprogramme::Programme programme = floorprogramme::Default ();
    Options options;
    Design design;
    std::vector<Design> undo, redo;
    bool Commit (Design next); // false when nothing changed
    bool Undo ();
    bool Redo ();
    void Reset (); // drop every edit
};

// --- what is under the pointer ----------------------------------------------------------
int FlatAt (const Scheme& s, Vec p);
int CoreAt (const Scheme& s, Vec p);
int WingAt (const Scheme& s, Vec p);
// A party wall between two flats of one band: its two ends, and the direction it moves in.
struct Wall {
    int low = -1, high = -1; // the flats before and after it along `axis`
    Vec a, b, axis;
};
std::optional<Wall> WallAt (const Scheme& s, Vec p, double tolerance);
struct End {
    int corridor = -1;
    bool last = false; // the axis's last point, else its first
    Vec at;
};
std::optional<End> EndAt (const Scheme& s, Vec p, double tolerance);

// --- edits: each returns the next design ------------------------------------------------
// Stairs: a moved stair goes where it is dropped, the wing's others pinned where they are; adding
// or taking one away spaces the wing's stairs evenly again, an added one where it is dropped
// (never below one stair per 500 m2).
Design MoveStair (const Scheme& s, Design d, int core, Vec to);
Design AddStair (const Scheme& s, Design d, Vec at);
Design RemoveStair (const Scheme& s, Design d, int core);
// Flats of a band: its walls and flats are pinned as they are, then one wall moves, a flat is
// split in two at `at`, or a flat joins its smaller neighbour.
Design MoveWall (const Scheme& s, Design d, const Wall& wall, Vec to);
Design SplitFlat (const Scheme& s, Design d, int flat, Vec at);
Design RemoveFlat (const Scheme& s, Design d, int flat);
Design SetRooms (const Scheme& s, Design d, int flat, double rooms);
Design MoveEnd (Design d, const End& end, Vec to);
// Two flats change places: in one band with their widths, across bands (the corridor) their room
// counts.
Design SwapFlats (const Scheme& s, Design d, int a, int b);
// A flat of `rooms` put in at `at`: the band flat there gives it a preferred frontage at the end
// nearer `at`. `Insertion` is where its wall would go (across the band) and the flat it makes.
struct Insertion {
    int flat = -1;
    Vec a, b;    // the new party wall, across the band
    Ring region; // the new flat
};
std::optional<Insertion> Insert (const Scheme& s, Vec at, double rooms);
Design InsertFlat (const Scheme& s, Design d, Vec at, double rooms);
// The floor's flats made `total`: the widest flats split, the narrowest join; locked ones stay.
Design SetCount (const Scheme& s, Design d, int total);
Design SetLocked (const Scheme& s, Design d, int flat, bool locked);
bool Locked (const Scheme& s, const Design& d, int flat);
Design SetAccess (const Scheme& s, Design d, int wing, Access access);
Design AddRect (Design d, Ring rect);
Design CutRect (Design d, Ring rect);
// A rectangle from two corners in the frame of the wing under `a` (or world axes): the
// silhouette follows the skeleton; corners snap to `grid` metres.
Ring Rectangle (const Scheme& s, Vec a, Vec b, double grid = 0.3);

// --- discrete gestures (user, 2026-10-10: "discrete in an optimal step like 0.4 m, with a
// little snap to optimal unit sizes") ---------------------------------------------------------
constexpr double kStep = 0.4;
// A dragged wall's point: moved along its axis in steps, pulled within half a step to where the
// flat on either side has a programme type's middle net area.
Vec SnapWall (const Scheme& s, const Wall& wall, Vec from, Vec to, const floorprogramme::Programme& programme,
              double step = kStep);
// A move from `from` to `to` in steps along the frame of the wing at `from` (world axes outside).
Vec Stepped (const Scheme& s, Vec from, Vec to, double step = kStep);
} // namespace geomsrv::archviz::floorscheme::edit
#endif
