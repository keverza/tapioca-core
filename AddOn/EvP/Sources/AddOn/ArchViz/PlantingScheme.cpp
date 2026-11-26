#include "ArchViz/PlantingScheme.hpp"

#include "ArchViz/PlantingHudModel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <numeric>
#include <set>

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

SchemeSite RestrictToZone (const SchemeSite& site, const PlantingZone& zone)
{
    SchemeSite out;
    out.layerHeight = site.layerHeight;
    std::vector<size_t> keep;
    for (size_t s = 0; s < site.Size (); ++s) {
        if (!zone.Contains (site.x[s], site.y[s]))
            continue;
        keep.push_back (s);
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
        s.radius = std::sqrt (1.0 / (kPi * plant.densityPerM2->min));
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

    // Competition: aggressive or high-vigour beside slow or low-vigour plants.
    for (size_t i = 0; i < plants.size (); ++i) {
        if (!plants[i])
            continue;
        const char* vigour = plants[i]->traits.ChoiceKey (Trait::CompetitiveVigour);
        const bool strong =
            plants[i]->growthIntensity == GrowthIntensity::Aggressive || (vigour && std::string (vigour) == "high");
        if (!strong)
            continue;
        std::vector<std::string> weak;
        std::vector<int> who { int (i) };
        for (size_t k = 0; k < plants.size (); ++k) {
            if (k == i || !plants[k])
                continue;
            const char* v = plants[k]->traits.ChoiceKey (Trait::CompetitiveVigour);
            if (plants[k]->growthIntensity == GrowthIntensity::Slow || (v && std::string (v) == "low")) {
                weak.push_back (name (k));
                who.push_back (int (k));
            }
        }
        if (!weak.empty ())
            notes.push_back ({ SchemeSeverity::Warning,
                               name (i) + " spreads strongly and may suppress " + NameList (weak) +
                                   "; keep them apart or plan for division.",
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
    if (space || noGo)
        result.room.resize (species.size ());

    // gap[s]: distance from sample s to the nearest placed crown's edge.
    std::vector<double> gap (n, std::numeric_limits<double>::infinity ());
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
            if (!space && !noGo) {
                ok[s] = 1;
                continue;
            }
            SchemeRoom& room = result.room[i];
            ++room.lightSamples;
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
        // The first plant of an empty site goes nearest the usable samples' centre.
        double cx = 0, cy = 0;
        size_t accepted = 0;
        for (size_t s = 0; s < n; ++s)
            if (ok[s]) {
                cx += site.x[s];
                cy += site.y[s];
                ++accepted;
            }
        if (accepted > 0) {
            cx /= double (accepted);
            cy /= double (accepted);
        }
        for (int k = 0; k < wanted; ++k) {
            if (int (result.plants.size ()) >= options.maxPlants) {
                capped = true;
                break;
            }
            size_t best = n;
            double bestScore = -std::numeric_limits<double>::infinity ();
            for (size_t s = 0; s < n; ++s) {
                if (!ok[s] || gap[s] < need)
                    continue;
                const double score = std::isinf (gap[s]) ? -std::hypot (site.x[s] - cx, site.y[s] - cy) : gap[s];
                if (score > bestScore) {
                    bestScore = score;
                    best = s;
                }
            }
            if (best == n)
                break;
            PlacedPlant p;
            p.species = int (i);
            p.x = site.x[best];
            p.y = site.y[best];
            p.z = site.groundZ[best];
            p.radius = r;
            p.height = result.sizes[i].height;
            p.cls = site.cls[best];
            p.lightChecked = plant.HasLight ();
            p.spaceChecked = space;
            result.plants.push_back (p);
            ++result.placed[i];
            for (size_t s = 0; s < n; ++s)
                gap[s] = std::min (gap[s], std::hypot (site.x[s] - p.x, site.y[s] - p.y) - r);
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
        if (i < result.room.size () && result.room[i].noGo > 0) {
            char text[160];
            std::snprintf (text, sizeof (text), ": %zu of %zu samples on its light are kept out by no-go zones.",
                           result.room[i].noGo, result.room[i].lightSamples);
            result.notes.push_back ({ SchemeSeverity::Info, name + text, { int (i) } });
        }
        if (space) {
            const SchemeRoom& room = result.room[i];
            if (room.overhead + room.narrow > 0) {
                char text[220];
                std::snprintf (text, sizeof (text),
                               ": %zu of %zu samples on its light lack room for a %.1f m tall, %.1f m wide crown "
                               "(%zu under an overhang, %zu too close to a wall).",
                               room.overhead + room.narrow, room.lightSamples, result.sizes[i].height,
                               2.0 * result.sizes[i].radius, room.overhead, room.narrow);
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
