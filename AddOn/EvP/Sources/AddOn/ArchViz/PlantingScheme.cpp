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

SchemeSite RestrictToZone (const SchemeSite& site, const PlantingZone& zone)
{
    SchemeSite out;
    out.layerHeight = site.layerHeight;
    for (size_t s = 0; s < site.Size (); ++s) {
        if (!zone.Contains (site.x[s], site.y[s]))
            continue;
        out.x.push_back (site.x[s]);
        out.y.push_back (site.y[s]);
        out.groundZ.push_back (site.groundZ[s]);
        out.cls.push_back (site.cls[s]);
        out.host.push_back (site.host[s]);
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
        // The first plant of an empty site goes nearest the accepted samples' centre.
        double cx = 0, cy = 0;
        size_t accepted = 0;
        for (size_t s = 0; s < n; ++s)
            if (site.cls[s] >= 0 && (mask >> site.cls[s]) & 1u) {
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
                if (site.cls[s] < 0 || !((mask >> site.cls[s]) & 1u) || gap[s] < need)
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
        if (result.wanted[i] > 0 && result.placed[i] < result.wanted[i]) {
            char text[96];
            std::snprintf (text, sizeof (text), "Only %d of %d fit", result.placed[i], result.wanted[i]);
            result.notes.push_back (
                { SchemeSeverity::Warning,
                  std::string (text) + ": " + name +
                      (plants[i]->HasLight () ? " (accepted light and room for its crown)." : " (room for its crown)."),
                  { int (i) } });
        }
        else if (result.wanted[i] == 0 && result.placed[i] == 0)
            result.notes.push_back (
                { SchemeSeverity::Warning, name + ": no room on a light class it accepts.", { int (i) } });
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
    return result;
}

} // namespace geomsrv::archviz::planting
