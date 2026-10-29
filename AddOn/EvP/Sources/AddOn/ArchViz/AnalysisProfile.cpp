#include "ArchViz/AnalysisProfile.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::analysisprofile {

namespace {

namespace meta = metadata;

constexpr double kMinGapMetres = 1.0e-3; // the closest two anchors may come

struct Point {
    double s = 0.0, z = 0.0;
};

// A handle of `a` toward a neighbour `span` away, scaled back to half the span.
Point Clamped (double handleStation, double handleZ, double span)
{
    const double limit = 0.5 * (std::max) (span, 0.0);
    const double scale = handleStation > limit && handleStation > 0.0 ? limit / handleStation : 1.0;
    return { handleStation * scale, handleZ * scale };
}

// The segment from anchor i to i + 1 as its four control points.
void Controls (const Profile& profile, size_t i, Point p[4])
{
    const Anchor& a = profile.anchors[i];
    const Anchor& b = profile.anchors[i + 1];
    const double span = b.station - a.station;
    const Point out = Clamped (a.handleStation, a.handleZ, span);
    const Point in = Clamped (b.handleStation, b.handleZ, span);
    p[0] = { a.station, a.z };
    p[1] = { a.station + out.s, a.z + out.z };
    p[2] = { b.station - in.s, b.z - in.z };
    p[3] = { b.station, b.z };
}

Point At (const Point p[4], double u)
{
    const double v = 1.0 - u;
    const double w0 = v * v * v, w1 = 3.0 * v * v * u, w2 = 3.0 * v * u * u, w3 = u * u * u;
    return { w0 * p[0].s + w1 * p[1].s + w2 * p[2].s + w3 * p[3].s,
             w0 * p[0].z + w1 * p[1].z + w2 * p[2].z + w3 * p[3].z };
}

Point Derivative (const Point p[4], double u)
{
    const double v = 1.0 - u;
    const double w0 = 3.0 * v * v, w1 = 6.0 * v * u, w2 = 3.0 * u * u;
    return { w0 * (p[1].s - p[0].s) + w1 * (p[2].s - p[1].s) + w2 * (p[3].s - p[2].s),
             w0 * (p[1].z - p[0].z) + w1 * (p[2].z - p[1].z) + w2 * (p[3].z - p[2].z) };
}

// The segment holding `station`, and the parameter along it there. The station
// only grows along a segment (see the header), so bisection finds it.
bool Locate (const Profile& profile, double station, size_t& segment, double& u)
{
    const auto& anchors = profile.anchors;
    if (anchors.size () < 2)
        return false;
    station = std::clamp (station, anchors.front ().station, anchors.back ().station);
    segment = 0;
    while (segment + 2 < anchors.size () && anchors[segment + 1].station < station)
        ++segment;
    Point p[4];
    Controls (profile, segment, p);
    double low = 0.0, high = 1.0;
    for (int step = 0; step < 52; ++step) {
        const double middle = 0.5 * (low + high);
        (At (p, middle).s < station ? low : high) = middle;
    }
    u = 0.5 * (low + high);
    return true;
}

bool Finite (const Anchor& a)
{
    return std::isfinite (a.station) && std::isfinite (a.z) && std::isfinite (a.handleStation) &&
           std::isfinite (a.handleZ) && a.handleStation >= 0.0;
}

bool Usable (const Profile& profile)
{
    if (profile.anchors.size () < 2)
        return false;
    for (size_t i = 0; i < profile.anchors.size (); ++i)
        if (!Finite (profile.anchors[i]) || (i > 0 && profile.anchors[i].station <= profile.anchors[i - 1].station))
            return false;
    return true;
}

} // namespace

Profile Flat (double length, double z)
{
    const double handle = (std::max) (length, 0.0) / 3.0;
    return { { { 0.0, z, handle, 0.0 }, { (std::max) (length, kMinGapMetres), z, handle, 0.0 } } };
}

void Fit (Profile& profile, double length)
{
    length = (std::max) (length, kMinGapMetres);
    if (!Usable (profile)) {
        double mean = 0.0;
        size_t count = 0;
        for (const auto& a : profile.anchors)
            if (std::isfinite (a.z)) {
                mean += a.z;
                ++count;
            }
        profile = Flat (length, count > 0 ? mean / double (count) : 0.0);
        return;
    }
    const double first = profile.anchors.front ().station;
    const double scale = length / (profile.anchors.back ().station - first);
    for (auto& a : profile.anchors) {
        a.station = (a.station - first) * scale;
        a.handleStation *= scale;
    }
    profile.anchors.front ().station = 0.0;
    profile.anchors.back ().station = length;
}

double ZAt (const Profile& profile, double station)
{
    size_t segment = 0;
    double u = 0.0;
    if (!Locate (profile, station, segment, u))
        return profile.anchors.empty () ? 0.0 : profile.anchors.front ().z;
    Point p[4];
    Controls (profile, segment, p);
    return At (p, u).z;
}

std::vector<std::pair<double, double>> Sample (const Profile& profile, size_t perSegment)
{
    std::vector<std::pair<double, double>> out;
    perSegment = (std::max) (perSegment, size_t (1));
    for (size_t i = 0; i + 1 < profile.anchors.size (); ++i) {
        Point p[4];
        Controls (profile, i, p);
        for (size_t k = i == 0 ? 0 : 1; k <= perSegment; ++k) {
            const Point at = At (p, double (k) / double (perSegment));
            out.push_back ({ at.s, at.z });
        }
    }
    return out;
}

size_t Insert (Profile& profile, double station)
{
    auto& anchors = profile.anchors;
    for (size_t i = 0; i < anchors.size (); ++i)
        if (std::abs (anchors[i].station - station) < kMinGapMetres)
            return i;
    size_t segment = 0;
    double u = 0.0;
    if (!Locate (profile, station, segment, u) || station <= anchors.front ().station ||
        station >= anchors.back ().station)
        return station <= anchors.front ().station ? 0 : anchors.size () - 1;
    Point p[4];
    Controls (profile, segment, p);
    const Point at = At (p, u);
    const Point d = Derivative (p, u);
    Anchor anchor;
    anchor.station = at.s;
    anchor.z = at.z;
    anchor.handleStation = (std::min) (at.s - anchors[segment].station, anchors[segment + 1].station - at.s) / 3.0;
    anchor.handleZ = d.s > 1.0e-12 ? d.z / d.s * anchor.handleStation : 0.0;
    anchors.insert (anchors.begin () + std::ptrdiff_t (segment + 1), anchor);
    return segment + 1;
}

bool Remove (Profile& profile, size_t index)
{
    if (index == 0 || index + 1 >= profile.anchors.size ())
        return false;
    profile.anchors.erase (profile.anchors.begin () + std::ptrdiff_t (index));
    return true;
}

void MoveAnchor (Profile& profile, size_t index, double station, double z)
{
    auto& anchors = profile.anchors;
    if (index >= anchors.size () || !std::isfinite (station) || !std::isfinite (z))
        return;
    anchors[index].z = z;
    if (index == 0 || index + 1 == anchors.size ())
        return; // the ends are the path's
    anchors[index].station =
        std::clamp (station, anchors[index - 1].station + kMinGapMetres, anchors[index + 1].station - kMinGapMetres);
}

void MoveHandle (Profile& profile, size_t index, double station, double z, bool out)
{
    if (index >= profile.anchors.size () || !std::isfinite (station) || !std::isfinite (z))
        return;
    Anchor& a = profile.anchors[index];
    const double sign = out ? 1.0 : -1.0;
    a.handleStation = (std::max) (0.0, sign * (station - a.station));
    a.handleZ = sign * (z - a.z);
}

std::pair<double, double> HandleTip (const Profile& profile, size_t index, bool out)
{
    const auto& anchors = profile.anchors;
    if (index >= anchors.size ())
        return { 0.0, 0.0 };
    const Anchor& a = anchors[index];
    const bool toNeighbour = out ? index + 1 < anchors.size () : index > 0;
    const double span =
        toNeighbour ? std::abs (anchors[out ? index + 1 : index - 1].station - a.station) : 2.0 * a.handleStation;
    const Point h = Clamped (a.handleStation, a.handleZ, span);
    const double sign = out ? 1.0 : -1.0;
    return { a.station + sign * h.s, a.z + sign * h.z };
}

meta::Property Encode (const Profile& profile)
{
    meta::Property property;
    property.key = kPropertyKey;
    property.generator = "Tapioca.AnalysisPath";
    property.value.type = meta::ValueType::List;
    property.value.elementType = meta::ValueType::Object;
    for (const auto& a : profile.anchors) {
        meta::Value row;
        row.type = meta::ValueType::Object;
        row.fields = { { "station", meta::Value::Number (a.station, meta::ValueType::Length) },
                       { "z", meta::Value::Number (a.z, meta::ValueType::Length) },
                       { "handleStation", meta::Value::Number (a.handleStation, meta::ValueType::Length) },
                       { "handleZ", meta::Value::Number (a.handleZ, meta::ValueType::Length) } };
        property.value.list.push_back (std::move (row));
    }
    return property;
}

bool Decode (const meta::Property& property, Profile& profile, std::string& error)
{
    Profile read;
    if (property.value.type != meta::ValueType::List) {
        error = std::string (kPropertyKey) + " is not a list of anchors";
        return false;
    }
    for (const auto& row : property.value.list) {
        const auto field = [&] (const char* name, double& out) {
            const auto it = row.fields.find (name);
            if (it == row.fields.end ())
                return false;
            out = it->second.AsNumber ();
            return true;
        };
        Anchor a;
        if (row.type != meta::ValueType::Object || !field ("station", a.station) || !field ("z", a.z) ||
            !field ("handleStation", a.handleStation) || !field ("handleZ", a.handleZ)) {
            error = std::string (kPropertyKey) + " has an anchor without station, z and handle";
            return false;
        }
        read.anchors.push_back (a);
    }
    if (!Usable (read)) {
        error = std::string (kPropertyKey) + " needs two or more finite anchors by increasing station";
        return false;
    }
    profile = std::move (read);
    return true;
}

} // namespace geomsrv::archviz::analysisprofile
