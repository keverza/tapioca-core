#include "Edit/EditLandscapeEntrance.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditShape.hpp"
#include <algorithm>
#include <cmath>

namespace tapioca::geometry {
namespace {
namespace edit = geomsrv::edit;

const LandscapeSource* BuildingSource (const edit::Document& doc, Id source)
{
    if (!doc.landscape)
        return nullptr;
    for (const auto& s : doc.landscape->sources)
        if (s.id == source && !s.deleted && s.role == SourceRole::Building)
            return &s;
    return nullptr;
}

// The outline as it is drawn: every segment's sampled points, closed back to the first.
std::vector<Vec3> Outline (const edit::Document& doc, const LandscapeSource& source)
{
    std::vector<Vec3> points;
    const auto* entity = edit::Find (doc, source.entityId);
    if (!entity || !entity->closed)
        return points;
    for (size_t i = 0; i < entity->segments.size (); ++i) {
        const auto piece = edit::SegmentPoints (*entity, i);
        points.insert (points.end (), piece.begin () + (points.empty () ? 0 : 1), piece.end ());
    }
    return points;
}

// Plan distance along `outline` of the point on it nearest `p`.
double AlongOf (const std::vector<Vec3>& outline, Vec3 p)
{
    double best = INFINITY, along = 0.0, walked = 0.0;
    for (size_t i = 0; i + 1 < outline.size (); ++i) {
        const Vec3 a = outline[i], b = outline[i + 1];
        const double dx = b.x - a.x, dy = b.y - a.y, length = std::hypot (dx, dy);
        const double t =
            length > 0 ? std::clamp (((p.x - a.x) * dx + (p.y - a.y) * dy) / (length * length), 0.0, 1.0) : 0.0;
        const double d = std::hypot (a.x + dx * t - p.x, a.y + dy * t - p.y);
        if (d < best)
            best = d, along = walked + length * t;
        walked += length;
    }
    return along;
}

bool Commit (edit::Document& doc, edit::Document candidate, std::string& error)
{
    if (!ValidateLandscape (candidate, error))
        return false;
    candidate.revision = doc.revision + 1;
    doc = std::move (candidate);
    error.clear ();
    return true;
}
} // namespace

std::string EntranceBinding (Id entrance)
{
    return "entrance:" + std::to_string (entrance);
}

bool AddLandscapeEntrance (edit::Document& doc, Id source, Vec3 near, double z, const std::string& doorGuid,
                           Id& entrance, std::string& error)
{
    const auto* building = BuildingSource (doc, source);
    const auto outline = building ? Outline (doc, *building) : std::vector<Vec3> {};
    if (outline.size () < 3) {
        error = "an entrance belongs on a closed building outline";
        return false;
    }
    if (!edit::Finite (near) || !std::isfinite (z)) {
        error = "an entrance needs a finite place and threshold height";
        return false;
    }
    auto candidate = doc;
    LandscapeEntrance value;
    value.id = edit::NewId (candidate);
    value.source = source;
    value.along = AlongOf (outline, near);
    value.z = z;
    value.doorGuid = doorGuid;
    candidate.landscape->entrances.push_back (value);
    if (!Commit (doc, std::move (candidate), error))
        return false;
    entrance = value.id;
    return true;
}

bool RemoveLandscapeEntrance (edit::Document& doc, Id entrance, std::string& error)
{
    if (!doc.landscape) {
        error = "landscape session is not started";
        return false;
    }
    auto candidate = doc;
    auto& entrances = candidate.landscape->entrances;
    const auto found = std::find_if (entrances.begin (), entrances.end (),
                                     [entrance] (const LandscapeEntrance& e) { return e.id == entrance; });
    if (found == entrances.end ()) {
        error = "the entrance no longer exists";
        return false;
    }
    entrances.erase (found);
    return Commit (doc, std::move (candidate), error);
}

bool LandscapeEntrancePoint (const edit::Document& doc, const LandscapeEntrance& entrance, Vec3& at)
{
    const auto* building = BuildingSource (doc, entrance.source);
    const auto outline = building ? Outline (doc, *building) : std::vector<Vec3> {};
    if (outline.size () < 3)
        return false;
    double walked = 0.0, perimeter = 0.0;
    for (size_t i = 0; i + 1 < outline.size (); ++i)
        perimeter += edit::DistanceXY (outline[i], outline[i + 1]);
    // The outline may have shrunk since the entrance was placed: it stays on it.
    const double along = perimeter > 0 ? std::fmod (entrance.along, perimeter) : 0.0;
    for (size_t i = 0; i + 1 < outline.size (); ++i) {
        const double length = edit::DistanceXY (outline[i], outline[i + 1]);
        if (walked + length >= along || i + 2 == outline.size ()) {
            const double t = length > 0 ? std::clamp ((along - walked) / length, 0.0, 1.0) : 0.0;
            at = edit::Lerp (outline[i], outline[i + 1], t);
            at.z = entrance.z;
            return true;
        }
        walked += length;
    }
    return false;
}

const LandscapeEntrance* EntranceNear (const edit::Document& doc, Vec3 point, double reach)
{
    if (!doc.landscape)
        return nullptr;
    const LandscapeEntrance* best = nullptr;
    double nearest = reach;
    for (const auto& entrance : doc.landscape->entrances) {
        Vec3 at;
        if (LandscapeEntrancePoint (doc, entrance, at) && edit::DistanceXY (at, point) <= nearest) {
            nearest = edit::DistanceXY (at, point);
            best = &entrance;
        }
    }
    return best;
}

bool ReachLandscapeEntrance (edit::Document& doc, Id path, Id entrance, std::string& error)
{
    const LandscapeEntrance* found = nullptr;
    if (doc.landscape)
        for (const auto& e : doc.landscape->entrances)
            if (e.id == entrance)
                found = &e;
    Vec3 at;
    const auto* p = edit::FindPath (doc, path);
    if (!found || !p || p->points.size () < 2 || !LandscapeEntrancePoint (doc, *found, at)) {
        error = "choose a path and an entrance on a building outline";
        return false;
    }
    const bool start = edit::DistanceXY (p->points.front ().p, at) <= edit::DistanceXY (p->points.back ().p, at);
    const Id point = start ? p->points.front ().id : p->points.back ().id;
    const double z = found->z;
    const std::string binding = EntranceBinding (entrance);
    auto candidate = doc;
    edit::AlignmentGeometry g;
    if (!edit::MovePathPoint (candidate, path, point, { at.x, at.y, 0.0 }, error) ||
        !edit::BuildAlignment (edit::FindPath (candidate, path)->points, g, error) ||
        !edit::FixElevationAt (candidate, path, start ? 0.0 : g.length, z, binding, error))
        return false;
    return Commit (doc, std::move (candidate), error);
}

} // namespace tapioca::geometry
