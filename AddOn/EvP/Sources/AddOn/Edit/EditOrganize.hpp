#ifndef EVP_EDIT_EDITORGANIZE_HPP
#define EVP_EDIT_EDITORGANIZE_HPP

// The editor's own organisation of its objects: layers, nested groups, and hiding and locking.
// None of it touches geometry. A hidden object (or one on a hidden layer) is neither drawn,
// picked nor snapped to; a locked one (or one on a locked layer) is drawn and snapped to but
// cannot be picked. Whole objects are what these edits take: any selected part stands for its
// object, and landscape faces, derived from their sources, are left out.
#include "Edit/EditDocument.hpp"
#include <string>
#include <vector>

namespace geomsrv::edit {

Membership* MemberOf (Document& doc, Id object);
const Membership* MemberOf (const Document& doc, Id object);
// kNoId finds layer 0.
Layer* FindLayer (Document& doc, Id layer);
const Layer* FindLayer (const Document& doc, Id layer);
const Group* FindGroup (const Document& doc, Id group);
// Not hidden itself, and on a visible layer.
bool Shown (const Document& doc, const Membership& member);
bool Shown (const Document& doc, Id object);
// Shown, and neither locked itself nor on a locked layer.
bool Pickable (const Document& doc, const Membership& member);
bool Pickable (const Document& doc, Id object);
// Layer and group ids unique, every membership naming a layer and group that exist, and
// every group's parent chain ending without coming back to it.
bool ValidateOrganization (const Document& doc, std::string& error);

// The whole objects `refs` reach, each once, in document order (paths first).
std::vector<Id> ObjectsOf (const Document& doc, const std::vector<Ref>& refs);
std::vector<Ref> WholeRefs (const std::vector<Id>& objects);

Id AddLayer (Document& doc, const std::string& name);
// Its objects go to layer 0, which itself cannot be deleted.
bool DeleteLayer (Document& doc, Id layer, std::string& error);
// `layer` kNoId is layer 0.
bool MoveToLayer (Document& doc, const std::vector<Ref>& refs, Id layer, std::string& error);

// The outermost group an object belongs to, or kNoId.
Id OutermostGroup (const Document& doc, Id object);
// Every object in `group` or in a group inside it.
std::vector<Id> GroupMembers (const Document& doc, Id group);
// A new group of the objects `refs` reach. An object already grouped brings its outermost
// group along, nested inside the new one. At least two objects or groups are needed. Inside
// `within` (an entered group), the groups just inside it count as outermost, and a new group
// of what is all inside it sits in it.
Id GroupObjects (Document& doc, const std::vector<Ref>& refs, const std::string& name, std::string& error,
                 Id within = kNoId);
// Dissolves the outermost group (inside `within`) of each object `refs` reach: its members and
// the groups inside it move up one level.
bool Ungroup (Document& doc, const std::vector<Ref>& refs, std::string& error, Id within = kNoId);
// Drops groups left with no object and no group inside them.
void PruneGroups (Document& doc);
// Each whole-object reference widened to every object of its outermost group (unless that
// group is `entered`, or inside it, when the object's own group inside `entered` is used);
// part references stay as they are.
std::vector<Ref> WidenToGroups (const Document& doc, const std::vector<Ref>& refs, Id entered = kNoId);

bool SetHidden (Document& doc, const std::vector<Ref>& refs, bool hidden, std::string& error);
bool SetLocked (Document& doc, const std::vector<Ref>& refs, bool locked, std::string& error);
// Hides every other shown object and remembers them; Show isolated brings back what is
// remembered (and is still hidden). Isolating again first shows the earlier isolation.
bool Isolate (Document& doc, const std::vector<Ref>& refs, std::string& error);
bool ShowIsolated (Document& doc);
// Every object shown, or unlocked; layers keep their own state.
bool ShowAll (Document& doc);
bool UnlockAll (Document& doc);

// Selection queries over the pickable objects.
enum class Likeness : uint8_t { Role, Layer, Kind };
std::vector<Ref> SelectAll (const Document& doc);
std::vector<Ref> Inverse (const Document& doc, const std::vector<Ref>& refs);
// The pickable objects that share the role, layer or kind (path, open line, closed outline or
// extrusion) of any object `refs` reach.
std::vector<Ref> SelectLike (const Document& doc, const std::vector<Ref>& refs, Likeness likeness);

} // namespace geomsrv::edit
#endif
