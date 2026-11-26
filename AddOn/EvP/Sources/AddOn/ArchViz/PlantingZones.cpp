#include "ArchViz/PlantingZones.hpp"

#include "ArchViz/PlantingHudModel.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace geomsrv::archviz::planting {

namespace {

double SignedArea (const std::vector<double>& ring)
{
    const size_t n = ring.size () / 2;
    double twice = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const size_t k = (i + 1) % n;
        twice += ring[2 * i] * ring[2 * k + 1] - ring[2 * k] * ring[2 * i + 1];
    }
    return twice / 2.0;
}

// Even-odd crossing test.
bool InRing (const std::vector<double>& ring, double x, double y)
{
    const size_t n = ring.size () / 2;
    bool inside = false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double xi = ring[2 * i], yi = ring[2 * i + 1];
        const double xj = ring[2 * j], yj = ring[2 * j + 1];
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi)
            inside = !inside;
    }
    return inside;
}

double SegmentDistance (double px, double py, double ax, double ay, double bx, double by)
{
    const double dx = bx - ax, dy = by - ay;
    const double len2 = dx * dx + dy * dy;
    double t = len2 > 0.0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0;
    t = std::clamp (t, 0.0, 1.0);
    return std::hypot (px - (ax + t * dx), py - (ay + t * dy));
}

} // namespace

double PlantingZone::PlanDistance (double x, double y) const
{
    if (Contains (x, y))
        return 0.0;
    double best = std::numeric_limits<double>::infinity ();
    for (const auto& ring : rings) {
        const size_t n = ring.size () / 2;
        if (n < 2)
            continue;
        for (size_t i = 0, j = n - 1; i < n; j = i++)
            best = std::min (best, SegmentDistance (x, y, ring[2 * j], ring[2 * j + 1], ring[2 * i], ring[2 * i + 1]));
    }
    return best;
}

void NoGoDistances (const std::vector<PlantingZone>& zones, const std::vector<double>& x, const std::vector<double>& y,
                    std::vector<float>& distance, std::vector<float>& allowance)
{
    distance.clear ();
    allowance.clear ();
    std::vector<const PlantingZone*> noGo;
    for (const auto& z : zones)
        if (z.role == ZoneRole::NoGo && !z.rings.empty () && z.rings[0].size () >= 6) {
            noGo.push_back (&z);
            allowance.push_back (z.allowedHeight);
        }
    if (noGo.empty ())
        return;
    distance.reserve (x.size () * noGo.size ());
    for (size_t s = 0; s < x.size (); ++s)
        for (const PlantingZone* z : noGo)
            distance.push_back (float (z->PlanDistance (x[s], y[s])));
}

double PlantingZone::PlanArea () const
{
    if (rings.empty ())
        return 0.0;
    double area = std::abs (SignedArea (rings[0]));
    for (size_t r = 1; r < rings.size (); ++r)
        area -= std::abs (SignedArea (rings[r]));
    return std::max (area, 0.0);
}

bool PlantingZone::Contains (double x, double y) const
{
    if (rings.empty () || rings[0].size () < 6 || !InRing (rings[0], x, y))
        return false;
    for (size_t r = 1; r < rings.size (); ++r)
        if (rings[r].size () >= 6 && InRing (rings[r], x, y))
            return false;
    return true;
}

std::vector<uint32_t> ZoneMasks (const std::vector<PlantingZone>& zones, const std::vector<double>& x,
                                 const std::vector<double>& y)
{
    std::vector<uint32_t> masks (x.size (), 0u);
    const size_t count = std::min (zones.size (), kMaxPlantingZones);
    for (size_t z = 0; z < count; ++z) {
        // Bounding box first: most samples of a courtyard lie outside a small bed.
        const auto& outline = zones[z].rings.empty () ? std::vector<double> () : zones[z].rings[0];
        if (outline.size () < 6)
            continue;
        double minX = outline[0], maxX = outline[0], minY = outline[1], maxY = outline[1];
        for (size_t i = 0; i + 1 < outline.size (); i += 2) {
            minX = std::min (minX, outline[i]);
            maxX = std::max (maxX, outline[i]);
            minY = std::min (minY, outline[i + 1]);
            maxY = std::max (maxY, outline[i + 1]);
        }
        for (size_t s = 0; s < x.size (); ++s)
            if (x[s] >= minX && x[s] <= maxX && y[s] >= minY && y[s] <= maxY && zones[z].Contains (x[s], y[s]))
                masks[s] |= 1u << z;
    }
    return masks;
}

ZoneClassAreas ClassAreasInZone (const std::vector<uint32_t>& masks, uint32_t zoneBit, const std::vector<double>& hours,
                                 const std::vector<double>& areas, const std::vector<double>& edges)
{
    ZoneClassAreas out;
    out.classAreas.assign (edges.size () + 1, 0.0);
    const size_t n = std::min ({ masks.size (), hours.size (), areas.size () });
    for (size_t s = 0; s < n; ++s) {
        const bool in = zoneBit == 0 ? masks[s] == 0 : (masks[s] & zoneBit) != 0;
        if (!in)
            continue;
        size_t cls = 0;
        for (double edge : edges)
            if (hours[s] >= edge)
                ++cls;
        out.classAreas[cls] += areas[s];
        out.measured += areas[s];
        ++out.samples;
    }
    return out;
}

PlantFit FitInZone (const PlantingHud& hud, const Plant& plant, const PlantFit& layerFit, const ZoneClassAreas& zone)
{
    PlantFit fit = layerFit;
    if (fit.layer < 0 || fit.status == PlantFit::Status::Unknown)
        return fit; // no light classes, or no layer: unknown inside the zone too
    fit.evaluatedArea = zone.measured;
    fit.compatibleArea = 0.0;
    if (zone.samples == 0) {
        fit.status = PlantFit::Status::Unknown;
        fit.reason = "the zone has no measured samples on that layer";
        return fit;
    }
    const uint32_t mask = AcceptedClassMask (hud, plant);
    for (size_t cls = 0; cls < zone.classAreas.size (); ++cls)
        if (mask & (1u << cls))
            fit.compatibleArea += zone.classAreas[cls];
    fit.status = fit.compatibleArea > 0 ? PlantFit::Status::Fits : PlantFit::Status::None;
    return fit;
}

} // namespace geomsrv::archviz::planting
