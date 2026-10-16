#include "Edit/EditPath.hpp"
#include <algorithm>
#include <cmath>

namespace geomsrv::edit {

namespace {

constexpr double kStationTolerance = 1.0e-6;

int IndexOf (const std::vector<GradeBreak>& breaks, Id id)
{
    for (size_t i = 0; i < breaks.size (); ++i)
        if (breaks[i].id == id)
            return static_cast<int> (i);
    return -1;
}

bool CheckOrder (const std::vector<GradeBreak>& b, std::string& error)
{
    for (size_t i = 0; i + 1 < b.size (); ++i) {
        if (!(b[i + 1].station - b[i].station > kStationTolerance)) {
            error = "grade breaks must be in station order and apart";
            return false;
        }
        if (b[i].station + b[i].curve * 0.5 > b[i + 1].station - b[i + 1].curve * 0.5 + kStationTolerance) {
            error = "the vertical curves at stations " + std::to_string (b[i].station) + " and " +
                    std::to_string (b[i + 1].station) + " overlap";
            return false;
        }
    }
    return true;
}

} // namespace

bool ValidateProfile (const std::vector<GradeBreak>& b, double length, std::string& error)
{
    if (b.size () < 2) {
        error = "a profile needs a start and an end";
        return false;
    }
    if (std::abs (b.front ().station) > kStationTolerance ||
        std::abs (b.back ().station - length) > kStationTolerance) {
        error = "the profile must run from the path's start to its end";
        return false;
    }
    if (b.front ().curve != 0.0 || b.back ().curve != 0.0) {
        error = "the ends of a profile carry no vertical curve";
        return false;
    }
    for (const auto& g : b)
        if (!std::isfinite (g.z) || !std::isfinite (g.station) || !(g.curve >= 0.0)) {
            error = "a grade break is not finite";
            return false;
        }
    return CheckOrder (b, error);
}

double TangentGrade (const std::vector<GradeBreak>& b, size_t i)
{
    return (b[i + 1].z - b[i].z) / (b[i + 1].station - b[i].station);
}

bool Elevation (const std::vector<GradeBreak>& b, double s, double& z, double* grade)
{
    if (b.size () < 2 || !(s >= b.front ().station - kStationTolerance) ||
        !(s <= b.back ().station + kStationTolerance))
        return false;
    s = std::clamp (s, b.front ().station, b.back ().station);
    // A vertical curve replaces its break's corner over [station - L/2, station + L/2].
    for (size_t j = 1; j + 1 < b.size (); ++j) {
        const double half = b[j].curve * 0.5;
        if (half <= 0.0 || s < b[j].station - half || s > b[j].station + half)
            continue;
        const double g1 = TangentGrade (b, j - 1), g2 = TangentGrade (b, j), x = s - (b[j].station - half);
        z = b[j].z - g1 * half + g1 * x + (g2 - g1) / (2.0 * b[j].curve) * x * x;
        if (grade != nullptr)
            *grade = g1 + (g2 - g1) * x / b[j].curve;
        return true;
    }
    size_t i = 0;
    while (i + 2 < b.size () && s > b[i + 1].station)
        ++i;
    const double g = TangentGrade (b, i);
    z = b[i].z + g * (s - b[i].station);
    if (grade != nullptr)
        *grade = g;
    return true;
}

bool SetBreakElevation (std::vector<GradeBreak>& b, Id id, double z, std::string& error)
{
    const int i = IndexOf (b, id);
    if (i < 0 || !std::isfinite (z)) {
        error = i < 0 ? "the grade break no longer exists" : "not a number";
        return false;
    }
    if (b[static_cast<size_t> (i)].fixed) {
        error = "that elevation is fixed";
        return false;
    }
    b[static_cast<size_t> (i)].z = z;
    return true;
}

bool SetGrade (std::vector<GradeBreak>& b, size_t i, double percent, std::string& error)
{
    if (i + 1 >= b.size () || !std::isfinite (percent)) {
        error = "there is no such stretch of the profile";
        return false;
    }
    GradeBreak &from = b[i], &to = b[i + 1];
    const double rise = percent / 100.0 * (to.station - from.station);
    if (from.fixed && to.fixed) {
        error = "both ends of this stretch are fixed";
        return false;
    }
    // A fixed end stays; else a junction (shared with another path) does; else the start.
    const bool keepEnd = to.fixed || (!from.fixed && from.crossing == kNoId && to.crossing != kNoId);
    if (keepEnd)
        from.z = to.z - rise;
    else
        to.z = from.z + rise;
    return true;
}

bool InsertBreak (std::vector<GradeBreak>& b, double station, Id id, std::string& error)
{
    if (b.size () < 2 || !(station > b.front ().station + kStationTolerance) ||
        !(station < b.back ().station - kStationTolerance)) {
        error = "a new grade break goes between the ends";
        return false;
    }
    for (size_t j = 0; j < b.size (); ++j) {
        if (std::abs (b[j].station - station) <= kStationTolerance) {
            error = "there is already a grade break there";
            return false;
        }
        if (b[j].curve > 0.0 && std::abs (station - b[j].station) < b[j].curve * 0.5) {
            error = "that station lies inside a vertical curve";
            return false;
        }
    }
    size_t i = 0;
    while (b[i + 1].station < station)
        ++i;
    GradeBreak g;
    g.id = id;
    g.station = station;
    g.z = b[i].z + TangentGrade (b, i) * (station - b[i].station);
    b.insert (b.begin () + static_cast<std::ptrdiff_t> (i + 1), g);
    return true;
}

bool DeleteBreak (std::vector<GradeBreak>& b, Id id, std::string& error)
{
    const int i = IndexOf (b, id);
    if (i <= 0 || i + 1 >= static_cast<int> (b.size ())) {
        error = i < 0 ? "the grade break no longer exists" : "the ends of a profile stay";
        return false;
    }
    if (b[static_cast<size_t> (i)].fixed) {
        error = "a fixed elevation cannot be removed; unfix it first";
        return false;
    }
    if (b[static_cast<size_t> (i)].crossing != kNoId) {
        error = "paths that cross share a grade break there: it goes when they part";
        return false;
    }
    b.erase (b.begin () + i);
    return CheckOrder (b, error);
}

bool MoveBreak (std::vector<GradeBreak>& b, Id id, double station, std::string& error)
{
    const int i = IndexOf (b, id);
    if (i <= 0 || i + 1 >= static_cast<int> (b.size ())) {
        error = i < 0 ? "the grade break no longer exists" : "the ends of a profile stay at the ends";
        return false;
    }
    if (b[static_cast<size_t> (i)].anchor != kNoId) {
        error = "that grade break follows a control point";
        return false;
    }
    if (b[static_cast<size_t> (i)].crossing != kNoId) {
        error = "that grade break stays where the paths cross";
        return false;
    }
    b[static_cast<size_t> (i)].station = station;
    return CheckOrder (b, error);
}

bool SetCurve (std::vector<GradeBreak>& b, Id id, double length, std::string& error)
{
    const int i = IndexOf (b, id);
    if (i <= 0 || i + 1 >= static_cast<int> (b.size ()) || !(length >= 0.0)) {
        error = i < 0 ? "the grade break no longer exists" : "a vertical curve needs an interior break and a length";
        return false;
    }
    b[static_cast<size_t> (i)].curve = length;
    return CheckOrder (b, error);
}

double CurveForK (const std::vector<GradeBreak>& b, size_t i, double k)
{
    if (i == 0 || i + 1 >= b.size ())
        return 0.0;
    return k * std::abs (TangentGrade (b, i) - TangentGrade (b, i - 1)) * 100.0;
}

bool Restation (std::vector<GradeBreak>& b, double oldLength, double newLength,
                const std::vector<std::pair<Id, double>>& anchors, std::string& error)
{
    if (b.size () < 2 || !(oldLength > 0.0) || !(newLength > 0.0)) {
        error = "a path needs a length";
        return false;
    }
    const double scale = newLength / oldLength;
    for (size_t i = 1; i + 1 < b.size (); ++i) {
        b[i].station *= scale;
        for (const auto& [id, station] : anchors)
            if (b[i].anchor == id)
                b[i].station = station;
    }
    b.back ().station = newLength;
    if (!CheckOrder (b, error)) {
        error = "the plan change would reorder the profile: " + error;
        return false;
    }
    return true;
}

std::vector<GradeBreak> ProfileFromSamples (const std::vector<std::pair<double, double>>& samples, double tolerance,
                                            Id& nextId)
{
    std::vector<GradeBreak> out;
    if (samples.size () < 2)
        return out;
    std::vector<bool> keep (samples.size (), false);
    keep.front () = keep.back () = true;
    std::vector<std::pair<size_t, size_t>> stack { { 0, samples.size () - 1 } };
    while (!stack.empty ()) {
        const auto [a, c] = stack.back ();
        stack.pop_back ();
        double worst = -1.0;
        size_t at = a;
        const auto [s0, z0] = samples[a];
        const auto [s1, z1] = samples[c];
        for (size_t k = a + 1; k < c; ++k) {
            const double t = (samples[k].first - s0) / (s1 - s0);
            const double d = std::abs (samples[k].second - (z0 + (z1 - z0) * t));
            if (d > worst) {
                worst = d;
                at = k;
            }
        }
        if (worst > tolerance) {
            keep[at] = true;
            stack.push_back ({ a, at });
            stack.push_back ({ at, c });
        }
    }
    for (size_t k = 0; k < samples.size (); ++k)
        if (keep[k]) {
            GradeBreak g;
            g.id = nextId++;
            g.station = samples[k].first;
            g.z = samples[k].second;
            out.push_back (g);
        }
    return out;
}

} // namespace geomsrv::edit
