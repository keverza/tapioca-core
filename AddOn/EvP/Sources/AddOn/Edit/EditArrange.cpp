#include "Edit/EditArrange.hpp"
#include <algorithm>
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
        Normalize (doc, e);
        pasted.push_back ({ e.id, Part::Entity, kNoId });
        doc.entities.push_back (std::move (e));
    }
    return true;
}

} // namespace geomsrv::edit
