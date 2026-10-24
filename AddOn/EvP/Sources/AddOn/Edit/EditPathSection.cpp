#include "Edit/EditPath.hpp"
#include <algorithm>
#include <cmath>

namespace geomsrv::edit {

namespace {

constexpr double kStationTolerance = 1.0e-6;
constexpr double kMaxSurfaceSlope = 100.0; // percent: steeper than 1:1 is a wall, not a surface

int IndexOf (const std::vector<Section>& sections, Id id)
{
    for (size_t i = 0; i < sections.size (); ++i)
        if (sections[i].id == id)
            return static_cast<int> (i);
    return -1;
}

bool Fine (const Section& s, std::string& error)
{
    if (!(s.widthLeft >= 0.0) || !(s.widthRight >= 0.0) || !(s.widthLeft + s.widthRight > 0.0) ||
        !std::isfinite (s.widthLeft + s.widthRight)) {
        error = "a path needs a width";
        return false;
    }
    if (!(std::abs (s.crossfall) < kMaxSurfaceSlope)) {
        error = "a crossfall must be below 100 %";
        return false;
    }
    if (!(s.shoulder >= 0.0) || !(s.cutSlope > 0.0) || !(s.fillSlope > 0.0) ||
        !std::isfinite (s.cutSlope + s.fillSlope + s.shoulder)) {
        error = "shoulders and grading slopes must be positive";
        return false;
    }
    if (!(s.rounding >= 0.0) || !std::isfinite (s.rounding)) {
        error = "a rounding cannot be negative";
        return false;
    }
    return true;
}

} // namespace

bool ValidateSections (const std::vector<Section>& sections, double length, std::string& error)
{
    if (sections.empty () || std::abs (sections.front ().station) > kStationTolerance) {
        error = "the first cross-section range starts at the path's start";
        return false;
    }
    for (size_t i = 0; i < sections.size (); ++i) {
        if (!Fine (sections[i], error))
            return false;
        if (i > 0 && !(sections[i].station - sections[i - 1].station > kStationTolerance)) {
            error = "cross-section ranges must be in station order and apart";
            return false;
        }
        if (i > 0 && !(sections[i].station < length - kStationTolerance)) {
            error = "a cross-section range starts past the path's end";
            return false;
        }
    }
    return true;
}

const Section* SectionAt (const std::vector<Section>& sections, double station)
{
    const Section* found = sections.empty () ? nullptr : &sections.front ();
    for (const auto& s : sections)
        if (s.station <= station + kStationTolerance)
            found = &s;
    return found;
}

void EdgeDrops (const Section& s, double& left, double& right)
{
    const double c = s.crossfall / 100.0;
    switch (s.fall) {
        case Fall::Crown:
            left = -c * s.widthLeft;
            right = -c * s.widthRight;
            break;
        case Fall::Left:
            left = -c * s.widthLeft;
            right = c * s.widthRight;
            break;
        case Fall::Right:
            left = c * s.widthLeft;
            right = -c * s.widthRight;
            break;
    }
}

bool SplitSection (std::vector<Section>& sections, double station, Id id, std::string& error)
{
    const Section* at = SectionAt (sections, station);
    if (at == nullptr || !(station > kStationTolerance)) {
        error = "a new range starts after the path's start";
        return false;
    }
    for (const auto& s : sections)
        if (std::abs (s.station - station) <= kStationTolerance) {
            error = "a range already starts there";
            return false;
        }
    Section next = *at;
    next.id = id;
    next.station = station;
    sections.insert (std::upper_bound (sections.begin (), sections.end (), station,
                                       [] (double s, const Section& x) { return s < x.station; }),
                     next);
    return true;
}

bool DeleteSection (std::vector<Section>& sections, Id id, std::string& error)
{
    const int i = IndexOf (sections, id);
    if (i <= 0) {
        error = i < 0 ? "the range no longer exists" : "the first range stays";
        return false;
    }
    sections.erase (sections.begin () + i);
    return true;
}

bool MoveSection (std::vector<Section>& sections, Id id, double station, std::string& error)
{
    const int i = IndexOf (sections, id);
    if (i <= 0) {
        error = i < 0 ? "the range no longer exists" : "the first range starts at the start";
        return false;
    }
    const size_t k = static_cast<size_t> (i);
    const double lo = sections[k - 1].station, hi = k + 1 < sections.size () ? sections[k + 1].station : 1.0e300;
    if (!(station > lo + kStationTolerance) || !(station < hi - kStationTolerance)) {
        error = "a range cannot pass its neighbours";
        return false;
    }
    sections[k].station = station;
    return true;
}

bool SetWidth (std::vector<Section>& sections, Id id, double left, double right, std::string& error)
{
    const int i = IndexOf (sections, id);
    if (i < 0) {
        error = "the range no longer exists";
        return false;
    }
    Section s = sections[static_cast<size_t> (i)];
    if (left >= 0.0)
        s.widthLeft = left;
    if (right >= 0.0)
        s.widthRight = right;
    if (!Fine (s, error))
        return false;
    sections[static_cast<size_t> (i)] = s;
    return true;
}

bool SetWidthEverywhere (std::vector<Section>& sections, double left, double right, std::string& error)
{
    for (const auto& s : sections)
        if (!SetWidth (sections, s.id, left, right, error))
            return false;
    return true;
}

bool SetWidthFromOffset (std::vector<Section>& sections, Id id, double offset, std::string& error)
{
    if (!std::isfinite (offset) || std::abs (offset) < 1.0e-6) {
        error = "drag the edge away from the centreline";
        return false;
    }
    return offset > 0.0 ? SetWidth (sections, id, offset, -1.0, error) : SetWidth (sections, id, -1.0, -offset, error);
}

bool SetCrossfall (std::vector<Section>& sections, Id id, double percent, Fall fall, std::string& error)
{
    const int i = IndexOf (sections, id);
    if (i < 0) {
        error = "the range no longer exists";
        return false;
    }
    Section s = sections[static_cast<size_t> (i)];
    s.crossfall = percent;
    s.fall = fall;
    if (!Fine (s, error))
        return false;
    sections[static_cast<size_t> (i)] = s;
    return true;
}

void RestationSections (std::vector<Section>& sections, double oldLength, double newLength)
{
    if (!(oldLength > 0.0) || !(newLength > 0.0))
        return;
    for (size_t i = 1; i < sections.size (); ++i)
        sections[i].station *= newLength / oldLength;
}

} // namespace geomsrv::edit
