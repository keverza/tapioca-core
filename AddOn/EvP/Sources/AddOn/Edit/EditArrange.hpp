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

// Whole objects held apart from any document, with the ids they had there.
struct Clipboard {
    std::vector<Entity> entities;
    std::vector<Path> paths;
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
// receives the new objects, whole.
bool Paste (Document& doc, const Clipboard& clip, Vec3 offset, std::vector<Ref>& pasted, std::string& error);

} // namespace geomsrv::edit
#endif
