#include "ArchViz/PlantingScheme.hpp"

#include "ArchViz/PlantHardiness.hpp"
#include "ArchViz/PlantingHudModel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>

namespace geomsrv::archviz::planting {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kAssumedRadius = 0.25;

std::string NameList (const std::vector<std::string>& names)
{
    std::string s;
    for (size_t i = 0; i < names.size (); ++i)
        s += (i == 0 ? "" : i + 1 == names.size () ? " and " : ", ") + names[i];
    return s;
}

bool HasChoice (const PlantTraits& traits, Trait t, const char* key)
{
    const TraitField& f = TraitFieldOf (t);
    const TraitValue& v = traits[t];
    auto is = [&] (int c) { return c >= 0 && c < f.choiceCount && std::string (f.choices[c]) == key; };
    return is (v.choice) || std::any_of (v.choices.begin (), v.choices.end (), is);
}

std::string Number (double v)
{
    char text[32];
    std::snprintf (text, sizeof (text), "%g", v);
    return text;
}

// Prim's tree over one species' plants, in plan.
void LinkSpecies (const std::vector<PlacedPlant>& plants, const std::vector<int>& members,
                  std::vector<SchemeLink>& links)
{
    const size_t n = members.size ();
    if (n < 2)
        return;
    std::vector<double> best (n, std::numeric_limits<double>::infinity ());
    std::vector<int> from (n, -1);
    std::vector<bool> in (n, false);
    best[0] = 0;
    for (size_t step = 0; step < n; ++step) {
        size_t pick = n;
        for (size_t i = 0; i < n; ++i)
            if (!in[i] && (pick == n || best[i] < best[pick]))
                pick = i;
        in[pick] = true;
        if (from[pick] >= 0)
            links.push_back ({ members[size_t (from[pick])], members[pick] });
        const PlacedPlant& p = plants[size_t (members[pick])];
        for (size_t i = 0; i < n; ++i) {
            if (in[i])
                continue;
            const PlacedPlant& q = plants[size_t (members[i])];
            const double d = std::hypot (p.x - q.x, p.y - q.y);
            if (d < best[i]) {
                best[i] = d;
                from[i] = int (pick);
            }
        }
    }
}

} // namespace

const char* SpaceFitName (SpaceFit fit)
{
    switch (fit) {
        case SpaceFit::Fits:
            return "fits";
        case SpaceFit::Overhead:
            return "under an overhang";
        case SpaceFit::Narrow:
            return "too close to a wall";
        case SpaceFit::Beyond:
            return "wider than the measured reach";
    }
    return "";
}

SpaceFit FitAt (const evp::sunstudy::CrownClearance& room, size_t sample, double radius, double height, double margin)
{
    const double above = room.overhead[sample];
    if (above >= 0.0 && above < height + margin)
        return SpaceFit::Overhead;
    const double need = radius + margin;
    const bool beyond = need > room.reach;
    for (size_t l = 0; l < room.levels.size (); ++l) {
        if (l > 0 && room.levels[l] > height)
            break;
        const double free = room.FreeAt (sample, l);
        if (free < need && free < room.reach)
            return SpaceFit::Narrow;
    }
    return beyond ? SpaceFit::Beyond : SpaceFit::Fits;
}

bool KeptOut (const SchemeSite& site, size_t sample, double radius, double height, double margin)
{
    const size_t zones = site.noGoAllowance.size ();
    if (zones == 0 || site.noGoDistance.size () != site.Size () * zones)
        return false;
    for (size_t z = 0; z < zones; ++z)
        if (height > double (site.noGoAllowance[z]) && double (site.noGoDistance[sample * zones + z]) < radius + margin)
            return true;
    return false;
}

namespace {

// The samples' own spacing: the median cell side, else 0.5 m.
double SampleSpacing (const SchemeSite& site)
{
    const size_t n = site.Size ();
    double cell = 0.5;
    if (site.area.size () == n && n > 0) {
        std::vector<double> sides;
        sides.reserve (n);
        for (double a : site.area)
            if (a > 0.0)
                sides.push_back (std::sqrt (a));
        if (!sides.empty ()) {
            std::nth_element (sides.begin (), sides.begin () + std::ptrdiff_t (sides.size () / 2), sides.end ());
            cell = std::max (0.05, sides[sides.size () / 2]);
        }
    }
    return cell;
}

// The nearest sample to a plan point, through buckets one spacing wide.
class SampleBuckets {
  public:
    SampleBuckets (const SchemeSite& site, double cell) : site_ (site), cell_ (cell)
    {
        for (size_t s = 0; s < site.Size (); ++s)
            buckets_[Key (Cell (site.x[s]), Cell (site.y[s]))].push_back (s);
    }
    // SIZE_MAX when no sample is within `reach`.
    size_t Nearest (double x, double y, double reach) const
    {
        size_t best = SIZE_MAX;
        double bestDistance = reach;
        const int64_t cx = Cell (x), cy = Cell (y);
        const int64_t span = int64_t (std::ceil (reach / cell_));
        for (int64_t i = cx - span; i <= cx + span; ++i)
            for (int64_t j = cy - span; j <= cy + span; ++j) {
                const auto it = buckets_.find (Key (i, j));
                if (it == buckets_.end ())
                    continue;
                for (size_t s : it->second) {
                    const double d = std::hypot (site_.x[s] - x, site_.y[s] - y);
                    if (d < bestDistance || (d == bestDistance && s < best)) {
                        bestDistance = d;
                        best = s;
                    }
                }
            }
        return best;
    }

  private:
    int64_t Cell (double v) const
    {
        return int64_t (std::floor (v / cell_));
    }
    static int64_t Key (int64_t i, int64_t j)
    {
        return (i << 32) ^ (j & 0xffffffffll);
    }
    const SchemeSite& site_;
    double cell_;
    std::unordered_map<int64_t, std::vector<size_t>> buckets_;
};

} // namespace

double EdgeField::At (double x, double y) const
{
    if (Empty ())
        return 0.0;
    const double fx = (x - x0) / cell, fy = (y - y0) / cell;
    const int i = int (std::floor (fx)), j = int (std::floor (fy));
    if (i < 0 || j < 0 || i + 1 >= w || j + 1 >= h)
        return 0.0;
    const double tx = fx - i, ty = fy - j;
    const auto v = [&] (int a, int b) { return double (metres[size_t (b) * size_t (w) + size_t (a)]); };
    return (1 - ty) * ((1 - tx) * v (i, j) + tx * v (i + 1, j)) +
           ty * ((1 - tx) * v (i, j + 1) + tx * v (i + 1, j + 1));
}

double SchemeSite::EdgeDistanceAt (size_t sample, double x, double y) const
{
    const bool field = !edgeField.Empty ();
    const bool zone =
        regionZone.size () == Size () && regionZone[sample] >= 0 && size_t (regionZone[sample]) < regionZones.size ();
    if (!field && !zone)
        return edgeDistance.size () == Size ()
                   ? double (edgeDistance[sample]) - std::hypot (x - this->x[sample], y - this->y[sample])
                   : std::numeric_limits<double>::infinity ();
    double d = field ? edgeField.At (x, y) : std::numeric_limits<double>::infinity ();
    if (zone) {
        const PlantingZone& z = regionZones[size_t (regionZone[sample])];
        d = std::min (d, z.Contains (x, y) ? z.EdgeDistance (x, y) : 0.0);
    }
    return d;
}

void SampleEdgeDistances (SchemeSite& site)
{
    site.edgeDistance.clear ();
    const size_t n = site.Size ();
    if (n == 0)
        return;
    // The raster's cell: the samples' own spacing.
    double cell = SampleSpacing (site);
    double x0 = site.x[0], x1 = x0, y0 = site.y[0], y1 = y0;
    for (size_t s = 0; s < n; ++s) {
        x0 = std::min (x0, site.x[s]);
        x1 = std::max (x1, site.x[s]);
        y0 = std::min (y0, site.y[s]);
        y1 = std::max (y1, site.y[s]);
    }
    // At most about four million cells: a coarser raster for a huge site.
    while ((x1 - x0) / cell * (y1 - y0) / cell > 4.0e6)
        cell *= 2.0;
    const int border = 3; // empty cells around the area, so its outline is an edge
    const int w = int ((x1 - x0) / cell) + 1 + 2 * border, h = int ((y1 - y0) / cell) + 1 + 2 * border;
    const auto at = [&] (int i, int j) { return size_t (j) * size_t (w) + size_t (i); };
    std::vector<uint8_t> in (size_t (w) * size_t (h), 0);
    std::vector<int> ci (n), cj (n);
    for (size_t s = 0; s < n; ++s) {
        ci[s] = border + int ((site.x[s] - x0) / cell + 0.5);
        cj[s] = border + int ((site.y[s] - y0) / cell + 0.5);
        ci[s] = std::clamp (ci[s], 0, w - 1);
        cj[s] = std::clamp (cj[s], 0, h - 1);
        in[at (ci[s], cj[s])] = 1;
    }
    // Close by one cell (dilate, then erode): lattices of neighbouring faces
    // do not line up with the raster, and a skipped cell is not an edge.
    const auto step = [&] (const std::vector<uint8_t>& from, bool dilate) {
        std::vector<uint8_t> to (from.size (), 0);
        for (int j = 1; j + 1 < h; ++j)
            for (int i = 1; i + 1 < w; ++i) {
                bool any = false, all = true;
                for (int dj = -1; dj <= 1; ++dj)
                    for (int di = -1; di <= 1; ++di) {
                        const bool v = from[at (i + di, j + dj)] != 0;
                        any = any || v;
                        all = all && v;
                    }
                to[at (i, j)] = uint8_t (dilate ? any : all);
            }
        return to;
    };
    in = step (step (in, true), false);
    for (size_t s = 0; s < n; ++s)
        in[at (ci[s], cj[s])] = 1; // a lone sample stays measured
    // Two-pass chamfer distance (1, sqrt 2) to the nearest empty cell, in cells.
    constexpr float kFar = 1.0e9f, kDiagonal = 1.41421356f;
    std::vector<float> d (in.size ());
    for (size_t k = 0; k < in.size (); ++k)
        d[k] = in[k] ? kFar : 0.0f;
    for (int j = 1; j < h; ++j)
        for (int i = 1; i + 1 < w; ++i) {
            float& v = d[at (i, j)];
            v = std::min ({ v, d[at (i - 1, j)] + 1.0f, d[at (i, j - 1)] + 1.0f, d[at (i - 1, j - 1)] + kDiagonal,
                            d[at (i + 1, j - 1)] + kDiagonal });
        }
    for (int j = h - 2; j >= 0; --j)
        for (int i = w - 2; i >= 1; --i) {
            float& v = d[at (i, j)];
            v = std::min ({ v, d[at (i + 1, j)] + 1.0f, d[at (i, j + 1)] + 1.0f, d[at (i + 1, j + 1)] + kDiagonal,
                            d[at (i - 1, j + 1)] + kDiagonal });
        }
    // A cell's centre is half a cell inside the boundary of an edge cell.
    site.edgeField.x0 = x0 - border * cell;
    site.edgeField.y0 = y0 - border * cell;
    site.edgeField.cell = cell;
    site.edgeField.w = w;
    site.edgeField.h = h;
    site.edgeField.metres.resize (d.size ());
    for (size_t k = 0; k < d.size (); ++k)
        site.edgeField.metres[k] = float (std::max (0.0, (double (d[k]) - 0.5) * cell));
    site.edgeDistance.resize (n);
    for (size_t s = 0; s < n; ++s)
        site.edgeDistance[s] = float (std::max (0.0, (double (d[at (ci[s], cj[s])]) - 0.5) * cell));
}

SchemeSite RestrictToZone (const SchemeSite& site, const PlantingZone& zone)
{
    return RestrictToZones (site, { &zone });
}

SchemeSite RestrictToZones (const SchemeSite& site, const std::vector<const PlantingZone*>& zonesIn)
{
    SchemeSite out;
    out.layerHeight = site.layerHeight;
    std::vector<size_t> keep;
    std::vector<const PlantingZone*> home; // the zone each kept sample is in
    for (size_t s = 0; s < site.Size (); ++s) {
        const auto in = std::find_if (zonesIn.begin (), zonesIn.end (),
                                      [&] (const PlantingZone* z) { return z->Contains (site.x[s], site.y[s]); });
        if (in == zonesIn.end ())
            continue;
        keep.push_back (s);
        home.push_back (*in);
        out.x.push_back (site.x[s]);
        out.y.push_back (site.y[s]);
        out.groundZ.push_back (site.groundZ[s]);
        out.cls.push_back (site.cls[s]);
        out.host.push_back (site.host[s]);
        if (site.area.size () == site.Size ())
            out.area.push_back (site.area[s]);
        out.index.push_back (site.SourceOf (s));
    }
    if (site.clearance.Count () == site.Size ())
        out.clearance = site.clearance.Subset (keep);
    // Inside the zone, and inside what was measured: the nearer edge counts.
    out.edgeField = site.edgeField;
    for (const PlantingZone* z : zonesIn)
        out.regionZones.push_back (*z);
    for (const PlantingZone* z : home)
        out.regionZone.push_back (int (std::find (zonesIn.begin (), zonesIn.end (), z) - zonesIn.begin ()));
    for (size_t k = 0; k < keep.size (); ++k) {
        const size_t s = keep[k];
        const double zoneEdge = home[k]->EdgeDistance (site.x[s], site.y[s]);
        const double layerEdge = site.edgeDistance.size () == site.Size () ? double (site.edgeDistance[s]) : zoneEdge;
        out.edgeDistance.push_back (float (std::min (zoneEdge, layerEdge)));
    }
    const size_t zones = site.noGoAllowance.size ();
    if (zones > 0 && site.noGoDistance.size () == site.Size () * zones) {
        out.noGoAllowance = site.noGoAllowance;
        for (size_t s : keep)
            out.noGoDistance.insert (out.noGoDistance.end (), site.noGoDistance.begin () + std::ptrdiff_t (s * zones),
                                     site.noGoDistance.begin () + std::ptrdiff_t ((s + 1) * zones));
    }
    return out;
}

int FindPlant (const Catalogue& catalogue, const std::string& id)
{
    for (size_t i = 0; i < catalogue.plants.size (); ++i)
        if (catalogue.plants[i].id == id)
            return int (i);
    return -1;
}

uint32_t SchemeSpeciesRgba (int species)
{
    // Distinct, mid-light hues that read on both the light and dark model.
    static constexpr uint32_t kPalette[] = { 0x4E9A52FFu, 0xD9822BFFu, 0x3A78C2FFu, 0xB8457AFFu, 0x8C6FD1FFu,
                                             0xC9B12EFFu, 0x2FA39BFFu, 0xC2523AFFu, 0x6B8E23FFu, 0x7F8C99FFu };
    constexpr int n = int (sizeof (kPalette) / sizeof (kPalette[0]));
    return kPalette[((species % n) + n) % n];
}

double SchematicHeightOf (const Plant& plant)
{
    switch (plant.habit) {
        case Habit::Tree:
            return 3.0;
        case Habit::Shrub:
        case Habit::Climber:
            return 1.5;
        default:
            return 0.5;
    }
}

double HexSpacing (double density)
{
    return std::sqrt (2.0 / (std::sqrt (3.0) * std::max (density, 1e-6)));
}

double RecommendedDensity (const Plant& plant)
{
    if (plant.densityPerM2 && plant.densityPerM2->min > 0) {
        const double hi = plant.densityPerM2->max > 0 ? plant.densityPerM2->max : plant.densityPerM2->min;
        return 0.5 * (plant.densityPerM2->min + hi);
    }
    double spread = 2.0 * kAssumedRadius;
    if (const auto& r = plant.matureSpreadM) {
        const double d = r->openMax ? r->min : r->max;
        if (d > 0)
            spread = d;
    }
    return 2.0 / (std::sqrt (3.0) * spread * spread);
}

SchemeSpeciesSize SizeOf (const Plant& plant)
{
    SchemeSpeciesSize s;
    if (const auto& r = plant.matureSpreadM) {
        const double d = r->openMax ? r->min : r->max; // the conservative end that is stated
        if (d > 0) {
            s.radius = d / 2;
            s.radiusFrom = "spread";
        }
    }
    if (s.radiusFrom.empty () && plant.densityPerM2 && plant.densityPerM2->min > 0) {
        s.radius = HexSpacing (plant.densityPerM2->min) / 2.0;
        s.radiusFrom = "density";
    }
    if (s.radiusFrom.empty ()) {
        s.radius = kAssumedRadius;
        s.radiusFrom = "assumed";
    }
    if (const auto& h = plant.matureHeightM) {
        const double v = h->openMax ? h->min : h->max;
        if (v > 0) {
            s.height = v;
            s.heightFrom = "height";
        }
    }
    if (s.heightFrom.empty ()) {
        s.height = ReceiverHeightOf (plant);
        s.heightFrom = "layer";
    }
    return s;
}

std::vector<SchemeNote> CheckCommunity (const Catalogue& catalogue, const std::vector<SchemeSpecies>& species)
{
    std::vector<SchemeNote> notes;
    std::vector<const Plant*> plants;
    for (size_t i = 0; i < species.size (); ++i) {
        const int at = FindPlant (catalogue, species[i].plantId);
        plants.push_back (at >= 0 ? &catalogue.plants[size_t (at)] : nullptr);
        if (at < 0)
            notes.push_back ({ SchemeSeverity::Conflict,
                               "'" + species[i].plantId + "' is no longer in the catalogue.",
                               { int (i) } });
    }
    auto name = [&] (size_t i) { return plants[i]->DisplayName (); };

    // Hard exceptions first: a legal status overrides suitability.
    for (size_t i = 0; i < plants.size (); ++i) {
        if (!plants[i])
            continue;
        const char* status = plants[i]->traits.ChoiceKey (Trait::RegulatoryStatus);
        if (status && std::string (status) == "prohibited")
            notes.push_back (
                { SchemeSeverity::Conflict, name (i) + " is prohibited (regulatory status).", { int (i) } });
        else if (status && std::string (status) == "restricted")
            notes.push_back (
                { SchemeSeverity::Warning, name (i) + " is restricted (regulatory status).", { int (i) } });
    }

    // Shared site envelope: water and ground. A mismatch is a zoning problem.
    {
        std::set<MoistureClass> common;
        std::map<MoistureClass, std::vector<std::string>> by;
        std::vector<int> who;
        bool first = true;
        for (size_t i = 0; i < plants.size (); ++i) {
            if (!plants[i] || plants[i]->moistureClasses.empty ())
                continue;
            const std::set<MoistureClass> mine (plants[i]->moistureClasses.begin (), plants[i]->moistureClasses.end ());
            for (MoistureClass m : mine)
                by[m].push_back (name (i));
            who.push_back (int (i));
            if (first)
                common = mine;
            else {
                std::set<MoistureClass> keep;
                std::set_intersection (common.begin (), common.end (), mine.begin (), mine.end (),
                                       std::inserter (keep, keep.begin ()));
                common = keep;
            }
            first = false;
        }
        if (who.size () > 1 && common.empty ()) {
            std::string text = "Water needs do not overlap:";
            for (const auto& [m, names] : by)
                text += std::string (" ") + MoistureClassName (m) + " (" + NameList (names) + ");";
            text.back () = '.';
            text += " Plant them in separate watering zones; this is a site mismatch, not plant-to-plant conflict.";
            notes.push_back ({ SchemeSeverity::Warning, text, who });
        }
    }
    {
        std::set<std::string> common;
        std::vector<int> who;
        bool first = true;
        for (size_t i = 0; i < plants.size (); ++i) {
            if (!plants[i] || plants[i]->soilTypes.empty ())
                continue;
            const std::set<std::string> mine (plants[i]->soilTypes.begin (), plants[i]->soilTypes.end ());
            who.push_back (int (i));
            if (first)
                common = mine;
            else {
                std::set<std::string> keep;
                std::set_intersection (common.begin (), common.end (), mine.begin (), mine.end (),
                                       std::inserter (keep, keep.begin ()));
                common = keep;
            }
            first = false;
        }
        if (who.size () > 1 && common.empty ())
            notes.push_back ({ SchemeSeverity::Warning,
                               "No ground type suits every plant listed with one; they need different beds.", who });
    }

    // The filled traits' site envelope (research note "plant compat"): soil pH,
    // then Ellenberg moisture and nutrients. Mismatches are zoning problems.
    {
        double lo = -1e9, hi = 1e9;
        std::vector<std::string> ranges;
        std::vector<int> who;
        for (size_t i = 0; i < plants.size (); ++i) {
            if (!plants[i])
                continue;
            const auto& a = plants[i]->traits[Trait::PhMin].number;
            const auto& b = plants[i]->traits[Trait::PhMax].number;
            if (!a || !b)
                continue;
            lo = std::max (lo, *a);
            hi = std::min (hi, *b);
            ranges.push_back (name (i) + " " + Number (*a) + "-" + Number (*b));
            who.push_back (int (i));
        }
        if (who.size () > 1 && lo > hi)
            notes.push_back ({ SchemeSeverity::Warning,
                               "Soil pH ranges do not overlap (" + NameList (ranges) +
                                   "): one soil cannot suit them all; give them separate beds.",
                               who });
    }
    auto spread = [&] (Trait t, double wide, SchemeSeverity severity, const char* scale, const char* advice) {
        int low = -1, high = -1;
        std::vector<int> who;
        for (size_t i = 0; i < plants.size (); ++i) {
            if (!plants[i] || !plants[i]->traits[t].number)
                continue;
            const double v = *plants[i]->traits[t].number;
            if (low < 0 || v < *plants[size_t (low)]->traits[t].number)
                low = int (i);
            if (high < 0 || v > *plants[size_t (high)]->traits[t].number)
                high = int (i);
            who.push_back (int (i));
        }
        if (who.size () < 2)
            return;
        const double a = *plants[size_t (low)]->traits[t].number, b = *plants[size_t (high)]->traits[t].number;
        if (b - a >= wide)
            notes.push_back ({ severity,
                               std::string (scale) + " are far apart: " + name (size_t (low)) + " " + Number (a) +
                                   ", " + name (size_t (high)) + " " + Number (b) + ". " + advice,
                               who });
    };
    spread (Trait::EllenbergMoisture, 4.0, SchemeSeverity::Warning, "Ellenberg moisture values (F)",
            "Give them separate watering zones; a site mismatch, not plant-to-plant conflict.");
    spread (Trait::EllenbergNutrients, 5.0, SchemeSeverity::Info, "Ellenberg nutrient values (N)",
            "A fertile bed favours the hungrier plant; a lean one, the other.");

    // Competition: a strong spreader beside a slow or weak plant. Strong: the
    // standard's "aggressive", high vigour, high lateral spread, or spreading by
    // long rhizomes, stolons or root suckers. Weak: the standard's "slow", low
    // vigour, or slow establishment.
    for (size_t i = 0; i < plants.size (); ++i) {
        if (!plants[i])
            continue;
        const PlantTraits& t = plants[i]->traits;
        std::string how;
        if (plants[i]->growthIntensity == GrowthIntensity::Aggressive)
            how = "grows aggressively (the standard)";
        else if (HasChoice (t, Trait::CompetitiveVigour, "high"))
            how = "has high competitive vigour";
        else if (HasChoice (t, Trait::SpreadMode, "longRhizome"))
            how = "spreads by long rhizomes";
        else if (HasChoice (t, Trait::SpreadMode, "stolon"))
            how = "spreads by stolons";
        else if (HasChoice (t, Trait::SpreadMode, "rootSucker"))
            how = "spreads by root suckers";
        else if (HasChoice (t, Trait::LateralSpread, "high"))
            how = "spreads widely";
        if (how.empty ())
            continue;
        std::vector<std::string> weak;
        std::vector<int> who { int (i) };
        for (size_t k = 0; k < plants.size (); ++k) {
            if (k == i || !plants[k])
                continue;
            const PlantTraits& o = plants[k]->traits;
            if (plants[k]->growthIntensity == GrowthIntensity::Slow || HasChoice (o, Trait::CompetitiveVigour, "low") ||
                HasChoice (o, Trait::EstablishmentSpeed, "slow")) {
                weak.push_back (name (k));
                who.push_back (int (k));
            }
        }
        if (!weak.empty ())
            notes.push_back ({ SchemeSeverity::Warning,
                               name (i) + " " + how + " and may overrun " + NameList (weak) +
                                   " (slow or weak); keep them apart, contain it, or plan for division.",
                               who });
    }

    // What the mix fills: the standard's bed layers and tiers.
    {
        bool layer[kPlantingLayerCount] = {};
        int trees = 0, shrubs = 0, herbs = 0;
        for (const Plant* p : plants) {
            if (!p)
                continue;
            for (PlantingLayer l : p->plantingLayers)
                layer[int (l)] = true;
            trees += p->habit == Habit::Tree;
            shrubs += p->habit == Habit::Shrub;
            herbs += p->habit != Habit::Tree && p->habit != Habit::Shrub;
        }
        char text[160];
        std::snprintf (text, sizeof (text),
                       "Mix: %d tree(s), %d shrub(s), %d herbaceous. Bed layers: cover %s, seasonal %s, "
                       "structural %s.",
                       trees, shrubs, herbs, layer[0] ? "yes" : "no", layer[1] ? "yes" : "no", layer[2] ? "yes" : "no");
        notes.push_back ({ SchemeSeverity::Info, text, {} });
    }

    // Missing data: unknown is not a pass.
    for (size_t i = 0; i < plants.size (); ++i) {
        if (!plants[i])
            continue;
        std::vector<std::string> missing;
        if (!plants[i]->HasLight ())
            missing.push_back ("light");
        if (plants[i]->moistureClasses.empty ())
            missing.push_back ("water");
        if (plants[i]->soilTypes.empty () && !std::any_of (plants[i]->siteSoil.begin (), plants[i]->siteSoil.end (),
                                                           [] (const std::string& s) { return !s.empty (); }))
            missing.push_back ("ground type");
        if (!missing.empty ())
            notes.push_back ({ SchemeSeverity::Info,
                               name (i) + ": no " + NameList (missing) + " in the catalogue; not checked.",
                               { int (i) } });
    }
    return notes;
}

SchemeResult PlaceScheme (const PlantingHud& hud, const Catalogue& catalogue, const std::vector<SchemeSpecies>& species,
                          const SchemeSite& site, const SchemeOptions& options)
{
    SchemeResult result;
    const size_t n = site.Size ();
    result.wanted.resize (species.size ());
    result.placed.assign (species.size (), 0);
    result.sizes.resize (species.size ());
    result.density.assign (species.size (), 0.0);
    result.spacing.assign (species.size (), 0.0);
    std::vector<const Plant*> plants (species.size (), nullptr);
    for (size_t i = 0; i < species.size (); ++i) {
        result.wanted[i] = std::max (0, species[i].count);
        const int at = FindPlant (catalogue, species[i].plantId);
        if (at >= 0) {
            plants[i] = &catalogue.plants[size_t (at)];
            result.sizes[i] = SizeOf (*plants[i]);
        }
    }

    // Largest crowns first: a tree is never squeezed out by the groundcover.
    std::vector<size_t> order (species.size ());
    std::iota (order.begin (), order.end (), size_t (0));
    std::stable_sort (order.begin (), order.end (),
                      [&] (size_t a, size_t b) { return result.sizes[a].radius > result.sizes[b].radius; });

    // The room is used only when it was measured for these very samples.
    const bool space = options.checkSpace && !site.clearance.Empty () && site.clearance.Count () == n;
    result.spaceChecked = space;
    const bool noGo = !site.noGoAllowance.empty ();
    const bool contain = options.containCrowns && site.edgeDistance.size () == n;
    const double offset = std::max (0.0, options.edgeOffset);
    const bool checkEdge = site.edgeDistance.size () == n && (contain || offset > 0.0);
    // How far inside the region's edge a plant's centre must be.
    const auto edgeNeed = [&] (double r) { return (contain ? r : 0.0) + offset; };
    result.room.resize (species.size ());
    const double cell = SampleSpacing (site);
    const SampleBuckets buckets (site, cell);

    bool capped = false;
    for (size_t i : order) {
        if (!plants[i])
            continue;
        const Plant& plant = *plants[i];
        const double r = result.sizes[i].radius;
        const double need = r * (1.0 - std::clamp (options.crownOverlap, 0.0, 1.0));
        const uint32_t mask = plant.HasLight () ? AcceptedClassMask (hud, plant) : ~0u;
        // "As many as fit" runs one past the cap, so reaching it is reported.
        const int wanted = result.wanted[i] > 0 ? result.wanted[i] : options.maxPlants + 1;
        // ok[s]: an accepted light class and, when measured, room for the crown.
        std::vector<uint8_t> ok (n, 0);
        for (size_t s = 0; s < n; ++s) {
            if (site.cls[s] < 0 || !((mask >> site.cls[s]) & 1u))
                continue;
            SchemeRoom& room = result.room[i];
            ++room.lightSamples;
            if (checkEdge && double (site.edgeDistance[s]) + 1e-6 < edgeNeed (r)) {
                ++room.outside;
                continue;
            }
            if (!space && !noGo) {
                ++room.fits;
                ok[s] = 1;
                continue;
            }
            if (KeptOut (site, s, r, result.sizes[i].height, options.clearanceMargin)) {
                ++room.noGo;
                continue;
            }
            if (!space) {
                ++room.fits;
                ok[s] = 1;
                continue;
            }
            switch (FitAt (site.clearance, s, r, result.sizes[i].height, options.clearanceMargin)) {
                case SpaceFit::Fits:
                    ++room.fits;
                    ok[s] = 1;
                    break;
                case SpaceFit::Beyond:
                    ++room.beyond;
                    ok[s] = 1; // not known is not refused; a note says so
                    break;
                case SpaceFit::Overhead:
                    ++room.overhead;
                    break;
                case SpaceFit::Narrow:
                    ++room.narrow;
                    break;
            }
        }
        // The usable area: its centroid and its lower-left corner.
        double cx = 0, cy = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        size_t accepted = 0;
        for (size_t s = 0; s < n; ++s) {
            if (!ok[s])
                continue;
            if (accepted == 0) {
                x0 = x1 = site.x[s];
                y0 = y1 = site.y[s];
            }
            cx += site.x[s];
            cy += site.y[s];
            x0 = std::min (x0, site.x[s]);
            x1 = std::max (x1, site.x[s]);
            y0 = std::min (y0, site.y[s]);
            y1 = std::max (y1, site.y[s]);
            ++accepted;
        }
        const double density = species[i].density > 0.0 ? species[i].density : RecommendedDensity (plant);
        const double spacing = HexSpacing (density);
        result.density[i] = density;
        result.spacing[i] = spacing;
        if (accepted == 0)
            continue;
        cx /= double (accepted);
        cy /= double (accepted);
        const bool corner = options.start == SchemeStart::Corner;
        const double ox = corner ? x0 : cx, oy = corner ? y0 : cy;

        // Candidates: where a plant may stand, in the order they are taken.
        struct Candidate {
            double x, y, key;
            size_t sample;
        };
        std::vector<Candidate> candidates;
        if (options.pattern == SchemePattern::Hex) {
            const double row = spacing * std::sqrt (3.0) / 2.0;
            // A grid of millions of points is a density the area cannot hold anyway.
            const double points = (x1 - x0 + 2 * spacing) * (y1 - y0 + 2 * spacing) / (spacing * row);
            if (points > 2.0e6)
                continue;
            const int j0 = int (std::floor ((y0 - oy) / row)) - 1, j1 = int (std::ceil ((y1 - oy) / row)) + 1;
            for (int j = j0; j <= j1; ++j) {
                const double y = oy + j * row;
                const double shift = (j % 2 != 0) ? spacing / 2.0 : 0.0;
                const int k0 = int (std::floor ((x0 - ox - shift) / spacing)) - 1;
                const int k1 = int (std::ceil ((x1 - ox - shift) / spacing)) + 1;
                for (int k = k0; k <= k1; ++k) {
                    const double x = ox + shift + k * spacing;
                    const size_t s = buckets.Nearest (x, y, 0.75 * cell);
                    if (s == SIZE_MAX || !ok[s])
                        continue;
                    // The grid point is off its sample by d: the edge and the
                    // no-go zones are that much nearer, never assumed farther.
                    const double d = std::hypot (x - site.x[s], y - site.y[s]);
                    if (checkEdge && site.EdgeDistanceAt (s, x, y) + 1e-6 < edgeNeed (r))
                        continue;
                    if (noGo && KeptOut (site, s, r, result.sizes[i].height, options.clearanceMargin + d))
                        continue;
                    candidates.push_back ({ x, y, std::hypot (x - ox, y - oy), s });
                }
            }
            std::stable_sort (candidates.begin (), candidates.end (),
                              [] (const Candidate& a, const Candidate& b) { return a.key < b.key; });
        }

        const auto clear = [&] (double x, double y) {
            for (const PlacedPlant& q : result.plants) {
                const double apart = q.species == int (i) ? spacing - 1e-6 : q.radius + need - 1e-9;
                if (std::hypot (q.x - x, q.y - y) < apart)
                    return false;
            }
            return true;
        };
        const auto place = [&] (double x, double y, size_t s) {
            PlacedPlant p;
            p.species = int (i);
            p.x = x;
            p.y = y;
            p.z = site.groundZ[s];
            p.radius = r;
            p.height = result.sizes[i].height;
            p.schematicHeight = SchematicHeightOf (plant);
            p.cls = site.cls[s];
            p.lightChecked = plant.HasLight ();
            p.spaceChecked = space;
            result.plants.push_back (p);
            ++result.placed[i];
        };

        if (options.pattern == SchemePattern::Hex) {
            for (const Candidate& c : candidates) {
                if (result.placed[i] >= wanted)
                    break;
                if (int (result.plants.size ()) >= options.maxPlants) {
                    capped = true;
                    break;
                }
                if (clear (c.x, c.y))
                    place (c.x, c.y, c.sample);
            }
            continue;
        }

        // Spread: each plant at the usable sample farthest from those placed;
        // the first nearest the start.
        for (int k = 0; k < wanted; ++k) {
            if (int (result.plants.size ()) >= options.maxPlants) {
                capped = true;
                break;
            }
            size_t best = n;
            double bestScore = -std::numeric_limits<double>::infinity ();
            for (size_t s = 0; s < n; ++s) {
                if (!ok[s] || !clear (site.x[s], site.y[s]))
                    continue;
                double gap = std::numeric_limits<double>::infinity ();
                for (const PlacedPlant& q : result.plants)
                    gap = std::min (gap, std::hypot (site.x[s] - q.x, site.y[s] - q.y) - q.radius);
                const double score = std::isinf (gap) ? -std::hypot (site.x[s] - ox, site.y[s] - oy) : gap;
                if (score > bestScore) {
                    bestScore = score;
                    best = s;
                }
            }
            if (best == n)
                break;
            place (site.x[best], site.y[best], best);
        }
    }

    for (size_t i = 0; i < species.size (); ++i) {
        std::vector<int> members;
        for (size_t k = 0; k < result.plants.size (); ++k)
            if (result.plants[k].species == int (i))
                members.push_back (int (k));
        LinkSpecies (result.plants, members, result.links);
    }

    result.notes = CheckCommunity (catalogue, species);
    // Winter hardiness against the design zone the user chose (Climate).
    const int zone = hud.climateState.designZone;
    if (zone >= 0) {
        std::vector<std::string> unknown;
        std::vector<int> unknownIds;
        for (size_t i = 0; i < species.size (); ++i) {
            if (!plants[i])
                continue;
            const std::string& range = plants[i]->traits[Trait::Hardiness].text;
            const Hardiness h = HardinessAt (range, zone);
            if (h == Hardiness::TooCold)
                result.notes.push_back ({ SchemeSeverity::Conflict,
                                          plants[i]->DisplayName () + " is hardy only to " + range +
                                              "; the design zone is USDA " + UsdaName (zone) + ".",
                                          { int (i) } });
            else if (h == Hardiness::Unknown) {
                unknown.push_back (plants[i]->DisplayName ());
                unknownIds.push_back (int (i));
            }
        }
        if (!unknown.empty ())
            result.notes.push_back (
                { SchemeSeverity::Info,
                  "Winter hardiness not checked (no USDA or RHS rating): " + NameList (unknown) + ".", unknownIds });
    }
    for (size_t i = 0; i < species.size (); ++i) {
        if (!plants[i])
            continue;
        const std::string name = plants[i]->DisplayName ();
        const std::string roomText = space ? "room for its crown, clear of walls and overhangs" : "room for its crown";
        if (result.wanted[i] > 0 && result.placed[i] < result.wanted[i]) {
            char text[96];
            std::snprintf (text, sizeof (text), "Only %d of %d fit", result.placed[i], result.wanted[i]);
            result.notes.push_back ({ SchemeSeverity::Warning,
                                      std::string (text) + ": " + name + " (" +
                                          (plants[i]->HasLight () ? "accepted light and " : "") + roomText + ").",
                                      { int (i) } });
        }
        else if (result.wanted[i] == 0 && result.placed[i] == 0)
            result.notes.push_back (
                { SchemeSeverity::Warning, name + ": no " + roomText + " on a light class it accepts.", { int (i) } });
        if (i < result.room.size () && result.room[i].outside > 0 && result.room[i].lightSamples > 0) {
            char text[200];
            std::snprintf (text, sizeof (text),
                           ": its %.1f m wide crown would cross the region's edge from %.0f%% of the area with its "
                           "light; it is kept inside.",
                           2.0 * result.sizes[i].radius,
                           100.0 * double (result.room[i].outside) / double (result.room[i].lightSamples));
            result.notes.push_back ({ SchemeSeverity::Info, name + text, { int (i) } });
        }
        if (i < result.room.size () && result.room[i].noGo > 0) {
            char text[160];
            std::snprintf (text, sizeof (text), ": no-go zones keep it out of %.0f%% of the area with its light.",
                           100.0 * double (result.room[i].noGo) /
                               double ((std::max) (result.room[i].lightSamples, size_t (1))));
            result.notes.push_back ({ SchemeSeverity::Info, name + text, { int (i) } });
        }
        if (space) {
            const SchemeRoom& room = result.room[i];
            if (room.overhead + room.narrow > 0) {
                char text[220];
                const double share = 100.0 / double ((std::max) (room.lightSamples, size_t (1)));
                std::snprintf (text, sizeof (text),
                               ": a %.1f m tall, %.1f m wide crown has no room on %.0f%% of the area with its light "
                               "(%.0f%% under an overhang, %.0f%% too close to a wall).",
                               result.sizes[i].height, 2.0 * result.sizes[i].radius,
                               share * double (room.overhead + room.narrow), share * double (room.overhead),
                               share * double (room.narrow));
                result.notes.push_back ({ SchemeSeverity::Info, name + text, { int (i) } });
            }
            if (room.beyond > 0)
                result.notes.push_back ({ SchemeSeverity::Info,
                                          name + ": its crown is wider than the " +
                                              std::to_string (int (std::lround (site.clearance.reach))) +
                                              " m measured around each sample; room beyond that is not known.",
                                          { int (i) } });
        }
        if (!plants[i]->HasLight () && result.placed[i] > 0)
            result.notes.push_back (
                { SchemeSeverity::Info, name + " has no light classes: placed without a light check.", { int (i) } });
        if (result.sizes[i].radiusFrom == "assumed")
            result.notes.push_back ({ SchemeSeverity::Info,
                                      name + ": no spread or density in the catalogue; spaced at an assumed "
                                             "0.25 m crown radius.",
                                      { int (i) } });
    }
    if (capped)
        result.notes.push_back (
            { SchemeSeverity::Warning,
              "Stopped at " + std::to_string (options.maxPlants) + " plants; set counts to place fewer.",
              {} });
    if (n == 0)
        result.notes.push_back ({ SchemeSeverity::Warning, "No measured samples: run a layer first.", {} });
    else if (!space && !result.plants.empty ())
        result.notes.push_back (
            { SchemeSeverity::Info, "Walls and overhangs were not checked: crowns may run into the building.", {} });
    return result;
}

} // namespace geomsrv::archviz::planting
