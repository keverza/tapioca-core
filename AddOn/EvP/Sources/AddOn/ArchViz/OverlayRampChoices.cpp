// ArchViz/OverlayRampChoices -- see the header.

#include "ArchViz/OverlayRampChoices.hpp"

#include <algorithm>
#include <map>
#include <span>

namespace geomsrv::archviz::overlaylayers::rampchoices {

namespace {

struct PresetChoice {
    std::string to;                // a library id, or kCustomGradientId
    std::vector<ColourStop> stops; // the custom stops
    bool flipped = false;
};
// Per layer: the preset a script authored -> the one the user chose.
std::map<std::string, std::map<std::string, PresetChoice>> g_choices; // MAIN THREAD

// Every colormap in a layer, the panel items' included.
template <typename Visit> void EachColormap (Layer& layer, Visit visit)
{
    for (auto& mesh : layer.meshes)
        visit (mesh.colormap);
    for (auto& legend : layer.legends)
        visit (legend.colormap);
    for (auto& panel : layer.panels)
        for (auto& item : panel.items)
            visit (item.colormap);
}

bool SameStops (const std::vector<ColourStop>& a, std::span<const ColourStop> b)
{
    return a.size () == b.size () && std::equal (a.begin (), a.end (), b.begin (), [] (const auto& x, const auto& y) {
               return x.at == y.at && x.rgba == y.rgba;
           });
}

// `to`'s stops: the library's, or the custom ones, end for end when flipped; empty when
// they cannot be used.
std::vector<ColourStop> ChoiceStops (const PresetChoice& choice)
{
    std::span<const ColourStop> stops;
    if (choice.to == kCustomGradientId) {
        if (choice.stops.size () >= 2 && choice.stops.size () <= kMaxStops)
            stops = choice.stops;
    }
    else if (const GradientPreset* preset = FindGradientPreset (choice.to))
        stops = preset->Stops ();
    if (stops.empty ())
        return {};
    return choice.flipped ? ReversedStops (stops) : std::vector<ColourStop> (stops.begin (), stops.end ());
}

bool RecolourMap (Colormap& map, const std::string& from, const PresetChoice& to)
{
    const auto stops = ChoiceStops (to);
    if (stops.empty () || map.presetId.empty () || map.presetId != from ||
        (map.presetId == to.to && map.flipped == to.flipped && SameStops (map.stops, stops)))
        return false;
    map.stops = stops;
    map.presetId = to.to;
    map.flipped = to.flipped;
    return true;
}

} // namespace

void Apply (Layer& layer)
{
    const auto choices = g_choices.find (layer.name);
    if (choices == g_choices.end ())
        return;
    EachColormap (layer, [&] (Colormap& map) {
        const auto chosen = choices->second.find (map.presetId);
        if (chosen != choices->second.end ())
            RecolourMap (map, map.presetId, chosen->second);
    });
}

bool Recolour (Layer& layer, const std::string& from, const std::string& to, const std::vector<ColourStop>& custom,
               bool flipped)
{
    const PresetChoice choice { to, to == kCustomGradientId ? custom : std::vector<ColourStop> (), flipped };
    if (ChoiceStops (choice).empty () || from.empty ())
        return false;
    bool changed = false;
    EachColormap (layer, [&] (Colormap& map) { changed = RecolourMap (map, from, choice) || changed; });
    if (!changed)
        return false;
    // Remembered against what the SCRIPT wrote: a choice made over an earlier choice
    // replaces it, and choosing the script's own preset again, unflipped, forgets it.
    std::map<std::string, PresetChoice>& choices = g_choices[layer.name];
    bool chained = false;
    for (auto it = choices.begin (); it != choices.end ();) {
        if (it->second.to == from) {
            chained = true;
            if (it->first == to && !flipped && to != kCustomGradientId) {
                it = choices.erase (it);
                continue;
            }
            it->second = choice;
        }
        ++it;
    }
    if (!chained)
        choices[from] = choice;
    if (choices.empty ())
        g_choices.erase (layer.name);
    return true;
}

void Forget ()
{
    g_choices.clear ();
}

} // namespace geomsrv::archviz::overlaylayers::rampchoices
