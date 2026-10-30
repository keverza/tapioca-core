#include "Edit/EditOrganize.hpp"
#include <algorithm>
#include <map>
#include <set>

namespace geomsrv::edit {

namespace {
// An object's groups from its own outwards.
std::vector<Id> Chain (const Document& doc, Id group)
{
    std::vector<Id> chain;
    for (const Group* g = FindGroup (doc, group); g != nullptr && chain.size () <= doc.groups.size ();
         g = FindGroup (doc, g->parent))
        chain.push_back (g->id);
    return chain;
}

std::vector<Id> AllObjects (const Document& doc)
{
    std::vector<Id> out;
    for (const auto& p : doc.paths)
        out.push_back (p.id);
    for (const auto& e : doc.entities)
        out.push_back (e.id);
    return out;
}

template <class F> bool EachMember (Document& doc, const std::vector<Ref>& refs, F change, std::string& error)
{
    const auto objects = ObjectsOf (doc, refs);
    if (objects.empty ()) {
        error = "select the objects first";
        return false;
    }
    for (const Id id : objects)
        change (*MemberOf (doc, id));
    return true;
}
} // namespace

Membership* MemberOf (Document& doc, Id object)
{
    if (Entity* e = Find (doc, object))
        return &e->member;
    if (Path* p = FindPath (doc, object))
        return &p->member;
    return nullptr;
}

const Membership* MemberOf (const Document& doc, Id object)
{
    return MemberOf (const_cast<Document&> (doc), object);
}

Layer* FindLayer (Document& doc, Id layer)
{
    for (auto& l : doc.layers)
        if (l.id == layer && layer != kNoId)
            return &l;
    return nullptr;
}

const Layer* FindLayer (const Document& doc, Id layer)
{
    return FindLayer (const_cast<Document&> (doc), layer);
}

const Group* FindGroup (const Document& doc, Id group)
{
    for (const auto& g : doc.groups)
        if (g.id == group && group != kNoId)
            return &g;
    return nullptr;
}

bool Shown (const Document& doc, const Membership& m)
{
    const Layer* layer = FindLayer (doc, m.layer);
    return !m.hidden && (layer == nullptr || layer->visible);
}

bool Shown (const Document& doc, Id object)
{
    const Membership* m = MemberOf (doc, object);
    return m == nullptr || Shown (doc, *m);
}

bool Pickable (const Document& doc, const Membership& m)
{
    const Layer* layer = FindLayer (doc, m.layer);
    return Shown (doc, m) && !m.locked && (layer == nullptr || !layer->locked);
}

bool Pickable (const Document& doc, Id object)
{
    const Membership* m = MemberOf (doc, object);
    return m == nullptr || Pickable (doc, *m);
}

bool ValidateOrganization (const Document& doc, std::string& error)
{
    std::set<Id> layers, groups;
    for (const auto& l : doc.layers)
        if (l.id == kNoId || l.id >= doc.nextId || !layers.insert (l.id).second) {
            error = "invalid or duplicate layer id " + std::to_string (l.id);
            return false;
        }
    for (const auto& g : doc.groups)
        if (g.id == kNoId || g.id >= doc.nextId || !groups.insert (g.id).second || layers.count (g.id)) {
            error = "invalid or duplicate group id " + std::to_string (g.id);
            return false;
        }
    for (const auto& g : doc.groups) {
        if (g.parent != kNoId && !groups.count (g.parent)) {
            error = "group " + std::to_string (g.id) + " sits in a group that does not exist";
            return false;
        }
        if (Chain (doc, g.id).size () > doc.groups.size ()) {
            error = "group " + std::to_string (g.id) + " sits inside itself";
            return false;
        }
    }
    for (const Id id : AllObjects (doc)) {
        const Membership& m = *MemberOf (doc, id);
        if ((m.layer != kNoId && !layers.count (m.layer)) || (m.group != kNoId && !groups.count (m.group))) {
            error = "object " + std::to_string (id) + " is on a layer or in a group that does not exist";
            return false;
        }
    }
    return true;
}

std::vector<Id> ObjectsOf (const Document& doc, const std::vector<Ref>& refs)
{
    std::set<Id> wanted;
    for (const auto& r : refs)
        if (r.part != Part::LandscapeFace)
            wanted.insert (r.entity);
    std::vector<Id> out;
    for (const Id id : AllObjects (doc))
        if (wanted.count (id))
            out.push_back (id);
    return out;
}

std::vector<Ref> WholeRefs (const std::vector<Id>& objects)
{
    std::vector<Ref> out;
    for (const Id id : objects)
        out.push_back ({ id, Part::Entity, kNoId });
    return out;
}

Id AddLayer (Document& doc, const std::string& name)
{
    Layer l;
    l.id = NewId (doc);
    l.name = name;
    doc.layers.push_back (l);
    return l.id;
}

bool DeleteLayer (Document& doc, Id layer, std::string& error)
{
    if (FindLayer (doc, layer) == nullptr) {
        error = "the layer no longer exists";
        return false;
    }
    for (const Id id : AllObjects (doc))
        if (Membership* m = MemberOf (doc, id); m->layer == layer)
            m->layer = kNoId;
    std::erase_if (doc.layers, [layer] (const Layer& l) { return l.id == layer; });
    return true;
}

bool MoveToLayer (Document& doc, const std::vector<Ref>& refs, Id layer, std::string& error)
{
    if (layer != kNoId && FindLayer (doc, layer) == nullptr) {
        error = "the layer no longer exists";
        return false;
    }
    return EachMember (doc, refs, [layer] (Membership& m) { m.layer = layer; }, error);
}

Id OutermostGroup (const Document& doc, Id object)
{
    const Membership* m = MemberOf (doc, object);
    const auto chain = m != nullptr ? Chain (doc, m->group) : std::vector<Id> {};
    return chain.empty () ? kNoId : chain.back ();
}

std::vector<Id> GroupMembers (const Document& doc, Id group)
{
    std::vector<Id> out;
    for (const Id id : AllObjects (doc)) {
        const auto chain = Chain (doc, MemberOf (doc, id)->group);
        if (std::find (chain.begin (), chain.end (), group) != chain.end ())
            out.push_back (id);
    }
    return out;
}

Id GroupObjects (Document& doc, const std::vector<Ref>& refs, const std::string& name, std::string& error)
{
    std::vector<Id> loose;
    std::set<Id> groups;
    for (const Id id : ObjectsOf (doc, refs)) {
        const Id outer = OutermostGroup (doc, id);
        if (outer != kNoId)
            groups.insert (outer);
        else
            loose.push_back (id);
    }
    if (loose.size () + groups.size () < 2) {
        error = "select two or more objects or groups to group";
        return kNoId;
    }
    Group g;
    g.id = NewId (doc);
    g.name = name.empty () ? "Group " + std::to_string (doc.groups.size () + 1) : name;
    for (auto& other : doc.groups)
        if (groups.count (other.id))
            other.parent = g.id;
    for (const Id id : loose)
        MemberOf (doc, id)->group = g.id;
    doc.groups.push_back (g);
    return g.id;
}

bool Ungroup (Document& doc, const std::vector<Ref>& refs, std::string& error)
{
    std::set<Id> outer;
    for (const Id id : ObjectsOf (doc, refs))
        if (const Id g = OutermostGroup (doc, id); g != kNoId)
            outer.insert (g);
    if (outer.empty ()) {
        error = "nothing selected is grouped";
        return false;
    }
    for (const Id g : outer) {
        const Id parent = FindGroup (doc, g)->parent; // kNoId: an outermost group has none
        for (auto& other : doc.groups)
            if (other.parent == g)
                other.parent = parent;
        for (const Id id : AllObjects (doc))
            if (Membership* m = MemberOf (doc, id); m->group == g)
                m->group = parent;
    }
    std::erase_if (doc.groups, [&] (const Group& g) { return outer.count (g.id) != 0; });
    return true;
}

void PruneGroups (Document& doc)
{
    for (bool pruned = true; pruned;) {
        std::set<Id> used;
        for (const Id id : AllObjects (doc))
            used.insert (MemberOf (doc, id)->group);
        for (const auto& g : doc.groups)
            used.insert (g.parent);
        pruned = std::erase_if (doc.groups, [&] (const Group& g) { return !used.count (g.id); }) != 0;
    }
}

std::vector<Ref> WidenToGroups (const Document& doc, const std::vector<Ref>& refs, Id entered)
{
    std::vector<Ref> out;
    const auto add = [&] (const Ref& r) {
        if (std::find (out.begin (), out.end (), r) == out.end ())
            out.push_back (r);
    };
    for (const auto& r : refs) {
        const Membership* m = MemberOf (doc, r.entity);
        if (r.part != Part::Entity || m == nullptr || m->group == kNoId) {
            add (r);
            continue;
        }
        const auto chain = Chain (doc, m->group);
        const auto inside = std::find (chain.begin (), chain.end (), entered);
        Id group = chain.back ();
        if (entered != kNoId && inside != chain.end ()) {
            if (inside == chain.begin ()) {
                add (r); // directly in the entered group: the object itself
                continue;
            }
            group = *(inside - 1);
        }
        for (const Id id : GroupMembers (doc, group))
            if (Pickable (doc, id))
                add ({ id, Part::Entity, kNoId });
    }
    return out;
}

bool SetHidden (Document& doc, const std::vector<Ref>& refs, bool hidden, std::string& error)
{
    return EachMember (doc, refs, [hidden] (Membership& m) { m.hidden = hidden; }, error);
}

bool SetLocked (Document& doc, const std::vector<Ref>& refs, bool locked, std::string& error)
{
    return EachMember (doc, refs, [locked] (Membership& m) { m.locked = locked; }, error);
}

bool Isolate (Document& doc, const std::vector<Ref>& refs, std::string& error)
{
    const auto keep = ObjectsOf (doc, refs);
    if (keep.empty ()) {
        error = "select the objects to isolate";
        return false;
    }
    ShowIsolated (doc);
    for (const Id id : AllObjects (doc)) {
        Membership& m = *MemberOf (doc, id);
        if (std::find (keep.begin (), keep.end (), id) == keep.end () && !m.hidden) {
            m.hidden = true;
            doc.isolated.push_back (id);
        }
    }
    return true;
}

bool ShowIsolated (Document& doc)
{
    const bool any = !doc.isolated.empty ();
    for (const Id id : doc.isolated)
        if (Membership* m = MemberOf (doc, id))
            m->hidden = false;
    doc.isolated.clear ();
    return any;
}

bool ShowAll (Document& doc)
{
    bool any = false;
    for (const Id id : AllObjects (doc)) {
        Membership& m = *MemberOf (doc, id);
        any = any || m.hidden;
        m.hidden = false;
    }
    doc.isolated.clear ();
    return any;
}

bool UnlockAll (Document& doc)
{
    bool any = false;
    for (const Id id : AllObjects (doc)) {
        Membership& m = *MemberOf (doc, id);
        any = any || m.locked;
        m.locked = false;
    }
    return any;
}

std::vector<Ref> SelectAll (const Document& doc)
{
    std::vector<Id> out;
    for (const Id id : AllObjects (doc))
        if (Pickable (doc, id))
            out.push_back (id);
    return WholeRefs (out);
}

std::vector<Ref> Inverse (const Document& doc, const std::vector<Ref>& refs)
{
    const auto selected = ObjectsOf (doc, refs);
    std::vector<Id> out;
    for (const Id id : AllObjects (doc))
        if (Pickable (doc, id) && std::find (selected.begin (), selected.end (), id) == selected.end ())
            out.push_back (id);
    return WholeRefs (out);
}

namespace {
// 0 a path, 1 an open line, 2 a closed outline, 3 an extrusion.
int KindOf (const Document& doc, Id object)
{
    if (const Entity* e = Find (doc, object))
        return e->kind == Kind::Extrusion ? 3 : e->closed ? 2 : 1;
    return 0;
}

std::string RoleOf (const Document& doc, Id object)
{
    if (const Entity* e = Find (doc, object))
        return e->role;
    if (const Path* p = FindPath (doc, object))
        return p->role;
    return {};
}
} // namespace

std::vector<Ref> SelectLike (const Document& doc, const std::vector<Ref>& refs, Likeness likeness)
{
    const auto key = [&] (Id id) -> std::string {
        switch (likeness) {
            case Likeness::Role:
                return RoleOf (doc, id);
            case Likeness::Layer:
                return std::to_string (MemberOf (doc, id)->layer);
            case Likeness::Kind:
                return std::to_string (KindOf (doc, id));
        }
        return {};
    };
    std::set<std::string> wanted;
    for (const Id id : ObjectsOf (doc, refs))
        wanted.insert (key (id));
    std::vector<Id> out;
    for (const Id id : AllObjects (doc))
        if (Pickable (doc, id) && wanted.count (key (id)))
            out.push_back (id);
    return WholeRefs (out);
}

} // namespace geomsrv::edit
