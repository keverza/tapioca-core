#include "Edit/EditPathEdit.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace geomsrv::edit {

namespace {

int PointIndex (const Path& p, Id id)
{
    for (size_t i = 0; i < p.points.size (); ++i)
        if (p.points[i].id == id)
            return static_cast<int> (i);
    return -1;
}

int BreakIndex (const Path& p, Id id)
{
    for (size_t i = 0; i < p.profile.size (); ++i)
        if (p.profile[i].id == id)
            return static_cast<int> (i);
    return -1;
}

double StationOfPoint (const Path& p, Id point)
{
    AlignmentGeometry g;
    std::string error;
    const int i = PointIndex (p, point);
    if (i < 0 || !BuildAlignment (p.points, g, error))
        return -1.0;
    return g.pointStations[static_cast<size_t> (i)];
}

} // namespace

Id AddPath (Document& doc, const std::vector<Vec3>& points, double width, const std::string& role)
{
    Path p;
    p.id = NewId (doc);
    p.role = role;
    for (const auto& q : points)
        p.points.push_back ({ NewId (doc), { q.x, q.y, 0.0 } });
    AlignmentGeometry g;
    std::string error;
    const double length = BuildAlignment (p.points, g, error) ? g.length : 0.0;
    GradeBreak start, end;
    start.id = NewId (doc);
    start.z = points.empty () ? 0.0 : points.front ().z;
    end.id = NewId (doc);
    end.station = length;
    end.z = points.empty () ? 0.0 : points.back ().z;
    p.profile = { start, end };
    Section s;
    s.id = NewId (doc);
    s.widthLeft = s.widthRight = width * 0.5;
    p.sections = { s };
    doc.paths.push_back (p);
    ++doc.revision;
    return p.id;
}

bool EditPath (Document& doc, Id id, const std::function<bool (Path&, std::string&)>& change, std::string& error)
{
    Path* found = FindPath (doc, id);
    if (found == nullptr) {
        error = "the path no longer exists";
        return false;
    }
    AlignmentGeometry before, after;
    if (!BuildAlignment (found->points, before, error))
        return false;
    Path p = *found;
    if (!change (p, error) || !BuildAlignment (p.points, after, error))
        return false;
    std::vector<std::pair<Id, double>> anchors;
    for (size_t i = 0; i < p.points.size (); ++i)
        anchors.push_back ({ p.points[i].id, after.pointStations[i] });
    // Anchors follow their points even when the length did not change.
    if (!Restation (p.profile, before.length, after.length, anchors, error))
        return false;
    RestationSections (p.sections, before.length, after.length);
    if (!ValidatePath (p, after, error))
        return false;
    *found = std::move (p);
    return true;
}

bool MovePathPoint (Document& doc, Id path, Id point, Vec3 to, std::string& error)
{
    return EditPath (
        doc, path,
        [&] (Path& p, std::string& why) {
            const int i = PointIndex (p, point);
            if (i < 0 || p.points[static_cast<size_t> (i)].lockXY) {
                why = i < 0 ? "the control point no longer exists" : "that control point is locked";
                return false;
            }
            p.points[static_cast<size_t> (i)].p = { to.x, to.y, 0.0 };
            return true;
        },
        error);
}

bool InsertPathPoint (Document& doc, Id path, double station, std::string& error, Id* created)
{
    const Id id = NewId (doc);
    const bool ok = EditPath (
        doc, path,
        [&] (Path& p, std::string& why) {
            AlignmentGeometry g;
            Placement at;
            if (!BuildAlignment (p.points, g, why) || !Locate (g, station, at) || !(station > 0.0) ||
                !(station < g.length)) {
                why = why.empty () ? "a control point goes inside the path" : why;
                return false;
            }
            size_t leg = 0;
            while (leg + 2 < p.points.size () && g.pointStations[leg + 1] <= station)
                ++leg;
            p.points.insert (p.points.begin () + static_cast<std::ptrdiff_t> (leg + 1), ControlPoint { id, at.point });
            return true;
        },
        error);
    if (ok && created != nullptr)
        *created = id;
    return ok;
}

bool DeletePathPoint (Document& doc, Id path, Id point, std::string& error)
{
    return EditPath (
        doc, path,
        [&] (Path& p, std::string& why) {
            const int i = PointIndex (p, point);
            if (i < 0 || p.points.size () <= 2) {
                why = i < 0 ? "the control point no longer exists" : "a path needs two control points";
                return false;
            }
            p.points.erase (p.points.begin () + i);
            for (auto& b : p.profile)
                if (b.anchor == point)
                    b.anchor = kNoId;
            return true;
        },
        error);
}

bool SetPathRadius (Document& doc, Id path, Id point, double radius, std::string& error)
{
    return EditPath (
        doc, path,
        [&] (Path& p, std::string& why) {
            const int i = PointIndex (p, point);
            if (i <= 0 || i + 1 >= static_cast<int> (p.points.size ())) {
                why = i < 0 ? "the control point no longer exists" : "the ends of a path have no corner";
                return false;
            }
            p.points[static_cast<size_t> (i)].radius = radius;
            return true;
        },
        error);
}

bool SetPathElevation (Document& doc, Id path, Id gradeBreak, double z, std::string& error)
{
    return EditPath (
        doc, path, [&] (Path& p, std::string& why) { return SetBreakElevation (p.profile, gradeBreak, z, why); },
        error);
}

bool SetPathGrade (Document& doc, Id path, double station, double percent, std::string& error)
{
    return EditPath (
        doc, path,
        [&] (Path& p, std::string& why) {
            size_t i = 0;
            while (i + 2 < p.profile.size () && p.profile[i + 1].station <= station)
                ++i;
            return SetGrade (p.profile, i, percent, why);
        },
        error);
}

bool InsertPathBreak (Document& doc, Id path, double station, std::string& error, Id* created)
{
    const Id id = NewId (doc);
    const bool ok = EditPath (
        doc, path, [&] (Path& p, std::string& why) { return InsertBreak (p.profile, station, id, why); }, error);
    if (ok && created != nullptr)
        *created = id;
    return ok;
}

bool DeletePathBreak (Document& doc, Id path, Id gradeBreak, std::string& error)
{
    return EditPath (
        doc, path, [&] (Path& p, std::string& why) { return DeleteBreak (p.profile, gradeBreak, why); }, error);
}

bool MovePathBreak (Document& doc, Id path, Id gradeBreak, double station, std::string& error)
{
    return EditPath (
        doc, path, [&] (Path& p, std::string& why) { return MoveBreak (p.profile, gradeBreak, station, why); }, error);
}

bool SetPathCurve (Document& doc, Id path, Id gradeBreak, double length, std::string& error)
{
    return EditPath (
        doc, path, [&] (Path& p, std::string& why) { return SetCurve (p.profile, gradeBreak, length, why); }, error);
}

bool FixElevationAt (Document& doc, Id path, double station, double z, const std::string& binding, std::string& error,
                     Id* created)
{
    const Id id = NewId (doc);
    Id used = kNoId;
    const bool ok = EditPath (
        doc, path,
        [&] (Path& p, std::string& why) {
            int i = -1;
            for (size_t k = 0; k < p.profile.size (); ++k)
                if (std::abs (p.profile[k].station - station) < 1.0e-6)
                    i = static_cast<int> (k);
            if (i < 0) {
                if (!InsertBreak (p.profile, station, id, why))
                    return false;
                i = BreakIndex (p, id);
            }
            GradeBreak& b = p.profile[static_cast<size_t> (i)];
            if (b.fixed && b.z != z) {
                why = "a different elevation is already fixed there";
                return false;
            }
            b.z = z;
            b.fixed = true;
            b.binding = binding;
            used = b.id;
            return true;
        },
        error);
    if (ok && created != nullptr)
        *created = used;
    return ok;
}

bool UnfixBreak (Document& doc, Id path, Id gradeBreak, std::string& error)
{
    return EditPath (
        doc, path,
        [&] (Path& p, std::string& why) {
            const int i = BreakIndex (p, gradeBreak);
            if (i < 0) {
                why = "the grade break no longer exists";
                return false;
            }
            p.profile[static_cast<size_t> (i)].fixed = false;
            p.profile[static_cast<size_t> (i)].binding.clear ();
            return true;
        },
        error);
}

bool AnchorBreak (Document& doc, Id path, Id gradeBreak, Id point, std::string& error)
{
    return EditPath (
        doc, path,
        [&] (Path& p, std::string& why) {
            const int i = BreakIndex (p, gradeBreak);
            const double station = StationOfPoint (p, point);
            if (i <= 0 || i + 1 >= static_cast<int> (p.profile.size ()) || station < 0.0) {
                why = "anchor an interior grade break to an existing control point";
                return false;
            }
            p.profile[static_cast<size_t> (i)].anchor = point;
            p.profile[static_cast<size_t> (i)].station = station;
            return true;
        },
        error);
}

bool ProfileFromSurface (Document& doc, Id path, const std::function<bool (double, double, double&)>& height,
                         double step, double tolerance, std::string& error)
{
    Id next = doc.nextId;
    const bool ok = EditPath (
        doc, path,
        [&] (Path& p, std::string& why) {
            AlignmentGeometry g;
            if (!height || !BuildAlignment (p.points, g, why))
                return false;
            // Pieces between fixed breaks are simplified separately so the fixed ones stay.
            std::vector<GradeBreak> fixed;
            for (const auto& b : p.profile)
                if (b.fixed)
                    fixed.push_back (b);
            std::vector<double> cuts { 0.0 };
            for (const auto& f : fixed)
                if (f.station > 0.0 && f.station < g.length)
                    cuts.push_back (f.station);
            cuts.push_back (g.length);
            const auto fixedAt = [&] (double s) -> const GradeBreak* {
                for (const auto& f : fixed)
                    if (std::abs (f.station - s) < 1.0e-6)
                        return &f;
                return nullptr;
            };
            std::vector<GradeBreak> out;
            for (size_t c = 0; c + 1 < cuts.size (); ++c) {
                std::vector<std::pair<double, double>> samples;
                const int pieces = std::max (1, static_cast<int> (std::ceil ((cuts[c + 1] - cuts[c]) / step)));
                for (int k = 0; k <= pieces; ++k) {
                    const double s = cuts[c] + (cuts[c + 1] - cuts[c]) * k / pieces;
                    Placement at;
                    double z = 0.0;
                    if (const GradeBreak* f = fixedAt (s))
                        z = f->z;
                    else if (!Locate (g, s, at) || !height (at.point.x, at.point.y, z)) {
                        why = "part of the path lies outside the surface";
                        return false;
                    }
                    samples.push_back ({ s, z });
                }
                auto piece = ProfileFromSamples (samples, tolerance, next);
                if (!out.empty ())
                    piece.erase (piece.begin ()); // shared with the previous piece's end
                out.insert (out.end (), piece.begin (), piece.end ());
            }
            for (auto& b : out)
                if (const GradeBreak* f = fixedAt (b.station))
                    b = *f;
            p.profile = out;
            return true;
        },
        error);
    if (ok)
        doc.nextId = std::max (doc.nextId, next);
    return ok;
}

bool SplitPathSection (Document& doc, Id path, double station, std::string& error, Id* created)
{
    const Id id = NewId (doc);
    const bool ok = EditPath (
        doc, path, [&] (Path& p, std::string& why) { return SplitSection (p.sections, station, id, why); }, error);
    if (ok && created != nullptr)
        *created = id;
    return ok;
}

bool DeletePathSection (Document& doc, Id path, Id range, std::string& error)
{
    return EditPath (
        doc, path, [&] (Path& p, std::string& why) { return DeleteSection (p.sections, range, why); }, error);
}

bool SetPathWidth (Document& doc, Id path, Id range, double left, double right, std::string& error)
{
    return EditPath (
        doc, path, [&] (Path& p, std::string& why) { return SetWidth (p.sections, range, left, right, why); }, error);
}

bool SetPathWidthFromPoint (Document& doc, Id path, Vec3 point, std::string& error)
{
    return EditPath (
        doc, path,
        [&] (Path& p, std::string& why) {
            AlignmentGeometry g;
            double station = 0.0, offset = 0.0;
            if (!BuildAlignment (p.points, g, why) || !StationOf (g, point, station, offset))
                return false;
            const Section* s = SectionAt (p.sections, station);
            return s != nullptr && SetWidthFromOffset (p.sections, s->id, offset, why);
        },
        error);
}

bool SetPathCrossfall (Document& doc, Id path, Id range, double percent, Fall fall, std::string& error)
{
    return EditPath (
        doc, path, [&] (Path& p, std::string& why) { return SetCrossfall (p.sections, range, percent, fall, why); },
        error);
}

bool TransformPathParts (Document& doc, const std::vector<Ref>& refs, Vec3 t, std::string& error)
{
    std::map<Id, std::vector<Ref>> byPath;
    for (const auto& r : refs)
        byPath[r.entity].push_back (r);
    for (const auto& [id, parts] : byPath) {
        const bool ok = EditPath (
            doc, id,
            [&] (Path& p, std::string& why) {
                for (const auto& r : parts) {
                    if (r.part == Part::Entity || r.part == Part::PathPoint) {
                        for (auto& c : p.points) {
                            if (r.part == Part::PathPoint && c.id != r.item)
                                continue;
                            if (c.lockXY && (t.x != 0.0 || t.y != 0.0)) {
                                why = "a control point is locked";
                                return false;
                            }
                            c.p = c.p + Vec3 { t.x, t.y, 0.0 };
                        }
                    }
                    if (t.z != 0.0 && (r.part == Part::Entity || r.part == Part::Break))
                        for (auto& b : p.profile) {
                            if (r.part == Part::Break && b.id != r.item)
                                continue;
                            if (b.fixed) {
                                if (r.part == Part::Entity)
                                    continue; // a fixed elevation holds while the rest lifts
                                why = "that elevation is fixed";
                                return false;
                            }
                            b.z += t.z;
                        }
                    if (r.part == Part::Range) {
                        why = "drag a range's edge to change its width";
                        return false;
                    }
                }
                return true;
            },
            error);
        if (!ok)
            return false;
    }
    return true;
}

bool MapPathParts (Document& doc, const std::vector<Ref>& refs, const std::function<Vec3 (Vec3)>& map,
                   double radiusFactor, std::string& error)
{
    std::map<Id, std::vector<Ref>> byPath;
    for (const auto& r : refs) {
        if (r.part != Part::Entity && r.part != Part::PathPoint) {
            error = "turn or scale the whole path or its control points; grade breaks and ranges sit along it";
            return false;
        }
        byPath[r.entity].push_back (r);
    }
    for (const auto& [id, parts] : byPath) {
        const bool ok = EditPath (
            doc, id,
            [&] (Path& p, std::string& why) {
                const bool whole =
                    std::any_of (parts.begin (), parts.end (), [] (const Ref& r) { return r.part == Part::Entity; });
                for (auto& c : p.points) {
                    if (!whole &&
                        std::none_of (parts.begin (), parts.end (), [&] (const Ref& r) { return r.item == c.id; }))
                        continue;
                    const Vec3 to = map (c.p);
                    if (!Finite (to)) {
                        why = "the transform is not finite";
                        return false;
                    }
                    if (c.lockXY && (std::abs (to.x - c.p.x) > 1.0e-12 || std::abs (to.y - c.p.y) > 1.0e-12)) {
                        why = "a control point is locked";
                        return false;
                    }
                    c.p = { to.x, to.y, c.p.z };
                    c.radius *= radiusFactor;
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
