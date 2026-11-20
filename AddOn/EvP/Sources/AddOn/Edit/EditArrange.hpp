#ifndef EVP_EDIT_EDITARRANGE_HPP
#define EVP_EDIT_EDITARRANGE_HPP

// Edits on whole objects: copying them out of the document and pasting them back. Any
// selected part (a vertex, a segment, a path's range) stands for its object. Landscape
// faces are derived from their sources, not objects, so they are left out; a pasted copy
// is a free object that no landscape source owns.
#include "Edit/EditDocument.hpp"
#include <string>
#include <vector>

namespace geomsrv::edit {

// Whole objects held apart from any document, with the ids they had there, and the groups
// copied whole with them: a group only partly copied is left behind.
struct Clipboard {
    std::vector<Entity> entities;
    std::vector<Path> paths;
    std::vector<Group> groups;
    bool Empty () const
    {
        return entities.empty () && paths.empty ();
    }
};

// The whole objects `refs` reach, each once, in document order.
Clipboard Copy (const Document& doc, const std::vector<Ref>& refs);
// The plan centre of the clipboard's extent: what a paste at a point puts there.
bool Centre (const Clipboard& clip, Vec3& centre);
// Adds the clipboard's objects moved by `offset`, with every id fresh. A path moves in plan
// only (its heights are its profile's) and keeps its own kerb corners; a junction corner or
// a crossing break comes along only when the other path is pasted with it. `pasted`
// receives the new objects, whole. Copies arrive shown and unlocked, on their layer if it
// still exists, in fresh copies of the groups that came with them.
bool Paste (Document& doc, const Clipboard& clip, Vec3 offset, std::vector<Ref>& pasted, std::string& error);

// Repeats the whole objects `refs` reach in a rectangular grid. Counts include the source cell;
// signed spacings are centre-to-centre metres and choose the grid directions, and an active axis
// needs nonzero spacing. `placed` receives source objects first, then row-major cells (X varies
// first), with paths before entities within each cell. One by one is a valid no-op. The operation
// is bounded to 10000 cells, 100000 object placements and one million fresh ids; all failures are
// atomic and leave `placed` unchanged. Copies have the same free-object semantics as Paste.
bool RectangularArray (Document& doc, const std::vector<Ref>& refs, size_t countX, size_t countY, double spacingX,
                       double spacingY, std::vector<Ref>& placed, std::string& error);

// #130: the angle between neighbouring items of a polar array of `count` (the source included)
// over the signed total `sweep` in radians. A full turn spreads the count evenly, so no copy lands
// back on the source at 360 degrees; a part turn puts the last copy at the sweep's end. Preview and
// commit both place copy k at k times this angle.
double PolarStep (size_t count, double sweep);
// `p` turned by `angle` radians (counter-clockwise) about the vertical through `centre`; its
// height stays.
Vec3 TurnInPlan (Vec3 p, Vec3 centre, double angle);

// Repeats the whole objects `refs` reach about the vertical through `centre` (its height is not
// used). `count` includes the source and `sweep` is the signed total angle, at most a full turn;
// copy k sits at k * PolarStep. With `rotate` each copy turns by its angle; without, it only moves,
// the plan centre of the selection's extent carried round the circle. A copy is a new object, so a
// vertex lock pins the source, not where its copies go. A path turns in plan and keeps its profile
// by station. `placed` receives source objects first, then each copy in angle order, with paths
// before entities within it. One item is a valid no-op. The bounds, atomicity and free-object
// semantics are RectangularArray's.
bool PolarArray (Document& doc, const std::vector<Ref>& refs, size_t count, Vec3 centre, double sweep, bool rotate,
                 std::vector<Ref>& placed, std::string& error);

// Reflects the whole objects `refs` reach across the vertical plane through `point` along
// the plan direction `axis`; heights stay. A closed outline keeps its winding (its order
// reverses with the reflection) and an open line its direction; Bezier handles and live
// corners reflect with their points. A path reflects in plan and its sides trade places:
// widths, falls and kerb corners. A locked vertex or control point off the axis refuses.
bool Mirror (Document& doc, const std::vector<Ref>& refs, Vec3 point, Vec3 axis, std::string& error);

} // namespace geomsrv::edit
#endif
