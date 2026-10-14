#include "Edit/EditPath.hpp"
#include <algorithm>
#include <cmath>

namespace geomsrv::edit {

bool ValidatePath (const Path& path, AlignmentGeometry& geometry, std::string& error)
{
    if (!BuildAlignment (path.points, geometry, error))
        return false;
    if (!ValidateProfile (path.profile, geometry.length, error))
        return false;
    return ValidateSections (path.sections, geometry.length, error);
}

bool Evaluate (const Path& path, const AlignmentGeometry& g, double station, StationFrame& out)
{
    Placement at;
    double z = 0.0, grade = 0.0;
    const Section* section = SectionAt (path.sections, station);
    if (section == nullptr || !Locate (g, station, at) || !Elevation (path.profile, station, z, &grade))
        return false;
    out.station = station;
    out.centre = { at.point.x, at.point.y, z };
    out.tangent = at.tangent;
    out.left = at.left;
    out.curvature = at.curvature;
    out.grade = grade;
    out.section = *section;
    double dropLeft = 0.0, dropRight = 0.0;
    EdgeDrops (*section, dropLeft, dropRight);
    out.leftEdge = out.centre + at.left * section->widthLeft + Vec3 { 0.0, 0.0, dropLeft };
    out.rightEdge = out.centre - at.left * section->widthRight + Vec3 { 0.0, 0.0, dropRight };
    return true;
}

bool Outline (const Path& path, double maxStep, PathOutline& out, std::string& error)
{
    out = {};
    AlignmentGeometry g;
    if (!ValidatePath (path, g, error))
        return false;
    std::vector<double> stations = SampleStations (path, g, maxStep);
    for (size_t i = 1; i < path.sections.size (); ++i)
        stations.push_back (std::max (0.0, path.sections[i].station - 1.0e-3));
    std::sort (stations.begin (), stations.end ());
    for (double s : stations) {
        StationFrame f;
        if (!Evaluate (path, g, s, f))
            continue;
        out.stations.push_back (s);
        out.centre.push_back (f.centre);
        out.left.push_back (f.leftEdge);
        out.right.push_back (f.rightEdge);
        out.range.push_back (f.section.id);
    }
    for (double s : g.pointStations) {
        StationFrame f;
        if (Evaluate (path, g, s, f))
            out.points.push_back (f.centre);
    }
    for (const auto& b : path.profile) {
        StationFrame f;
        if (Evaluate (path, g, b.station, f))
            out.breaks.push_back ({ f.centre.x, f.centre.y, b.z });
    }
    return true;
}

std::vector<double> SampleStations (const Path& path, const AlignmentGeometry& g, double maxStep)
{
    std::vector<double> s = ElementStations (g);
    for (const auto& b : path.profile) {
        s.push_back (b.station);
        if (b.curve > 0.0) {
            s.push_back (b.station - b.curve * 0.5);
            s.push_back (b.station + b.curve * 0.5);
        }
    }
    for (const auto& r : path.sections)
        s.push_back (r.station);
    s.push_back (0.0);
    s.push_back (g.length);
    for (auto& x : s)
        x = std::clamp (x, 0.0, g.length);
    std::sort (s.begin (), s.end ());
    s.erase (std::unique (s.begin (), s.end (), [] (double a, double b) { return b - a < 1.0e-6; }), s.end ());
    if (!(maxStep > 0.0))
        return s;
    std::vector<double> out;
    for (size_t i = 0; i + 1 < s.size (); ++i) {
        const int pieces = std::max (1, static_cast<int> (std::ceil ((s[i + 1] - s[i]) / maxStep - 1.0e-9)));
        for (int k = 0; k < pieces; ++k)
            out.push_back (s[i] + (s[i + 1] - s[i]) * k / pieces);
    }
    out.push_back (s.back ());
    return out;
}

} // namespace geomsrv::edit
