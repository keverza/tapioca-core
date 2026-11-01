#include "Edit/EditPath.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

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

bool Wide (double left, double right)
{
    return left >= 0.0 && right >= 0.0 && left + right > 0.0 && std::isfinite (left + right);
}

bool PercentSlope (const tapioca::geometry::TerrainSlope& authored, tapioca::geometry::TerrainSlope& result,
                   std::string& error)
{
    auto percent = authored;
    // NoRust still validates legacy percent paths. All other conversion belongs
    // to the existing Rust slope service, never local trigonometry or ratios.
    using Unit = tapioca::geometry::TerrainSlope::Unit;
    if (percent.unit != Unit::Percent &&
        !tapioca::geometry::ConvertTerrainSlope (authored, Unit::Percent, 1, percent, error))
        return false;
    result = percent;
    return true;
}

bool ResolveCrossfall (const PathCrossfall& authored, double& percent, std::string& error)
{
    auto converted = authored.slope;
    if (converted.unit != tapioca::geometry::TerrainSlope::Unit::Percent &&
        !tapioca::geometry::ConvertTerrainSlope (converted, tapioca::geometry::TerrainSlope::Unit::Percent, 1,
                                                 converted, error))
        return false;
    if (converted.unit != tapioca::geometry::TerrainSlope::Unit::Percent || converted.b != 0 ||
        !(std::abs (converted.a) < kMaxSurfaceSlope)) {
        error = "a crossfall must be below 100 %";
        return false;
    }
    percent = authored.negative ? -converted.a : converted.a;
    return true;
}

bool Fine (const Section& s, std::string& error)
{
    if (!Wide (s.widthLeft, s.widthRight) || (s.taper && !Wide (s.endLeft, s.endRight))) {
        error = "a path needs a width";
        return false;
    }
    double crossfall = 0;
    if (!ResolveCrossfall (s.crossfall, crossfall, error))
        return false;
    tapioca::geometry::TerrainSlope cut, fill;
    // Preserve legacy positive finite percent policy (including its aggregate
    // overflow check), not the separate region fields' 1000% cap.
    if (!PercentSlope (s.cutSlope, cut, error) || !PercentSlope (s.fillSlope, fill, error))
        return false;
    if (!(s.shoulder >= 0.0) || !(cut.a > 0.0) || cut.b != 0 || !(fill.a > 0.0) || fill.b != 0 ||
        !std::isfinite (cut.a + fill.a + s.shoulder)) {
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

Section SectionIn (const PathPreparation& preparation, double length, double station)
{
    return preparation.ready ? SectionIn (preparation.sections, length, station) : Section {};
}

bool PreparedCrossfall (const PathPreparation& preparation, const Section& section, double& percent)
{
    if (!preparation.ready)
        return false;
    const auto found = std::find_if (preparation.sections.begin (), preparation.sections.end (),
                                     [&] (const Section& candidate) { return candidate.id == section.id; });
    if (found == preparation.sections.end ())
        return false;
    const auto index = static_cast<size_t> (found - preparation.sections.begin ());
    // SectionIn may interpolate taper widths; only authored crossfall intent
    // must match this range's resolved projection.
    if (found->crossfall != section.crossfall || found->fall != section.fall ||
        index >= preparation.crossfallPercent.size ())
        return false;
    percent = preparation.crossfallPercent[index];
    return true;
}

Section SectionIn (const std::vector<Section>& sections, double length, double station)
{
    const Section* at = SectionAt (sections, station);
    if (at == nullptr)
        return {};
    Section out = *at;
    if (!at->taper)
        return out;
    const size_t i = static_cast<size_t> (at - sections.data ());
    const double end = i + 1 < sections.size () ? sections[i + 1].station : length;
    const double span = end - at->station;
    const double t = span > kStationTolerance ? std::clamp ((station - at->station) / span, 0.0, 1.0) : 0.0;
    out.widthLeft = at->widthLeft + (at->endLeft - at->widthLeft) * t;
    out.widthRight = at->widthRight + (at->endRight - at->widthRight) * t;
    return out;
}

void EdgeDrops (const Section& s, double percent, double& left, double& right)
{
    const double c = percent / 100.0;
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

bool SplitSection (std::vector<Section>& sections, double length, double station, Id id, std::string& error)
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
    Section next = SectionIn (sections, length, station);
    next.id = id;
    next.station = station;
    if (at->taper) {
        Section& before = sections[static_cast<size_t> (at - sections.data ())];
        before.endLeft = next.widthLeft;
        before.endRight = next.widthRight;
    }
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
    if (!s.taper)
        s.endLeft = s.widthLeft, s.endRight = s.widthRight;
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

bool SetTaper (std::vector<Section>& sections, Id id, bool taper, double endLeft, double endRight, std::string& error)
{
    const int i = IndexOf (sections, id);
    if (i < 0) {
        error = "the range no longer exists";
        return false;
    }
    Section s = sections[static_cast<size_t> (i)];
    // A range that starts to taper does so from where it is: a side not given stays parallel.
    if (!taper || !s.taper)
        s.endLeft = s.widthLeft, s.endRight = s.widthRight;
    s.taper = taper;
    if (taper) {
        if (endLeft >= 0.0)
            s.endLeft = endLeft;
        if (endRight >= 0.0)
            s.endRight = endRight;
    }
    if (!Fine (s, error))
        return false;
    sections[static_cast<size_t> (i)] = s;
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

void EdgeDrops (const Section& section, double& left, double& right)
{
    // Invalid input remains invalid; this legacy helper has no error channel.
    // Document edits reject before this direct arithmetic is used.
    EdgeDrops (section, PathCrossfallPercent (section.crossfall), left, right);
}

bool PathCrossfallPercent (const PathCrossfall& crossfall, double& percent, std::string& error)
{
    return ResolveCrossfall (crossfall, percent, error);
}

double PathCrossfallPercent (const PathCrossfall& crossfall)
{
    double percent = std::numeric_limits<double>::quiet_NaN ();
    std::string error;
    ResolveCrossfall (crossfall, percent, error);
    return percent;
}

PathCrossfall PercentCrossfall (double percent)
{
    return { { tapioca::geometry::TerrainSlope::Unit::Percent, std::abs (percent), 0 }, std::signbit (percent) };
}

bool SetCrossfall (std::vector<Section>& sections, Id id, double percent, Fall fall, std::string& error)
{
    return SetCrossfall (sections, id, PercentCrossfall (percent), fall, error);
}

bool SetCrossfall (std::vector<Section>& sections, Id id, const PathCrossfall& crossfall, Fall fall, std::string& error)
{
    const int i = IndexOf (sections, id);
    if (i < 0) {
        error = "the range no longer exists";
        return false;
    }
    double percent = 0;
    if (!ResolveCrossfall (crossfall, percent, error))
        return false;
    Section s = sections[static_cast<size_t> (i)];
    s.crossfall = crossfall;
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
