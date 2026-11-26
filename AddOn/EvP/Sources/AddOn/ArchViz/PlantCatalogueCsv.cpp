#include "ArchViz/PlantCatalogueCsv.hpp"

#include "NodeGraph/Json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>

namespace geomsrv::archviz::planting {

namespace json = evp::nodegraph::json;

namespace {

constexpr const char* kBom = "\xEF\xBB\xBF";

std::string Quote (const std::string& field)
{
    if (field.find_first_of (",\"\r\n") == std::string::npos)
        return field;
    std::string out = "\"";
    for (char c : field)
        out += c == '"' ? std::string ("\"\"") : std::string (1, c);
    return out + "\"";
}

std::string Number (double v)
{
    char text[32];
    std::snprintf (text, sizeof (text), "%g", v);
    return text;
}

std::string Join (const std::vector<std::string>& items)
{
    std::string out;
    for (const auto& item : items)
        out += (out.empty () ? "" : "|") + item;
    return out;
}

std::vector<std::string> Split (const std::string& text)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size ()) {
        const size_t bar = text.find ('|', start);
        std::string item = text.substr (start, bar == std::string::npos ? std::string::npos : bar - start);
        item.erase (0, item.find_first_not_of (" \t"));
        item.erase (item.find_last_not_of (" \t") + 1);
        if (!item.empty ())
            out.push_back (item);
        if (bar == std::string::npos)
            break;
        start = bar + 1;
    }
    return out;
}

std::string Trim (std::string s)
{
    s.erase (0, s.find_first_not_of (" \t"));
    s.erase (s.find_last_not_of (" \t") + 1);
    return s;
}

json::JsonValue RangeJson (const std::optional<Range>& r)
{
    if (!r)
        return {};
    return json::JsonValue::Object (
        { { "min", json::JsonValue::Double (r->min) }, { "max", json::JsonValue::Double (r->max) } });
}

json::JsonValue StringsJson (const std::vector<std::string>& items)
{
    json::JsonArray array;
    for (const auto& item : items)
        array.push_back (json::JsonValue::String (item));
    return json::JsonValue::Array (std::move (array));
}

} // namespace

const std::vector<std::string>& CatalogueCsvColumns ()
{
    static const std::vector<std::string> columns = {
        "id",
        "scientificName",
        "cultivar",
        "name_lt",
        "name_en",
        "habit",
        "light",
        "matureHeightMinM",
        "matureHeightMaxM",
        "matureSpreadMinM",
        "matureSpreadMaxM",
        "receiverHeightM",
        "densityMinPerM2",
        "densityMaxPerM2",
        "floweringMonths",
        "deciduous",
        "moistureClasses",
        "plantingLayers",
        "source",
        "page",
    };
    return columns;
}

std::string CatalogueToCsv (const Catalogue& catalogue)
{
    std::string out = kBom;
    for (size_t i = 0; i < CatalogueCsvColumns ().size (); ++i)
        out += (i ? "," : "") + CatalogueCsvColumns ()[i];
    out += "\r\n";
    for (const Plant& p : catalogue.plants) {
        std::vector<std::string> light;
        for (int c = 0; c < kLightClassCount; ++c)
            if (p.light[size_t (c)])
                light.push_back (LightKey (LightClass (c)));
        std::vector<std::string> months;
        for (int m : p.floweringMonths)
            months.push_back (std::to_string (m));
        auto name = [&] (const char* lang) {
            const auto it = p.localNames.find (lang);
            return it == p.localNames.end () ? std::string () : it->second;
        };
        auto lo = [] (const std::optional<Range>& r) { return r ? Number (r->min) : std::string (); };
        auto hi = [] (const std::optional<Range>& r) { return r ? Number (r->max) : std::string (); };
        const std::vector<std::string> fields = {
            p.id,
            p.scientificName,
            p.cultivar,
            name ("lt"),
            name ("en"),
            HabitKey (p.habit),
            Join (light),
            lo (p.matureHeightM),
            hi (p.matureHeightM),
            lo (p.matureSpreadM),
            hi (p.matureSpreadM),
            p.receiverHeightM ? Number (*p.receiverHeightM) : "",
            lo (p.densityPerM2),
            hi (p.densityPerM2),
            Join (months),
            p.deciduous ? (*p.deciduous ? "true" : "false") : "",
            Join (p.moistureClasses),
            Join (p.plantingLayers),
            p.sources.empty () ? "" : p.sources[0].sourceId,
            p.sources.empty () || !p.sources[0].page ? "" : std::to_string (p.sources[0].page),
        };
        for (size_t i = 0; i < fields.size (); ++i)
            out += (i ? "," : "") + Quote (fields[i]);
        out += "\r\n";
    }
    return out;
}

bool ParseCsv (const std::string& input, std::vector<std::vector<std::string>>& rows, std::string& error)
{
    rows.clear ();
    std::string text = input;
    if (text.compare (0, 3, kBom) == 0)
        text.erase (0, 3);
    std::vector<std::string> row;
    std::string field;
    bool quoted = false, any = false;
    size_t line = 1, quoteLine = 0;
    for (size_t i = 0; i < text.size (); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"' && i + 1 < text.size () && text[i + 1] == '"') {
                field += '"';
                ++i;
            }
            else if (c == '"') {
                quoted = false;
            }
            else {
                if (c == '\n')
                    ++line;
                field += c;
            }
            continue;
        }
        if (c == '"') {
            quoted = true;
            quoteLine = line;
            any = true;
        }
        else if (c == ',') {
            row.push_back (field);
            field.clear ();
            any = true;
        }
        else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < text.size () && text[i + 1] == '\n')
                ++i;
            if (any || !field.empty ()) {
                row.push_back (field);
                rows.push_back (row);
            }
            row.clear ();
            field.clear ();
            any = false;
            ++line;
        }
        else {
            field += c;
        }
    }
    if (quoted) {
        error = "line " + std::to_string (quoteLine) + ": a quoted field is not closed";
        rows.clear ();
        return false;
    }
    if (any || !field.empty ()) {
        row.push_back (field);
        rows.push_back (row);
    }
    return true;
}

std::string CatalogueToJson (const Catalogue& catalogue)
{
    json::JsonArray sources;
    for (const auto& s : catalogue.sources) {
        json::JsonObject o { { "id", json::JsonValue::String (s.id) } };
        if (!s.title.empty ())
            o["title"] = json::JsonValue::String (s.title);
        if (!s.note.empty ())
            o["note"] = json::JsonValue::String (s.note);
        if (!s.url.empty ())
            o["url"] = json::JsonValue::String (s.url);
        if (s.year)
            o["year"] = json::JsonValue::Integer (s.year);
        sources.push_back (json::JsonValue::Object (std::move (o)));
    }
    json::JsonArray plants;
    for (const Plant& p : catalogue.plants) {
        json::JsonObject o;
        o["id"] = json::JsonValue::String (p.id);
        o["scientificName"] = json::JsonValue::String (p.scientificName);
        o["cultivar"] = p.cultivar.empty () ? json::JsonValue () : json::JsonValue::String (p.cultivar);
        json::JsonObject names;
        for (const auto& [lang, name] : p.localNames)
            names[lang] = json::JsonValue::String (name);
        o["localNames"] = json::JsonValue::Object (std::move (names));
        o["habit"] = p.habit == Habit::Unknown ? json::JsonValue () : json::JsonValue::String (HabitKey (p.habit));
        std::vector<std::string> light;
        for (int c = 0; c < kLightClassCount; ++c)
            if (p.light[size_t (c)])
                light.push_back (LightKey (LightClass (c)));
        o["light"] = json::JsonValue::Object ({ { "acceptedClasses", StringsJson (light) } });
        o["matureHeightM"] = RangeJson (p.matureHeightM);
        o["matureSpreadM"] = RangeJson (p.matureSpreadM);
        o["plantingDensityPerM2"] = RangeJson (p.densityPerM2);
        o["preferredReceiverHeightM"] =
            p.receiverHeightM ? json::JsonValue::Double (*p.receiverHeightM) : json::JsonValue ();
        json::JsonArray months;
        for (int m : p.floweringMonths)
            months.push_back (json::JsonValue::Integer (m));
        o["floweringMonths"] = json::JsonValue::Array (std::move (months));
        o["deciduous"] = p.deciduous ? json::JsonValue::Bool (*p.deciduous) : json::JsonValue ();
        o["moistureClasses"] = StringsJson (p.moistureClasses);
        o["plantingLayers"] = StringsJson (p.plantingLayers);
        json::JsonArray refs;
        for (const auto& r : p.sources) {
            json::JsonObject ref { { "sourceId", json::JsonValue::String (r.sourceId) } };
            if (r.page)
                ref["page"] = json::JsonValue::Integer (r.page);
            refs.push_back (json::JsonValue::Object (std::move (ref)));
        }
        o["sourceRefs"] = json::JsonValue::Array (std::move (refs));
        plants.push_back (json::JsonValue::Object (std::move (o)));
    }
    json::JsonObject root;
    root["schemaVersion"] = json::JsonValue::Integer (kSchemaVersion);
    root["catalogueId"] = json::JsonValue::String (catalogue.catalogueId);
    if (!catalogue.locale.empty ())
        root["locale"] = json::JsonValue::String (catalogue.locale);
    root["sources"] = json::JsonValue::Array (std::move (sources));
    root["plants"] = json::JsonValue::Array (std::move (plants));
    return json::Write (json::JsonValue::Object (std::move (root)), 2) + "\n";
}

bool CatalogueFromCsv (const std::string& text, const Catalogue& base, Catalogue& out, ParseProblems& problems)
{
    problems = {};
    std::vector<std::vector<std::string>> rows;
    std::string error;
    if (!ParseCsv (text, rows, error)) {
        problems.items.push_back (error);
        return false;
    }
    if (rows.empty ()) {
        problems.items.push_back ("the CSV is empty; it needs a header row");
        return false;
    }
    std::map<std::string, size_t> column;
    for (size_t i = 0; i < rows[0].size (); ++i) {
        const std::string name = Trim (rows[0][i]);
        const auto& known = CatalogueCsvColumns ();
        if (std::find (known.begin (), known.end (), name) == known.end ()) {
            if (!name.empty ())
                problems.items.push_back ("header: unknown column '" + name + "'");
            continue;
        }
        column[name] = i;
    }
    for (const char* required : { "id", "scientificName" })
        if (!column.count (required))
            problems.items.push_back (std::string ("header: column '") + required + "' is required");
    if (!problems.Empty ())
        return false;
    // Build plants.json in memory, then let ParseCatalogue judge it: one validator.
    Catalogue built;
    built.catalogueId = base.catalogueId.empty () ? "imported" : base.catalogueId;
    built.locale = base.locale;
    std::set<std::string> sourceIds;
    for (size_t r = 1; r < rows.size (); ++r) {
        const auto& row = rows[r];
        const std::string where = "CSV row " + std::to_string (r + 1) + ": ";
        auto cell = [&] (const char* name) {
            const auto it = column.find (name);
            return it == column.end () || it->second >= row.size () ? std::string () : Trim (row[it->second]);
        };
        auto number = [&] (const char* name, std::optional<double>& value) {
            const std::string s = cell (name);
            if (s.empty ())
                return;
            try {
                size_t used = 0;
                const double v = std::stod (s, &used);
                if (used != s.size () || !std::isfinite (v))
                    throw std::invalid_argument (s);
                value = v;
            }
            catch (...) {
                problems.items.push_back (where + name + ": '" + s + "' is not a number");
            }
        };
        auto range = [&] (const char* lo, const char* hi, std::optional<Range>& out) {
            std::optional<double> a, b;
            number (lo, a);
            number (hi, b);
            if (a && b)
                out = Range { *a, *b };
            else if (a || b)
                problems.items.push_back (where + lo + " and " + hi + " are given together or not at all");
        };
        Plant p;
        p.id = cell ("id");
        p.scientificName = cell ("scientificName");
        if (p.id.empty () && p.scientificName.empty ())
            continue; // a blank row
        p.cultivar = cell ("cultivar");
        if (!cell ("name_lt").empty ())
            p.localNames["lt"] = cell ("name_lt");
        if (!cell ("name_en").empty ())
            p.localNames["en"] = cell ("name_en");
        const std::string habit = cell ("habit");
        p.habit = Habit::Unknown;
        for (int h = 1; h < kHabitCount; ++h)
            if (habit == HabitKey (Habit (h)))
                p.habit = Habit (h);
        if (!habit.empty () && p.habit == Habit::Unknown)
            problems.items.push_back (where + "habit: '" + habit + "' is not a known habit");
        for (const auto& key : Split (cell ("light"))) {
            bool found = false;
            for (int c = 0; c < kLightClassCount; ++c)
                if (key == LightKey (LightClass (c))) {
                    p.light[size_t (c)] = true;
                    found = true;
                }
            if (!found)
                problems.items.push_back (where + "light: '" + key + "' is not shade, partial or full");
        }
        range ("matureHeightMinM", "matureHeightMaxM", p.matureHeightM);
        range ("matureSpreadMinM", "matureSpreadMaxM", p.matureSpreadM);
        range ("densityMinPerM2", "densityMaxPerM2", p.densityPerM2);
        number ("receiverHeightM", p.receiverHeightM);
        for (const auto& m : Split (cell ("floweringMonths"))) {
            try {
                p.floweringMonths.push_back (std::stoi (m));
            }
            catch (...) {
                problems.items.push_back (where + "floweringMonths: '" + m + "' is not a month");
            }
        }
        const std::string deciduous = cell ("deciduous");
        if (deciduous == "true" || deciduous == "yes")
            p.deciduous = true;
        else if (deciduous == "false" || deciduous == "no")
            p.deciduous = false;
        else if (!deciduous.empty ())
            problems.items.push_back (where + "deciduous: '" + deciduous + "' is not true or false");
        p.moistureClasses = Split (cell ("moistureClasses"));
        p.plantingLayers = Split (cell ("plantingLayers"));
        const std::string source = cell ("source");
        if (!source.empty ()) {
            PlantSourceRef ref { source, 0 };
            const std::string page = cell ("page");
            if (!page.empty ()) {
                try {
                    ref.page = std::stoi (page);
                }
                catch (...) {
                    problems.items.push_back (where + "page: '" + page + "' is not a page number");
                }
            }
            p.sources.push_back (ref);
            if (sourceIds.insert (source).second) {
                const auto known = std::find_if (base.sources.begin (), base.sources.end (),
                                                 [&] (const CatalogueSource& s) { return s.id == source; });
                built.sources.push_back (known != base.sources.end () ? *known : CatalogueSource { source, source });
            }
        }
        built.plants.push_back (std::move (p));
    }
    if (!problems.Empty ())
        return false;
    // The JSON reader is the one validator: ranges, months, duplicates.
    Catalogue checked;
    if (!ParseCatalogue (CatalogueToJson (built), checked, problems))
        return false;
    out = std::move (checked);
    return true;
}

} // namespace geomsrv::archviz::planting
