#ifndef EVP_EDIT_EDITROOF_HPP
#define EVP_EDIT_EDITROOF_HPP

// The roof drainage design as editor content: one authored record on the document, beside
// the entities it refers to. The footprint is an ordinary closed (possibly compound) entity,
// so the shared editor's vertex, segment and contour tools edit it; the roof record holds
// what those tools do not know: height controls, drains and the drainage cells between them.
//
// Every node and cell carries an id from the document's one id space. Ids survive edits that
// keep the item, so a lock, a drain move or an annotation offset follows the item, not its
// position or its index. Coordinates are world metres and Z is absolute, as everywhere in
// the document. Pure C++: the generator, meshing and Bake stay in their own modules and read
// this record through an adapter.
#include "Edit/EditTypes.hpp"
#include <string>
#include <vector>

namespace geomsrv::edit {

struct Document;

// The footprint entities' role. The roof record names the entities; the role marks them in lists.
inline constexpr char kRoofFootprintRole[] = "roof-footprint";

enum class RoofNodeKind : uint8_t { Corner, Drain };
// A height control. A corner bounds cells; a drain is the low point a cell's faces fall to.
// XY and Z locks are independent and are never changed by an operation that was not asked to.
struct RoofNode {
    Id id = kNoId;
    RoofNodeKind kind = RoofNodeKind::Corner;
    Vec3 p;
    bool lockXY = false, lockZ = false;
    bool operator== (const RoofNode&) const = default;
};
// A drainage cell: its rim corners counter-clockwise in plan and the drain its faces fall to.
// The rectangle preset makes four-corner cells aligned with the design frame.
struct RoofCell {
    Id id = kNoId;
    std::vector<Id> rim;
    Id drain = kNoId;
    bool operator== (const RoofCell&) const = default;
};
struct RoofDesign {
    // Closed entities with role kRoofFootprintRole: each an outer boundary with its openings.
    // Disconnected wings are separate entities, since one entity's contours nest in its first.
    std::vector<Id> footprints;
    // The layout frame: plan origin and bearing (radians) of the generator's U axis. Only an
    // explicit reorientation changes it; footprint edits keep it.
    Vec3 origin;
    double bearing = 0.0;
    double datumZ = 0.0;        // the lowest a drain may sit: the source's top, never edited here
    double ridgeZ = 0.0;        // the generator's default corner height
    double cap = 200.0;         // every rectangle envelope stays strictly below this area, m2
    double targetPercent = 2.0; // the generator's target grade, not a hard constraint
    double textHeight = 0.18;
    std::vector<RoofNode> nodes;
    std::vector<RoofCell> cells;
    bool operator== (const RoofDesign&) const = default;
};

const RoofNode* FindRoofNode (const RoofDesign& roof, Id node);
RoofNode* FindRoofNode (RoofDesign& roof, Id node);
const RoofCell* FindRoofCell (const RoofDesign& roof, Id cell);
// Structural checks only: every footprint is a distinct closed entity with the roof role, parameters
// are finite and in range, every cell names existing nodes of the right kinds (at least three
// distinct rim corners and one drain), every node is used by a cell, coordinates are finite
// and no drain sits below the datum. Geometric validity (coverage, planarity, falls) is the
// roof module's, not the document's. Ids are claimed in Validate with the rest of the document.
bool ValidateRoof (const Document& doc, std::string& error);

} // namespace geomsrv::edit
#endif
