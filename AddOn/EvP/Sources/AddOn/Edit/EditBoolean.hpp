#ifndef EVP_EDIT_EDITBOOLEAN_HPP
#define EVP_EDIT_EDITBOOLEAN_HPP

// Booleans of closed entities: union, subtract, intersect and exclude, with holes and islands.
// SDK-free; plan geometry only.
//
// Input semantics. Each operand is read as its drawn outline (EditShape.hpp ShapeRings): every
// contour of a compound entity, even-odd, so its holes are holes and its islands solid. Curves and
// live corners are flattened to straight pieces of at most kBooleanMaxAngle radians of turn; the
// result is straight segments -- it never claims an analytic arc or curve. Coordinates are kept to
// 1e-6 m (Clipper2 decimal precision 6). Each operand's fill is resolved on its own before the
// operands meet, so two overlapping operands never cancel each other by parity.
//
// Operation semantics, operands in order A, B, C...:
//   Union      A + B + C           everything any of them covers;
//   Subtract   A - (B + C)         the first, less all the others;
//   Intersect  A * B * C           only what all of them cover;
//   Exclude    A ^ B ^ C           what an odd number of them cover.
//
// Output. Each separate piece of the result -- a top-level outline with every hole and island
// nested in it -- is one entity, compound when it has holes (EditDocument.hpp), contours wound by
// depth. The largest piece keeps the first operand's entity id, kind, height, role and
// membership; any further pieces are new entities with the same kind, height, role and
// membership. Every vertex, segment and contour of the result has a new id, and segment and
// vertex attributes (widths, tags, locks, radii, handles) do not survive: the outline is new.
// The other operands are removed. All of it happens only after a valid result is in hand; an
// empty result, or any refused input, changes nothing.
//
// Refused inputs: fewer than two operands, a repeated or missing one, an open entity, a hidden or
// locked one, operands of different kinds, operands not in one horizontal plane (an extrusion's base, every vertex of a
// polyline), and any entity whose landscape ownership forbids it (BooleanOwnershipRefusal).
//
// Landscape ownership (#158). A boolean is destructive drafting; the landscape's own domain
// booleans keep their dependencies. So the only landscape sources it takes are editor-owned
// Reference lines -- what the editor's default drawing role makes -- that are unlocked, not
// imported from Archicad and used by nothing else in the landscape (operation input, region
// descriptor, property line, entrance). Their records are reconciled in the same edit: a removed
// operand's source becomes a tombstone, further pieces become reference lines of the kept
// outline's style, and the result stays linework with no terrain contribution. Every other
// source role, and every imported, locked or used reference line, is refused with its reason.
#include "Edit/EditDocument.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::edit {

enum class BooleanKind : uint8_t { Union, Subtract, Intersect, Exclude };

constexpr double kBooleanMaxAngle = 0.05;         // radians a flattened curve piece may turn
constexpr double kBooleanPlaneTolerance = 1.0e-6; // metres of Z by which operands may differ

// One piece of a boolean result: its outline first, then every hole and island inside it.
using BooleanPiece = std::vector<std::vector<Vec3>>;

// The pure geometry: operands as even-odd ring sets, in order, at height `z`. The pieces come
// largest first; empty when the result is empty. False only when the inputs cannot be clipped
// (non-finite, out of range, too many points).
bool BooleanRings (const std::vector<std::vector<std::vector<Vec3>>>& operands, BooleanKind kind, double z,
                   std::vector<BooleanPiece>& pieces, std::string& error);

// The edit: refuses atomically (document unchanged) with a reason. `results` receives the
// result entities, the first operand's id first.
bool BooleanEntities (Document& doc, const std::vector<Id>& operands, BooleanKind kind, std::string& error,
                      std::vector<Id>* results = nullptr);

// Why the landscape forbids `entity` as a boolean operand; empty when it may take part (a plain
// entity, or an editor-owned reference line nothing else uses). Preview and commit both apply it.
std::string BooleanOwnershipRefusal (const Document& doc, Id entity);

// "union", "subtract", "intersect", "exclude".
const char* BooleanName (BooleanKind kind);

} // namespace geomsrv::edit
#endif
