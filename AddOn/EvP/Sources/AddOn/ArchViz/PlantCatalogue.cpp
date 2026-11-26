#include "ArchViz/PlantCatalogue.hpp"

#include "ArchViz/PlantingHudModel.hpp"
#include "NodeGraph/Json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

namespace geomsrv::archviz::planting {

namespace json = evp::nodegraph::json;

namespace {

constexpr const char* kLightKeys[kLightClassCount] = { "shade", "partial", "full" };
constexpr const char* kLightNames[kLightClassCount] = { "Shade", "Partial shade", "Full sun" };
constexpr const char* kHabitKeys[kHabitCount] = { "",     "groundcover", "perennial", "grass", "fern",
                                                  "bulb", "annual",      "climber",   "shrub", "tree" };
constexpr const char* kHabitNames[kHabitCount] = { "Unknown", "Ground cover", "Perennial", "Grass", "Fern",
                                                   "Bulb",    "Annual",       "Climber",   "Shrub", "Tree" };

// Collects problems under a record's path.
struct Reader {
    ParseProblems& problems;
    std::string where;

    void Fail (const std::string& field, const std::string& what) const
    {
        if (problems.items.size () < 50)
            problems.items.push_back (where + (field.empty () ? "" : field + ": ") + what);
    }

    bool String (const json::JsonValue& obj, const char* key, std::string& out, bool required) const
    {
        const json::JsonValue* v = obj.Find (key);
        if (!v || v->IsNull ()) {
            if (required)
                Fail (key, "is required");
            return !required;
        }
        if (!v->AsString (out)) {
            Fail (key, "must be text");
            return false;
        }
        if (required && out.empty ()) {
            Fail (key, "must not be empty");
            return false;
        }
        return true;
    }

    bool Number (const json::JsonValue& v, const std::string& field, double& out) const
    {
        if (!v.AsDouble (out) || !std::isfinite (out)) {
            Fail (field, "must be a number");
            return false;
        }
        return true;
    }

    // null, absent, or {min, max} with 0 <= min <= max.
    void OptionalRange (const json::JsonValue& obj, const char* key, std::optional<Range>& out) const
    {
        const json::JsonValue* v = obj.Find (key);
        if (!v || v->IsNull ())
            return;
        const json::JsonValue* lo = v->Find ("min");
        const json::JsonValue* hi = v->Find ("max");
        if (!v->AsObject () || !lo || !hi) {
            Fail (key, "must be null or {\"min\": .., \"max\": ..}");
            return;
        }
        Range r;
        if (!Number (*lo, std::string (key) + ".min", r.min) || !Number (*hi, std::string (key) + ".max", r.max))
            return;
        if (r.min < 0 || r.max < r.min) {
            Fail (key, "needs 0 <= min <= max");
            return;
        }
        out = r;
    }

    void StringList (const json::JsonValue& obj, const char* key, std::vector<std::string>& out) const
    {
        const json::JsonValue* v = obj.Find (key);
        if (!v || v->IsNull ())
            return;
        const json::JsonArray* items = v->AsArray ();
        if (!items) {
            Fail (key, "must be a list");
            return;
        }
        for (size_t i = 0; i < items->size (); ++i) {
            std::string s;
            if (!(*items)[i].AsString (s))
                Fail (std::string (key) + "[" + std::to_string (i) + "]", "must be text");
            else
                out.push_back (s);
        }
    }
};

std::string LineColumn (const std::string& text, size_t offset)
{
    size_t line = 1, column = 1;
    for (size_t i = 0; i < offset && i < text.size (); ++i) {
        if (text[i] == '\n') {
            ++line;
            column = 1;
        }
        else {
            ++column;
        }
    }
    return "line " + std::to_string (line) + ", column " + std::to_string (column);
}

// The root object at the supported schema version, or nullptr with a problem.
const json::JsonValue* Root (const std::string& text, json::ParseResult& parsed, ParseProblems& problems)
{
    parsed = json::Parse (text);
    if (!parsed.ok) {
        problems.items.push_back (LineColumn (text, parsed.offset) + ": " + parsed.error);
        return nullptr;
    }
    if (!parsed.value.AsObject ()) {
        problems.items.push_back ("the file must hold one JSON object");
        return nullptr;
    }
    const json::JsonValue* version = parsed.value.Find ("schemaVersion");
    int64_t v = 0;
    if (!version || !version->AsInteger (v)) {
        problems.items.push_back ("schemaVersion is required (1)");
        return nullptr;
    }
    if (v != kSchemaVersion) {
        problems.items.push_back ("schemaVersion " + std::to_string (v) + " is not supported (1)");
        return nullptr;
    }
    return &parsed.value;
}

bool ParseMonthDay (const std::string& text, int& month, int& day)
{
    const size_t slash = text.find ('/');
    if (slash == std::string::npos)
        return false;
    try {
        month = std::stoi (text.substr (0, slash));
        day = std::stoi (text.substr (slash + 1));
    }
    catch (...) {
        return false;
    }
    static constexpr int kDays[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return month >= 1 && month <= 12 && day >= 1 && day <= kDays[month - 1];
}

void ParsePlant (const json::JsonValue& item, const Reader& r, const std::set<std::string>& sourceIds, Plant& p)
{
    r.String (item, "scientificName", p.scientificName, true);
    r.String (item, "cultivar", p.cultivar, false);
    if (const json::JsonValue* names = item.Find ("localNames"); names && !names->IsNull ()) {
        if (const json::JsonObject* obj = names->AsObject ()) {
            for (const auto& [lang, value] : *obj) {
                std::string s;
                if (value.AsString (s))
                    p.localNames[lang] = s;
                else if (!value.IsNull ())
                    r.Fail ("localNames." + lang, "must be text");
            }
        }
        else {
            r.Fail ("localNames", "must be an object of language: name");
        }
    }
    std::string habit;
    if (r.String (item, "habit", habit, false) && !habit.empty ()) {
        const auto* end = std::end (kHabitKeys);
        const auto* found = std::find (std::begin (kHabitKeys) + 1, end, habit);
        if (found == end)
            r.Fail ("habit", "'" + habit +
                                 "' is not one of groundcover, perennial, grass, fern, bulb, annual, "
                                 "climber, shrub, tree");
        else
            p.habit = Habit (found - std::begin (kHabitKeys));
    }
    if (const json::JsonValue* light = item.Find ("light"); light && !light->IsNull ()) {
        std::vector<std::string> classes;
        Reader inner { r.problems, r.where + "light." };
        inner.StringList (*light, "acceptedClasses", classes);
        for (size_t i = 0; i < classes.size (); ++i) {
            const auto* found = std::find (std::begin (kLightKeys), std::end (kLightKeys), classes[i]);
            if (found == std::end (kLightKeys))
                inner.Fail ("acceptedClasses[" + std::to_string (i) + "]",
                            "'" + classes[i] + "' is not shade, partial or full");
            else
                p.light[size_t (found - std::begin (kLightKeys))] = true;
        }
    }
    r.OptionalRange (item, "matureHeightM", p.matureHeightM);
    r.OptionalRange (item, "matureSpreadM", p.matureSpreadM);
    if (const json::JsonValue* h = item.Find ("preferredReceiverHeightM"); h && !h->IsNull ()) {
        double v = 0;
        if (r.Number (*h, "preferredReceiverHeightM", v)) {
            if (v <= 0 || v > kMaxHeight)
                r.Fail ("preferredReceiverHeightM", "must be above 0 and at most 100 m");
            else
                p.receiverHeightM = v;
        }
    }
    r.StringList (item, "moistureClasses", p.moistureClasses);
    r.StringList (item, "plantingLayers", p.plantingLayers);
    if (const json::JsonValue* months = item.Find ("floweringMonths"); months && !months->IsNull ()) {
        const json::JsonArray* list = months->AsArray ();
        for (size_t i = 0; list && i < list->size (); ++i) {
            int64_t m = 0;
            if (!(*list)[i].AsInteger (m) || m < 1 || m > 12)
                r.Fail ("floweringMonths[" + std::to_string (i) + "]", "must be a month 1-12");
            else
                p.floweringMonths.push_back (int (m));
        }
        if (!list)
            r.Fail ("floweringMonths", "must be a list");
    }
    if (const json::JsonValue* d = item.Find ("deciduous"); d && !d->IsNull ()) {
        bool b = false;
        if (d->AsBool (b))
            p.deciduous = b;
        else
            r.Fail ("deciduous", "must be true, false or null");
    }
    if (const json::JsonValue* refs = item.Find ("sourceRefs"); refs && !refs->IsNull ()) {
        const json::JsonArray* list = refs->AsArray ();
        for (size_t i = 0; list && i < list->size (); ++i) {
            const std::string at = "sourceRefs[" + std::to_string (i) + "]";
            PlantSourceRef ref;
            const json::JsonValue& one = (*list)[i];
            const json::JsonValue* sid = one.Find ("sourceId");
            if (!sid || !sid->AsString (ref.sourceId) || !sourceIds.count (ref.sourceId)) {
                r.Fail (at, "sourceId must name an entry of sources");
                continue;
            }
            int64_t page = 0;
            if (const json::JsonValue* pg = one.Find ("page"); pg && !pg->IsNull () && pg->AsInteger (page))
                ref.page = int (page);
            p.sources.push_back (ref);
        }
        if (!list)
            r.Fail ("sourceRefs", "must be a list");
    }
}

} // namespace

const char* LightKey (LightClass c)
{
    return kLightKeys[size_t (c)];
}
const char* LightName (LightClass c)
{
    return kLightNames[size_t (c)];
}
const char* HabitKey (Habit h)
{
    return kHabitKeys[size_t (h)];
}
const char* HabitName (Habit h)
{
    return kHabitNames[size_t (h)];
}

std::string Plant::DisplayName () const
{
    return cultivar.empty () ? scientificName : scientificName + " '" + cultivar + "'";
}

bool ParseCatalogue (const std::string& text, Catalogue& out, ParseProblems& problems)
{
    problems = {};
    json::ParseResult parsed;
    const json::JsonValue* root = Root (text, parsed, problems);
    if (!root)
        return false;
    Catalogue result;
    const Reader top { problems, "" };
    top.String (*root, "catalogueId", result.catalogueId, true);
    top.String (*root, "locale", result.locale, false);
    std::set<std::string> sourceIds;
    if (const json::JsonValue* sources = root->Find ("sources"); sources && !sources->IsNull ()) {
        const json::JsonArray* list = sources->AsArray ();
        if (!list)
            top.Fail ("sources", "must be a list");
        for (size_t i = 0; list && i < list->size (); ++i) {
            const Reader r { problems, "sources[" + std::to_string (i) + "] " };
            CatalogueSource s;
            const json::JsonValue& one = (*list)[i];
            if (!r.String (one, "id", s.id, true))
                continue;
            r.String (one, "title", s.title, false);
            r.String (one, "note", s.note, false);
            r.String (one, "url", s.url, false);
            int64_t year = 0;
            if (const json::JsonValue* y = one.Find ("year"); y && !y->IsNull () && y->AsInteger (year))
                s.year = int (year);
            if (!sourceIds.insert (s.id).second)
                r.Fail ("id", "'" + s.id + "' is used twice");
            result.sources.push_back (std::move (s));
        }
    }
    const json::JsonValue* plants = root->Find ("plants");
    const json::JsonArray* list = plants ? plants->AsArray () : nullptr;
    if (!list)
        top.Fail ("plants", "is required and must be a list");
    std::set<std::string> ids;
    for (size_t i = 0; list && i < list->size (); ++i) {
        const json::JsonValue& item = (*list)[i];
        Plant p;
        const Reader head { problems, "plants[" + std::to_string (i) + "] " };
        if (!item.AsObject ()) {
            head.Fail ("", "must be an object");
            continue;
        }
        if (!head.String (item, "id", p.id, true))
            continue;
        const Reader r { problems, "plants[" + std::to_string (i) + "] '" + p.id + "': " };
        if (!ids.insert (p.id).second)
            r.Fail ("id", "is used twice");
        ParsePlant (item, r, sourceIds, p);
        result.plants.push_back (std::move (p));
    }
    if (!problems.Empty ())
        return false;
    out = std::move (result);
    return true;
}

bool ParseProfiles (const std::string& text, std::vector<Profile>& out, ParseProblems& problems)
{
    problems = {};
    json::ParseResult parsed;
    const json::JsonValue* root = Root (text, parsed, problems);
    if (!root)
        return false;
    const json::JsonValue* profiles = root->Find ("profiles");
    const json::JsonArray* list = profiles ? profiles->AsArray () : nullptr;
    if (!list || list->empty ()) {
        problems.items.push_back ("profiles is required and must list at least one profile");
        return false;
    }
    std::vector<Profile> result;
    std::set<std::string> ids;
    for (size_t i = 0; i < list->size (); ++i) {
        const json::JsonValue& item = (*list)[i];
        Profile p;
        const Reader head { problems, "profiles[" + std::to_string (i) + "] " };
        if (!head.String (item, "id", p.id, true))
            continue;
        const Reader r { problems, "profiles[" + std::to_string (i) + "] '" + p.id + "': " };
        if (!ids.insert (p.id).second)
            r.Fail ("id", "is used twice");
        r.String (item, "name", p.name, true);
        r.String (item, "source", p.source, false);
        r.String (item, "edition", p.edition, false);
        const json::JsonValue* edges = item.Find ("classEdgesH");
        const json::JsonValue* partial = edges ? edges->Find ("partial") : nullptr;
        const json::JsonValue* full = edges ? edges->Find ("full") : nullptr;
        double a = 0, b = 0;
        if (!partial || !full) {
            r.Fail ("classEdgesH", "needs partial and full, in hours");
        }
        else if (r.Number (*partial, "classEdgesH.partial", a) && r.Number (*full, "classEdgesH.full", b)) {
            if (a <= 0 || b <= a || b > 24)
                r.Fail ("classEdgesH", "needs 0 < partial < full <= 24");
            p.partialEdgeH = float (a);
            p.fullEdgeH = float (b);
        }
        if (const json::JsonValue* deep = edges ? edges->Find ("deepShade") : nullptr; deep && !deep->IsNull ()) {
            double d = 0;
            if (r.Number (*deep, "classEdgesH.deepShade", d)) {
                if (d <= 0 || d >= a)
                    r.Fail ("classEdgesH.deepShade", "needs 0 < deepShade < partial");
                p.deepShadeEdgeH = float (d);
            }
        }
        if (const json::JsonValue* season = item.Find ("season"); season && !season->IsNull ()) {
            std::string from, to;
            const Reader s { problems, r.where + "season." };
            if (s.String (*season, "from", from, true) && s.String (*season, "to", to, true)) {
                if (!ParseMonthDay (from, p.fromMonth, p.fromDay) || !ParseMonthDay (to, p.toMonth, p.toDay))
                    s.Fail ("", "from and to must be month/day, e.g. \"4/28\"");
                else
                    p.hasSeason = true;
            }
            s.String (*season, "note", p.seasonNote, false);
        }
        if (const json::JsonValue* heights = item.Find ("receiverHeightsM"); heights && !heights->IsNull ()) {
            const json::JsonArray* hs = heights->AsArray ();
            for (size_t k = 0; hs && k < hs->size (); ++k) {
                double h = 0;
                const std::string at = "receiverHeightsM[" + std::to_string (k) + "]";
                if (r.Number ((*hs)[k], at, h)) {
                    if (h <= 0 || h > kMaxHeight)
                        r.Fail (at, "must be above 0 and at most 100 m");
                    else
                        p.receiverHeightsM.push_back (float (h));
                }
            }
            if (!hs || p.receiverHeightsM.size () > size_t (kHeightCount))
                r.Fail ("receiverHeightsM", "must list at most 4 heights");
        }
        result.push_back (std::move (p));
    }
    if (!problems.Empty ())
        return false;
    out = std::move (result);
    return true;
}

float ReceiverHeightOf (const Plant& plant)
{
    if (plant.receiverHeightM)
        return float (*plant.receiverHeightM);
    switch (plant.habit) {
        case Habit::Groundcover:
            return kDefaultHeights[0];
        case Habit::Shrub:
        case Habit::Climber:
            return kDefaultHeights[2];
        case Habit::Tree:
            return kDefaultHeights[3];
        default:
            return kDefaultHeights[1];
    }
}

void ApplyProfile (PlantingHud& hud, const Profile& profile)
{
    hud.partialEdge = profile.partialEdgeH;
    hud.sunEdge = profile.fullEdgeH;
    hud.deepShade = profile.deepShadeEdgeH.has_value ();
    if (profile.deepShadeEdgeH)
        hud.deepShadeEdge = *profile.deepShadeEdgeH;
    if (profile.hasSeason) {
        hud.season = kCustom;
        hud.custom = { profile.fromMonth, profile.fromDay, profile.toMonth, profile.toDay, false, false };
    }
    if (!profile.receiverHeightsM.empty ()) {
        for (int slot = 0; slot < kHeightCount; ++slot) {
            const bool given = size_t (slot) < profile.receiverHeightsM.size ();
            hud.heightOn[slot] = given;
            if (given)
                hud.heights[slot] = profile.receiverHeightsM[size_t (slot)];
        }
    }
    ClampEdges (hud);
    hud.profileId = profile.id;
}

namespace {

LightClass LightOfClass (const PlantingHud& hud, int cls)
{
    const int shift = hud.deepShade ? 1 : 0; // deep shade counts as shade
    const int c = std::max (0, cls - shift);
    return c == 0 ? LightClass::Shade : c == 1 ? LightClass::Partial : LightClass::Full;
}

} // namespace

uint32_t AcceptedClassMask (const PlantingHud& hud, const Plant& plant)
{
    uint32_t mask = 0;
    for (int cls = 0; cls < ClassCount (hud); ++cls)
        if (plant.light[size_t (LightOfClass (hud, cls))])
            mask |= 1u << cls;
    return mask;
}

PlantFit FitOf (const PlantingHud& hud, const Plant& plant)
{
    PlantFit fit;
    fit.wantedHeight = ReceiverHeightOf (plant);
    if (!plant.HasLight ()) {
        fit.reason = "no light classes in the catalogue";
        return fit;
    }
    const std::vector<double> edges = AreaEdges (hud);
    float best = 1e9f;
    for (size_t i = 0; i < hud.layers.size (); ++i) {
        const Layer& layer = hud.layers[i];
        if (layer.classAreas.empty () || layer.areaEdges != edges)
            continue;
        const float gap = std::abs (layer.height - fit.wantedHeight);
        if (gap < best) {
            best = gap;
            fit.layer = int (i);
        }
    }
    if (fit.layer < 0) {
        fit.reason = hud.layers.empty () ? "no layer measured yet" : "no layer read at these class edges";
        return fit;
    }
    const Layer& layer = hud.layers[size_t (fit.layer)];
    fit.layerHeight = layer.height;
    fit.evaluatedArea = layer.evaluatedArea;
    const uint32_t mask = AcceptedClassMask (hud, plant);
    for (size_t cls = 0; cls < layer.classAreas.size (); ++cls)
        if (mask & (1u << cls))
            fit.compatibleArea += layer.classAreas[cls];
    fit.status = fit.compatibleArea > 0 ? PlantFit::Status::Fits : PlantFit::Status::None;
    return fit;
}

namespace {

std::string Lower (std::string s)
{
    for (char& c : s)
        c = char (std::tolower (static_cast<unsigned char> (c)));
    return s;
}

} // namespace

bool Matches (const Plant& plant, const PlantFilter& filter)
{
    if (filter.habit >= 0 && int (plant.habit) != filter.habit)
        return false;
    if (filter.light >= 0 && (filter.light >= kLightClassCount || !plant.light[size_t (filter.light)]))
        return false;
    if (filter.text.empty ())
        return true;
    const std::string needle = Lower (filter.text);
    auto has = [&] (const std::string& hay) { return Lower (hay).find (needle) != std::string::npos; };
    if (has (plant.scientificName) || has (plant.cultivar) || has (plant.id))
        return true;
    return std::any_of (plant.localNames.begin (), plant.localNames.end (),
                        [&] (const auto& kv) { return has (kv.second); });
}

namespace {

#include "ArchViz/PlantCatalogueSeed.inc"

template <size_t N> std::string Join (const char* const (&pieces)[N])
{
    std::string text;
    for (const char* piece : pieces)
        text += piece;
    return text;
}

} // namespace

// The seed is PlantCatalogueSeed.plants.json / .profiles.json, tracked beside
// this file and compiled in through tools/generate_plant_catalogue_seed.py.
const std::string& SeedPlantsJson ()
{
    static const std::string text = Join (kSeedPlantsPieces);
    return text;
}

const std::string& SeedProfilesJson ()
{
    static const std::string text = Join (kSeedProfilesPieces);
    return text;
}

} // namespace geomsrv::archviz::planting
