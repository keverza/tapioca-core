#ifndef EVP_ARCHVIZ_FLOORSCHEMESKETCH_HPP
#define EVP_ARCHVIZ_FLOORSCHEMESKETCH_HPP

// A floor's skeleton as a drawing the virtual editor's kernel edits (Edit/EditDocument.hpp): each
// wing a closed rectangle, each corridor its centre line, each stair its rectangle. The pointer
// picks a corner, a side or a whole shape with the kernel's pick; a drag is the kernel's edit of
// that part (a side moved parallel to itself, an end run along its line, a shape moved) and snaps
// with the kernel's snap; and `Read` turns the drawing that results back into the design's pins
// through the floor plan's own edits, so every rule they keep is kept and every refusal is theirs.
// The drawing is never the source: it is made from a scheme and read once, per gesture.
// Pure C++: no ImGui, no Archicad.
#include "ArchViz/FloorSchemeEdit.hpp"
#include "Edit/EditSelection.hpp"
#include "Edit/EditSnap.hpp"

namespace geomsrv::archviz::floorscheme::sketch {
namespace ge = ::geomsrv::edit;

enum class What : uint8_t { Wing, Corridor, Stair };
struct Item {
    What what = What::Wing;
    int index = -1; // into the scheme's wings, corridors or cores
    ge::Id entity = ge::kNoId;
};
struct Sketch {
    ge::Document doc;
    std::vector<Item> items;
    const Item* Find (ge::Id entity) const;
    const Item* Find (What what, int index) const;
};
Sketch Of (const Scheme& s);

// The floor's own lines for the snap: its outline, and the slab's contour when it differs.
ge::SnapContext Context (const Scheme& s, const std::vector<Ring>& contour = {});

// The part `ref` of `base` taken `by` (world metres), in `doc` -- a copy of `base.doc`:
//   a wing     its side moves parallel to itself, its corner moves the two sides it joins, the
//              whole wing moves;
//   a corridor an end runs along its line (its length), a leg moves across its wing;
//   a stair    a side moves parallel to itself (its size), the whole stair moves.
// False with `error` for a part that does not move (a corridor's turn).
bool Drag (ge::Document& doc, const Sketch& base, const ge::Ref& ref, Vec by, std::string& error);

// The design the drawing `after` asks for, drawn from `before` (`Of (s)`): every shape that
// changed becomes the edit that makes it (MoveWing, SetWing, MoveStair, ResizeStair, MoveEnd,
// SetCorridor). Unchanged, with `why`, when the floor cannot take one of them.
edit::Design Read (const Scheme& s, edit::Design d, const Sketch& before, const ge::Document& after,
                   std::string* why = nullptr, const Options& o = {});

// How the floor is entered, as a graph over the plan: a node for each stair, each corridor leg and
// each flat, and an edge from each flat through its door to the circulation it opens onto, and
// from each stair to the corridor it stands on. And inside each flat whose rooms are laid (user,
// 2026-11-15: "graph editor should show also unit rooms"): a node for each room, an edge from the
// entrance door to the hall, and from each room to the room it is entered from -- the hall, else the
// nearest room to the hall it shares a wall with.
struct Graph {
    enum class Kind : uint8_t { Stair, Corridor, Flat, Room };
    struct Node {
        Kind kind = Kind::Flat;
        int index = -1; // a stair, a corridor, a flat; a room's flat
        Vec at;
        int room = -1;                    // a room: its index in the flat's roomList
        RoomKind what = RoomKind::Living; // a room's kind
    };
    struct Edge {
        std::vector<Vec> line; // from a node, through a door where there is one, to circulation
        // Access: a flat to circulation, its second point the door when the line has three.
        // Inside: within flat `flat`, door to hall or room to room.
        enum class Kind : uint8_t { Circulation, Access, Inside } kind = Kind::Circulation;
        int flat = -1;
    };
    std::vector<Node> nodes;
    std::vector<Edge> edges;
};
Graph Access (const Scheme& s);
} // namespace geomsrv::archviz::floorscheme::sketch
#endif
