#include "Edit/EditArrange.hpp"
#include "Edit/EditOrganize.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditPolyline.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace geomsrv::edit {

Clipboard Copy (const Document& doc, const std::vector<Ref>& refs)
{
    std::set<Id> wanted;
    for (const auto& r : refs)
        if (r.part != Part::LandscapeFace)
            wanted.insert (r.entity);
    Clipboard clip;
    for (const auto& e : doc.entities)
        if (wanted.count (e.id))
            clip.entities.push_back (e);
    for (const auto& p : doc.paths)
        if (wanted.count (p.id))
            clip.paths.push_back (p);
    // A group comes along when every object in it is copied; an object keeps the innermost
    // such group, and a group the nearest such one around it (groups nest, so it is its parent).
    std::set<Id> copied;
    for (const auto& e : clip.entities)
        copied.insert (e.id);
    for (const auto& p : clip.paths)
        copied.insert (p.id);
    std::map<Id, bool> whole;
    const auto isWhole = [&] (Id group) {
        if (group == kNoId)
            return false;
        if (const auto it = whole.find (group); it != whole.end ())
            return it->second;
        const auto members = GroupMembers (doc, group);
        return whole[group] = std::all_of (members.begin (), members.end (), [&] (Id id) { return copied.count (id); });
    };
    std::set<Id> kept;
    const auto regroup = [&] (Membership& m) {
        Id g = m.group;
        while (g != kNoId && !isWhole (g))
            g = FindGroup (doc, g)->parent;
        m.group = g;
        for (; isWhole (g); g = FindGroup (doc, g)->parent)
            kept.insert (g); // its parent holds more, or is whole too
    };
    for (auto& e : clip.entities)
        regroup (e.member);
    for (auto& p : clip.paths)
        regroup (p.member);
    for (Group g : doc.groups)
        if (kept.count (g.id)) {
            if (!kept.count (g.parent))
                g.parent = kNoId; // the group around it stays behind
            clip.groups.push_back (g);
        }
    return clip;
}

bool Centre (const Clipboard& clip, Vec3& centre)
{
    constexpr double inf = std::numeric_limits<double>::infinity ();
    double x0 = inf, y0 = inf, x1 = -inf, y1 = -inf, z = 0.0;
    bool any = false;
    const auto take = [&] (Vec3 p) {
        if (!any)
            z = p.z;
        any = true;
        x0 = std::min (x0, p.x), y0 = std::min (y0, p.y);
        x1 = std::max (x1, p.x), y1 = std::max (y1, p.y);
    };
    for (const auto& e : clip.entities)
        for (const auto& v : e.vertices)
            take (v.p);
    for (const auto& p : clip.paths)
        for (const auto& c : p.points)
            take (c.p);
    if (any)
        centre = { (x0 + x1) * 0.5, (y0 + y1) * 0.5, z };
    return any;
}

bool Paste (Document& doc, const Clipboard& clip, Vec3 offset, std::vector<Ref>& pasted, std::string& error)
{
    if (clip.Empty ()) {
        error = "nothing has been copied";
        return false;
    }
    pasted.clear ();
    std::map<Id, Id> groupIds;
    for (const auto& g : clip.groups)
        groupIds[g.id] = NewId (doc);
    for (Group g : clip.groups) {
        g.id = groupIds[g.id];
        g.parent = groupIds.count (g.parent) ? groupIds[g.parent] : kNoId;
        doc.groups.push_back (g);
    }
    const auto place = [&] (Membership& m) {
        m.group = groupIds.count (m.group) ? groupIds[m.group] : kNoId;
        if (FindLayer (doc, m.layer) == nullptr)
            m.layer = kNoId;
        m.hidden = m.locked = false;
    };
    // Paths first, so that one pasted path's references to another find its new id.
    std::map<Id, Id> pathIds;
    for (const auto& p : clip.paths)
        pathIds[p.id] = NewId (doc);
    const auto newPath = [&] (Id old) {
        const auto it = pathIds.find (old);
        return it != pathIds.end () ? it->second : kNoId;
    };
    for (Path p : clip.paths) {
        p.id = pathIds[p.id];
        std::map<Id, Id> pointIds;
        for (auto& c : p.points) {
            pointIds[c.id] = NewId (doc);
            c.id = pointIds[c.id];
            c.p = { c.p.x + offset.x, c.p.y + offset.y, c.p.z };
        }
        const auto newPoint = [&] (Id old) {
            const auto it = pointIds.find (old);
            return it != pointIds.end () ? it->second : kNoId;
        };
        for (auto& b : p.profile) {
            b.id = NewId (doc);
            b.anchor = newPoint (b.anchor);
            b.crossing = newPath (b.crossing);
            b.binding.clear (); // the host item it was read from is not under the copy
        }
        for (auto& s : p.sections)
            s.id = NewId (doc);
        std::vector<KerbCorner> kerbs;
        for (auto k : p.kerbs) {
            if (k.other != kNoId && (k.other = newPath (k.other)) == kNoId)
                continue; // that junction is not among the pasted paths
            if (k.point != kNoId && (k.point = newPoint (k.point)) == kNoId)
                continue;
            kerbs.push_back (k);
        }
        p.kerbs = std::move (kerbs);
        place (p.member);
        pasted.push_back ({ p.id, Part::Entity, kNoId });
        doc.paths.push_back (std::move (p));
    }
    for (Entity e : clip.entities) {
        e.id = kNoId;
        for (auto& v : e.vertices) {
            v.id = kNoId;
            v.p = v.p + offset;
        }
        for (auto& s : e.segments)
            s.id = kNoId;
        place (e.member);
        Normalize (doc, e);
        pasted.push_back ({ e.id, Part::Entity, kNoId });
        doc.entities.push_back (std::move (e));
    }
    return true;
}

namespace {
KerbSide Opposite (KerbSide side)
{
    return side == KerbSide::Left ? KerbSide::Right : side == KerbSide::Right ? KerbSide::Left : side;
}
} // namespace

bool Mirror (Document& doc, const std::vector<Ref>& refs, Vec3 point, Vec3 axis, std::string& error)
{
    Vec3 a;
    if (!Normalize ({ axis.x, axis.y, 0.0 }, a)) {
        error = "a mirror needs a direction in plan";
        return false;
    }
    // Plan offsets reflect about the axis; points about the axis through `point`.
    const auto offset = [a] (Vec3 d) {
        const double along = 2.0 * (d.x * a.x + d.y * a.y);
        return Vec3 { along * a.x - d.x, along * a.y - d.y, d.z };
    };
    const auto reflect = [&] (Vec3 p) {
        const Vec3 r = offset ({ p.x - point.x, p.y - point.y, 0.0 });
        return Vec3 { point.x + r.x, point.y + r.y, p.z };
    };
    const auto moves = [] (Vec3 from, Vec3 to) {
        return std::abs (to.x - from.x) > 1.0e-9 || std::abs (to.y - from.y) > 1.0e-9;
    };
    const Clipboard objects = Copy (doc, refs);
    if (objects.Empty ()) {
        error = "select the objects to mirror";
        return false;
    }
    std::set<Id> mirrored;
    for (const auto& p : objects.paths)
        mirrored.insert (p.id);
    for (const auto& found : objects.entities) {
        Entity& e = *Find (doc, found.id);
        for (auto& v : e.vertices) {
            const Vec3 to = reflect (v.p);
            if (v.lockXY && moves (v.p, to)) {
                error = "a locked vertex is off the mirror line";
                return false;
            }
            v.p = to;
            v.in = offset (v.in);
            v.out = offset (v.out);
        }
        if (e.closed)
            ReverseOrder (e);
    }
    for (const auto& found : objects.paths) {
        const bool ok = EditPath (
            doc, found.id,
            [&] (Path& p, std::string& why) {
                for (auto& c : p.points) {
                    const Vec3 to = reflect (c.p);
                    if (c.lockXY && moves (c.p, to)) {
                        why = "a locked control point is off the mirror line";
                        return false;
                    }
                    c.p = to;
                }
                for (auto& s : p.sections) {
                    std::swap (s.widthLeft, s.widthRight);
                    std::swap (s.endLeft, s.endRight);
                    s.fall = s.fall == Fall::Left ? Fall::Right : s.fall == Fall::Right ? Fall::Left : s.fall;
                }
                for (auto& k : p.kerbs) {
                    k.side = Opposite (k.side);
                    if (k.other != kNoId && mirrored.count (k.other))
                        k.otherSide = Opposite (k.otherSide);
                }
                return true;
            },
            error);
        if (!ok)
            return false;
    }
    return true;
}

} // namespace geomsrv::edit
