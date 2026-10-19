#include "Edit/EditProfileView.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace geomsrv::edit {

namespace {

double HorizontalScale (const ProfileView& v)
{
    return v.width / std::max (v.stationMax - v.stationMin, 1.0e-9);
}

double PointToSegment (double px, double py, double ax, double ay, double bx, double by)
{
    const double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    const double t = len2 > 0.0 ? std::clamp (((px - ax) * dx + (py - ay) * dy) / len2, 0.0, 1.0) : 0.0;
    return std::hypot (ax + dx * t - px, ay + dy * t - py);
}

} // namespace

ProfileView FitProfile (const Path& path, double left, double top, double width, double height, double margin,
                        const std::vector<std::pair<double, double>>& terrain, double maxExaggeration)
{
    ProfileView v;
    v.left = left + margin;
    v.top = top + margin;
    v.width = std::max (1.0, width - 2.0 * margin);
    v.height = std::max (1.0, height - 2.0 * margin);
    double lo = 1.0e300, hi = -1.0e300, end = 0.0;
    for (const auto& b : path.profile) {
        lo = std::min (lo, b.z);
        hi = std::max (hi, b.z);
        end = std::max (end, b.station);
    }
    for (const auto& [s, z] : terrain) {
        lo = std::min (lo, z);
        hi = std::max (hi, z);
    }
    if (!(hi >= lo)) {
        lo = 0.0;
        hi = 1.0;
    }
    v.stationMin = 0.0;
    v.stationMax = std::max (end, 1.0e-3);
    const double sx = HorizontalScale (v);
    // Independent linear vertical scaling must also compress steep profiles.
    const double range = std::max (hi - lo, 1.0e-3);
    v.exaggeration = std::min (v.height / range / sx, std::max (1.0, maxExaggeration));
    const double visible = v.height / (sx * v.exaggeration);
    const double mid = (lo + hi) * 0.5;
    v.zMin = mid - visible * 0.5;
    v.zMax = mid + visible * 0.5;
    return v;
}

void ProfileToScreen (const ProfileView& v, double station, double z, double& x, double& y)
{
    const double sx = HorizontalScale (v);
    x = v.left + (station - v.stationMin) * sx;
    y = v.top + (v.zMax - z) * sx * v.exaggeration;
}

void ProfileFromScreen (const ProfileView& v, double x, double y, double& station, double& z)
{
    const double sx = HorizontalScale (v);
    station = v.stationMin + (x - v.left) / sx;
    z = v.zMax - (y - v.top) / (sx * v.exaggeration);
}

std::vector<ProfileHit> PickProfile (const Path& path, const ProfileView& v, double x, double y, double radius)
{
    std::vector<ProfileHit> hits;
    const auto& b = path.profile;
    std::vector<std::pair<double, double>> s (b.size ());
    for (size_t i = 0; i < b.size (); ++i)
        ProfileToScreen (v, b[i].station, b[i].z, s[i].first, s[i].second);
    for (size_t i = 0; i < b.size (); ++i) {
        const double d = std::hypot (s[i].first - x, s[i].second - y);
        if (d <= radius)
            hits.push_back ({ ProfileHit::Kind::Break, i, d });
    }
    for (size_t i = 0; i + 1 < b.size (); ++i) {
        const double d = PointToSegment (x, y, s[i].first, s[i].second, s[i + 1].first, s[i + 1].second);
        if (d <= radius)
            hits.push_back ({ ProfileHit::Kind::Tangent, i, d });
    }
    std::stable_sort (hits.begin (), hits.end (), [] (const ProfileHit& a, const ProfileHit& c) {
        return a.kind != c.kind ? a.kind == ProfileHit::Kind::Break : a.distance < c.distance;
    });
    return hits;
}

void SnapBreak (const Path& path, size_t index, double& station, double& z, const ProfileSnap& snap, std::string& hint)
{
    hint.clear ();
    const auto& b = path.profile;
    if (index >= b.size ())
        return;
    if (snap.stationStep > 0.0 && index > 0 && index + 1 < b.size ())
        station = std::round (station / snap.stationStep) * snap.stationStep;
    // A round grade on either side wins over a round elevation.
    double best = snap.gradeTolerance;
    bool graded = false;
    for (int side : { -1, 1 }) {
        if ((side < 0 && index == 0) || (side > 0 && index + 1 >= b.size ()) || !(snap.gradeStep > 0.0))
            continue;
        const GradeBreak& other = b[side < 0 ? index - 1 : index + 1];
        const double run = side < 0 ? station - other.station : other.station - station;
        if (!(run > 1.0e-6))
            continue;
        const double percent = (side < 0 ? z - other.z : other.z - z) / run * 100.0;
        const double round = std::round (percent / snap.gradeStep) * snap.gradeStep;
        if (std::abs (round - percent) <= best) {
            best = std::abs (round - percent);
            z = side < 0 ? other.z + round / 100.0 * run : other.z - round / 100.0 * run;
            char text[32];
            std::snprintf (text, sizeof text, "%.1f %%", round);
            hint = text;
            graded = true;
        }
    }
    if (!graded && snap.zStep > 0.0)
        z = std::round (z / snap.zStep) * snap.zStep;
}

Operation DragBreak (Id path, Id gradeBreak, double station, double z)
{
    return [=] (Document& doc, std::string& error) {
        const Path* p = FindPath (doc, path);
        if (p == nullptr) {
            error = "the path no longer exists";
            return false;
        }
        const GradeBreak* b = nullptr;
        bool end = false;
        for (size_t i = 0; i < p->profile.size (); ++i)
            if (p->profile[i].id == gradeBreak) {
                b = &p->profile[i];
                end = i == 0 || i + 1 == p->profile.size ();
            }
        if (b == nullptr) {
            error = "the grade break no longer exists";
            return false;
        }
        const bool moveStation =
            !end && b->anchor == kNoId && b->crossing == kNoId && std::abs (station - b->station) > 1.0e-9;
        const bool moveZ = !b->fixed && std::abs (z - b->z) > 1.0e-12;
        if (moveStation && !MovePathBreak (doc, path, gradeBreak, station, error))
            return false;
        if (moveZ && !SetPathElevation (doc, path, gradeBreak, z, error))
            return false;
        if (!moveStation && !moveZ && b->fixed && std::abs (z - b->z) > 1.0e-12) {
            error = "that elevation is fixed";
            return false;
        }
        return true;
    };
}

Operation DragTangent (Id path, size_t index, double dz)
{
    return [=] (Document& doc, std::string& error) {
        return EditPath (
            doc, path,
            [&] (Path& p, std::string& why) {
                if (index + 1 >= p.profile.size ()) {
                    why = "there is no such stretch of the profile";
                    return false;
                }
                if (p.profile[index].fixed || p.profile[index + 1].fixed) {
                    why = "a fixed elevation holds this stretch; drag its free end instead";
                    return false;
                }
                p.profile[index].z += dz;
                p.profile[index + 1].z += dz;
                return true;
            },
            error);
    };
}

ProfileDrawing DrawProfile (const Path& path, double step, const std::vector<std::pair<double, double>>& terrain)
{
    ProfileDrawing d;
    d.terrain = terrain;
    const auto& b = path.profile;
    if (b.size () < 2)
        return d;
    const double end = b.back ().station;
    std::vector<double> stations;
    for (const auto& g : b) {
        stations.push_back (g.station);
        if (g.curve > 0.0)
            for (int k = 0; k <= 16; ++k)
                stations.push_back (g.station - g.curve * 0.5 + g.curve * k / 16.0);
    }
    if (step > 0.0)
        for (double s = 0.0; s < end; s += step)
            stations.push_back (s);
    std::sort (stations.begin (), stations.end ());
    stations.erase (
        std::unique (stations.begin (), stations.end (), [] (double x, double y) { return y - x < 1.0e-9; }),
        stations.end ());
    for (double s : stations) {
        double z = 0.0;
        if (Elevation (b, s, z))
            d.line.push_back ({ s, z });
    }
    for (const auto& g : b) {
        d.breaks.push_back ({ g.station, g.z, g.fixed, g.id });
        if (g.curve > 0.0)
            d.curves.push_back ({ g.station - g.curve * 0.5, g.station + g.curve * 0.5 });
    }
    for (size_t i = 0; i + 1 < b.size (); ++i)
        d.grades.push_back (
            { (b[i].station + b[i + 1].station) * 0.5, (b[i].z + b[i + 1].z) * 0.5, TangentGrade (b, i) * 100.0 });
    return d;
}

} // namespace geomsrv::edit
