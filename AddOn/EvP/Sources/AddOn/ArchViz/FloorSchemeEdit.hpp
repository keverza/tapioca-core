#ifndef EVP_ARCHVIZ_FLOORSCHEMEEDIT_HPP
#define EVP_ARCHVIZ_FLOORSCHEMEEDIT_HPP

// Editing one massing floor's typology scheme (user, 2026-10-10: "a limited drawing program
// strictly following the skeleton"). Every gesture becomes a Design: rectangles added to or cut
// from the floor's silhouette, and pins the generator honours (stairs, party walls, flat counts,
// room counts, corridor ends, access per wing). The scheme is always regenerated from the floor
// and the design, so every hard rule holds after every edit; undo and redo swap designs.
// Pure C++: no ImGui, no Archicad. The HUD runs `Run` off the UI thread.
#include "ArchViz/FloorScheme.hpp"
#include <optional>

namespace geomsrv::archviz::floorscheme::edit {
struct Design {
    std::vector<Ring> added, cut; // world rectangles drawn onto and out of the silhouette
    Pins pins;
    Access shallow = Access::Auto; // fixed once a session has chosen, so edits do not flip it
    bool operator== (const Design&) const = default;
};

// The silhouette planned: the floor, plus the added rectangles, less the cut ones.
std::vector<Ring> Outline (const std::vector<Ring>& floor, const Design& design);
Scheme Run (const std::vector<Ring>& floor, const floorprogramme::Programme& programme, const Design& design,
            const Options& options = {});
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
Design SetAccess (const Scheme& s, Design d, int wing, Access access);
Design AddRect (Design d, Ring rect);
Design CutRect (Design d, Ring rect);
// A rectangle from two corners in the frame of the wing under `a` (or world axes): the
// silhouette follows the skeleton; corners snap to `grid` metres.
Ring Rectangle (const Scheme& s, Vec a, Vec b, double grid = 0.3);
} // namespace geomsrv::archviz::floorscheme::edit
#endif
