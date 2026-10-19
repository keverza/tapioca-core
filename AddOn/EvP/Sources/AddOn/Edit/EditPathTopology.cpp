#include "Edit/EditPathEdit.hpp"
#include "Edit/EditPathCrossing.hpp"
#include <algorithm>
#include <cmath>

namespace geomsrv::edit {
namespace {
constexpr double kJoin = 1e-6;
bool Fail (std::string& error, const char* reason)
{
    error = reason;
    return false;
}
KerbSide Reversed (KerbSide side)
{
    switch (side) {
        case KerbSide::Left:
            return KerbSide::Right;
        case KerbSide::Right:
            return KerbSide::Left;
        case KerbSide::Start:
            return KerbSide::End;
        case KerbSide::End:
            return KerbSide::Start;
    }
    return side;
}
void Reverse (Path& path, double length)
{
    std::reverse (path.points.begin (), path.points.end ());
    std::reverse (path.profile.begin (), path.profile.end ());
    for (auto& b : path.profile)
        b.station = length - b.station;
    const auto sections = path.sections;
    path.sections.clear ();
    for (size_t i = sections.size (); i-- > 0;) {
        Section section = sections[i];
        section.station = length - (i + 1 < sections.size () ? sections[i + 1].station : length);
        std::swap (section.widthLeft, section.widthRight);
        section.fall = Flipped (section.fall);
        path.sections.push_back (section);
    }
    for (auto& k : path.kerbs)
        k.side = Reversed (k.side);
}
bool EndMoved (const Document& before, Id path, bool start, Vec3 at)
{
    const Path* old = FindPath (before, path);
    return old == nullptr || DistanceXY (start ? old->points.front ().p : old->points.back ().p, at) > kJoin;
}
} // namespace

bool JoinPathEnds (Document& doc, Id first, bool firstStart, Id second, bool secondStart, std::string& error)
{
    const Path *pa = FindPath (doc, first), *pb = FindPath (doc, second);
    if (!pa || !pb || first == second)
        return Fail (error, "join needs the ends of two different paths");
    Path a = *pa, b = *pb;
    AlignmentGeometry ga, gb;
    if (!ValidatePath (a, ga, error) || !ValidatePath (b, gb, error))
        return false;
    if (firstStart)
        Reverse (a, ga.length);
    if (!secondStart)
        Reverse (b, gb.length);
    // The surviving path retains its role/defaults. Different kerb defaults on
    // the appended run become explicit corner intent, rather than being lost.
    if (a.edgeRadius != b.edgeRadius)
        for (size_t i = 1; i < b.points.size (); ++i)
            for (auto side : { KerbSide::Left, KerbSide::Right }) {
                KerbCorner corner;
                corner.point = b.points[i].id;
                corner.side = side;
                corner.radius = b.edgeRadius;
                if (std::none_of (b.kerbs.begin (), b.kerbs.end (),
                                  [&] (const KerbCorner& k) { return k.Same (corner); }))
                    b.kerbs.push_back (corner);
            }
    if (DistanceXY (a.points.back ().p, b.points.front ().p) > kJoin ||
        std::abs (a.profile.back ().z - b.profile.front ().z) > kJoin)
        return Fail (error, "joined endpoints must have the same position and elevation");
    if (!a.profile.back ().binding.empty () && !b.profile.front ().binding.empty () &&
        a.profile.back ().binding != b.profile.front ().binding)
        return Fail (error, "the endpoint elevation bindings conflict");
    const Id removedPoint = b.points.front ().id, sharedPoint = a.points.back ().id;
    a.points.back ().lockXY = a.points.back ().lockXY || b.points.front ().lockXY;
    auto& seam = a.profile.back ();
    seam.fixed = seam.fixed || b.profile.front ().fixed;
    if (seam.binding.empty ())
        seam.binding = b.profile.front ().binding;
    seam.anchor = sharedPoint;
    seam.crossing = kNoId;
    a.points.insert (a.points.end (), b.points.begin () + 1, b.points.end ());
    for (size_t i = 1; i < b.profile.size (); ++i) {
        auto grade = b.profile[i];
        grade.station += ga.length;
        if (grade.anchor == removedPoint)
            grade.anchor = sharedPoint;
        a.profile.push_back (grade);
    }
    for (auto section : b.sections) {
        section.station += ga.length;
        a.sections.push_back (section);
    }
    std::vector<KerbCorner> corners;
    for (const auto* path : { &a, &b })
        for (auto k : path->kerbs) {
            if (k.point == removedPoint)
                k.point = sharedPoint;
            if (k.other == first || k.other == second) {
                k.point = sharedPoint;
                k.other = kNoId;
                k.otherSide = KerbSide::Left;
            }
            const auto same =
                std::find_if (corners.begin (), corners.end (), [&] (const KerbCorner& x) { return x.Same (k); });
            if (same == corners.end ())
                corners.push_back (k);
            else if (same->radius != k.radius)
                return Fail (error, "the joined corner has conflicting requested kerb radii");
        }
    a.kerbs = std::move (corners);
    for (auto& grade : a.profile)
        if (grade.crossing == first || grade.crossing == second)
            grade.crossing = kNoId;
    AlignmentGeometry joined;
    if (!ValidatePath (a, joined, error))
        return false;
    Document result = doc;
    for (auto& path : result.paths) {
        if (path.id == first || path.id == second)
            continue;
        for (auto& grade : path.profile)
            if (grade.crossing == second)
                grade.crossing = first;
        for (auto& k : path.kerbs) {
            if (k.other == first && firstStart)
                k.otherSide = Reversed (k.otherSide);
            if (k.other == second) {
                k.other = first;
                if (!secondStart)
                    k.otherSide = Reversed (k.otherSide);
            }
        }
    }
    *FindPath (result, first) = std::move (a);
    result.paths.erase (std::remove_if (result.paths.begin (), result.paths.end (),
                                        [=] (const Path& path) { return path.id == second; }),
                        result.paths.end ());
    // Junction intent is stored on the lower-id path. A merge can change that
    // owner; move the request as well as remapping its other-path reference.
    std::vector<std::pair<Id, KerbCorner>> transferred;
    for (auto& path : result.paths)
        path.kerbs.erase (std::remove_if (path.kerbs.begin (), path.kerbs.end (),
                                          [&] (KerbCorner k) {
                                              if (k.other == kNoId || k.other > path.id)
                                                  return false;
                                              const Id owner = k.other;
                                              k.other = path.id;
                                              std::swap (k.side, k.otherSide);
                                              transferred.push_back ({ owner, k });
                                              return true;
                                          }),
                          path.kerbs.end ());
    for (const auto& [owner, corner] : transferred) {
        auto* path = FindPath (result, owner);
        if (!path)
            return Fail (error, "a junction kerb owner no longer exists");
        const auto same = std::find_if (path->kerbs.begin (), path->kerbs.end (),
                                        [&] (const KerbCorner& k) { return k.Same (corner); });
        if (same == path->kerbs.end ())
            path->kerbs.push_back (corner);
        else if (same->radius != corner.radius)
            return Fail (error, "joining would combine conflicting junction kerb requests");
    }
    if (!Validate (result, error))
        return false;
    doc = std::move (result);
    return true;
}

bool JoinMovedPathEnds (Document& doc, const Document& before, std::string& error)
{
    Document result = doc;
    for (bool joined = true; joined;) {
        joined = false;
        for (size_t i = 0; i < result.paths.size () && !joined; ++i)
            for (size_t j = i + 1; j < result.paths.size () && !joined; ++j)
                for (bool startA : { false, true }) {
                    if (joined)
                        break;
                    for (bool startB : { true, false }) {
                        const auto& a = result.paths[i];
                        const auto& b = result.paths[j];
                        const Vec3 endA = startA ? a.points.front ().p : a.points.back ().p;
                        const Vec3 endB = startB ? b.points.front ().p : b.points.back ().p;
                        if (DistanceXY (endA, endB) > kJoin ||
                            (!EndMoved (before, a.id, startA, endA) && !EndMoved (before, b.id, startB, endB)))
                            continue;
                        size_t ends = 0;
                        for (const auto& path : result.paths)
                            for (const auto& p : { path.points.front ().p, path.points.back ().p })
                                ends += DistanceXY (p, endA) <= kJoin;
                        if (ends != 2)
                            continue; // a branching node is not an arbitrary pairing
                        if (!JoinPathEnds (result, a.id, startA, b.id, startB, error))
                            return false;
                        joined = true;
                        break;
                    }
                }
    }
    doc = std::move (result);
    return true;
}

bool SplitPath (Document& doc, Id id, double station, std::string& error, Id* created)
{
    const Path* source = FindPath (doc, id);
    AlignmentGeometry g;
    if (!source || !ValidatePath (*source, g, error))
        return Fail (error, "the path no longer exists or is invalid");
    if (!std::isfinite (station) || station <= kJoin || station >= g.length - kJoin)
        return Fail (error, "split at a point inside the path, not at an endpoint");
    for (const auto& e : g.elements)
        if (e.kind == ElementKind::Arc && station > e.start - kJoin && station < e.start + e.length + kJoin)
            return Fail (error, "split on a straight run or sharp control point, not a circular bend");
    for (const auto& grade : source->profile)
        if (grade.curve > 0 && std::abs (station - grade.station) < grade.curve * 0.5 + kJoin)
            return Fail (error, "split outside the vertical curve to preserve its profile");
    Document result = doc;
    Path a = *source, b = *source;
    b.id = NewId (result);
    Placement at;
    Locate (g, station, at);
    a.points.clear ();
    b.points.clear ();
    ControlPoint left { NewId (result), at.point }, right { NewId (result), at.point };
    for (size_t i = 0; i < source->points.size (); ++i) {
        if (g.pointStations[i] < station - kJoin)
            a.points.push_back (source->points[i]);
        else if (g.pointStations[i] > station + kJoin)
            b.points.push_back (source->points[i]);
        else {
            left = source->points[i];
            right.lockXY = left.lockXY;
        }
    }
    a.points.push_back (left);
    b.points.insert (b.points.begin (), right);
    double z = 0;
    Elevation (source->profile, station, z);
    GradeBreak seam { NewId (result), station, z };
    a.profile.clear ();
    b.profile.clear ();
    for (auto grade : source->profile) {
        if (std::abs (grade.station - station) <= kJoin)
            seam = grade;
        else if (grade.station < station)
            a.profile.push_back (grade);
        else {
            grade.station -= station;
            if (grade.anchor == left.id)
                grade.anchor = right.id;
            b.profile.push_back (grade);
        }
    }
    seam.station = station;
    seam.anchor = left.id;
    seam.crossing = kNoId;
    a.profile.push_back (seam);
    seam.id = NewId (result);
    seam.station = 0;
    seam.anchor = right.id;
    b.profile.insert (b.profile.begin (), seam);
    a.sections.clear ();
    b.sections.clear ();
    Section active = *SectionAt (source->sections, station);
    active.id = NewId (result);
    active.station = 0;
    b.sections.push_back (active);
    for (auto section : source->sections) {
        if (section.station < station - kJoin)
            a.sections.push_back (section);
        else if (section.station > station + kJoin) {
            section.station -= station;
            b.sections.push_back (section);
        }
    }
    a.kerbs.clear ();
    b.kerbs.clear ();
    for (auto k : source->kerbs) {
        if (k.other != kNoId)
            continue; // transferred by junction station below
        const bool onRight =
            std::any_of (b.points.begin (), b.points.end (), [&] (const ControlPoint& p) { return p.id == k.point; });
        if (k.point == left.id) {
            a.kerbs.push_back (k);
            k.point = right.id;
            b.kerbs.push_back (k);
            continue;
        }
        (onRight ? b.kerbs : a.kerbs).push_back (k);
    }
    AlignmentGeometry check;
    if (!ValidatePath (a, check, error) || !ValidatePath (b, check, error))
        return false;
    *FindPath (result, id) = std::move (a);
    const Id newId = b.id;
    result.paths.push_back (std::move (b));
    const auto junctions = FindJunctions (doc.paths);
    for (auto& path : result.paths)
        if (path.id != id && path.id != newId)
            path.kerbs.erase (std::remove_if (path.kerbs.begin (), path.kerbs.end (),
                                              [=] (const KerbCorner& k) { return k.other == id; }),
                              path.kerbs.end ());
    for (const auto& path : doc.paths)
        for (const auto& request : path.kerbs) {
            if (request.other == kNoId || (path.id != id && request.other != id))
                continue;
            const Id other = path.id == id ? request.other : path.id;
            bool firstHalf = false, secondHalf = false;
            for (const auto& j : junctions) {
                const Id ja = doc.paths[j.a].id, jb = doc.paths[j.b].id;
                if (!((ja == id && jb == other) || (jb == id && ja == other)))
                    continue;
                const double s = ja == id ? j.stationA : j.stationB;
                firstHalf |= s <= station + kJoin;
                secondHalf |= s >= station - kJoin;
            }
            if (!firstHalf && !secondHalf)
                firstHalf = true; // retain dormant intent
            for (const Id half : { id, newId }) {
                if (!(half == id ? firstHalf : secondHalf))
                    continue;
                KerbCorner k = request;
                Id owner = path.id == id ? half : path.id;
                k.other = path.id == id ? other : half;
                if (owner > k.other) {
                    std::swap (owner, k.other);
                    std::swap (k.side, k.otherSide);
                }
                FindPath (result, owner)->kerbs.push_back (k);
            }
        }
    if (!Validate (result, error))
        return false;
    doc = std::move (result);
    if (created)
        *created = newId;
    return true;
}
} // namespace geomsrv::edit
