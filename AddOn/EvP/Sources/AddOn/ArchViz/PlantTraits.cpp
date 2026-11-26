#include "ArchViz/PlantTraits.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace geomsrv::archviz::planting {

namespace json = evp::nodegraph::json;

namespace {

constexpr const char* kDegree[] = { "low", "medium", "high" };
constexpr const char* kSpeed[] = { "slow", "medium", "fast" };
constexpr const char* kSpread[] = { "none", "low", "medium", "high" };
constexpr const char* kDrainage[] = { "freelyDrained", "moderatelyDrained", "poorlyDrained", "waterlogged" };
constexpr const char* kSpreadMode[] = { "clump", "shortRhizome", "longRhizome", "stolon", "rootSucker", "layering" };
constexpr const char* kRootDepth[] = { "shallow", "medium", "deep" };
constexpr const char* kRootArchitecture[] = { "fibrous", "taproot", "heart", "flat" };
constexpr const char* kCanopyDensity[] = { "light", "medium", "dense" };
constexpr const char* kLifespan[] = { "short", "medium", "long" };
constexpr const char* kRegulatory[] = { "allowed", "restricted", "prohibited" };

#define CHOICES(list) list, int (std::size (list))

// In the order of enum class Trait.
const TraitField kFields[kTraitCount] = {
    { "ecology", "ellenbergLight", TraitKind::Number, nullptr, 0, 1, 9 },
    { "ecology", "ellenbergMoisture", TraitKind::Number, nullptr, 0, 1, 12 },
    { "ecology", "ellenbergReaction", TraitKind::Number, nullptr, 0, 1, 9 },
    { "ecology", "ellenbergNutrients", TraitKind::Number, nullptr, 0, 1, 9 },
    { "ecology", "ellenbergSalinity", TraitKind::Number, nullptr, 0, 0, 9 },
    { "ecology", "pHMin", TraitKind::Number, nullptr, 0, 3, 10 },
    { "ecology", "pHMax", TraitKind::Number, nullptr, 0, 3, 10 },
    { "ecology", "nutrientPreference", TraitKind::Choice, CHOICES (kDegree) },
    { "ecology", "drainage", TraitKind::ChoiceList, CHOICES (kDrainage) },
    { "ecology", "hardiness", TraitKind::Text },
    { "competition", "establishmentSpeed", TraitKind::Choice, CHOICES (kSpeed) },
    { "competition", "competitiveVigour", TraitKind::Choice, CHOICES (kDegree) },
    { "competition", "spreadMode", TraitKind::ChoiceList, CHOICES (kSpreadMode) },
    { "competition", "lateralSpread", TraitKind::Choice, CHOICES (kSpread) },
    { "competition", "selfSeeding", TraitKind::Choice, CHOICES (kSpread) },
    { "roots", "depthClass", TraitKind::Choice, CHOICES (kRootDepth) },
    { "roots", "architecture", TraitKind::Choice, CHOICES (kRootArchitecture) },
    { "roots", "competitiveIntensity", TraitKind::Choice, CHOICES (kDegree) },
    { "canopy", "density", TraitKind::Choice, CHOICES (kCanopyDensity) },
    { "canopy", "transmittance", TraitKind::Number, nullptr, 0, 0, 1 },
    { "phenology", "emergenceMonths", TraitKind::Months },
    { "phenology", "senescenceMonths", TraitKind::Months },
    { "phenology", "leafOnMonths", TraitKind::Months },
    { "lifecycle", "lifespan", TraitKind::Choice, CHOICES (kLifespan) },
    { "lifecycle", "maintenanceSensitivity", TraitKind::Choice, CHOICES (kDegree) },
    { "regulatory", "status", TraitKind::Choice, CHOICES (kRegulatory) },
    { "regulatory", "scope", TraitKind::Text },
    { "regulatory", "note", TraitKind::Text },
};

#undef CHOICES

constexpr const char* kConfidence[] = { "low", "medium", "high" };

// The standard's own fields, which evidence may also name.
constexpr const char* kRecordFields[] = { "scientificName",
                                          "cultivar",
                                          "localNames",
                                          "section",
                                          "habit",
                                          "light",
                                          "matureHeightM",
                                          "matureSpreadM",
                                          "plantingDensityPerM2",
                                          "preferredReceiverHeightM",
                                          "moistureClasses",
                                          "plantingLayers",
                                          "soilTypes",
                                          "saltTolerance",
                                          "growthIntensity",
                                          "sites",
                                          "siteSoils",
                                          "floweringMonths",
                                          "deciduous",
                                          "breeam",
                                          "ornamentalFlowering",
                                          "ornithological" };

std::string ChoiceText (const TraitField& f)
{
    std::string s;
    for (int i = 0; i < f.choiceCount; ++i)
        s += std::string (i == 0 ? "" : i + 1 == f.choiceCount ? " or " : ", ") + f.choices[i];
    return s;
}

int ChoiceIndex (const TraitField& f, const std::string& key)
{
    for (int i = 0; i < f.choiceCount; ++i)
        if (key == f.choices[i])
            return i;
    return -1;
}

std::string NumberText (double v)
{
    char text[32];
    std::snprintf (text, sizeof (text), "%g", v);
    return text;
}

bool KnownEvidenceField (const std::string& field)
{
    return FindTrait (field).has_value () ||
           std::find (std::begin (kRecordFields), std::end (kRecordFields), field) != std::end (kRecordFields);
}

std::vector<std::string> SplitOn (const std::string& text, char sep)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        const size_t at = text.find (sep, start);
        std::string item = text.substr (start, at == std::string::npos ? std::string::npos : at - start);
        item.erase (0, item.find_first_not_of (" \t"));
        item.erase (item.find_last_not_of (" \t") + 1);
        out.push_back (item);
        if (at == std::string::npos)
            return out;
        start = at + 1;
    }
}

} // namespace

const TraitField& TraitFieldOf (Trait t)
{
    return kFields[size_t (t)];
}

std::string TraitPath (Trait t)
{
    const TraitField& f = TraitFieldOf (t);
    return std::string (f.group) + "." + f.key;
}

std::optional<Trait> FindTrait (const std::string& path)
{
    for (int i = 0; i < kTraitCount; ++i)
        if (TraitPath (Trait (i)) == path)
            return Trait (i);
    return std::nullopt;
}

const char* const* TraitConfidenceKeys ()
{
    return kConfidence;
}

const char* PlantTraits::ChoiceKey (Trait t) const
{
    const TraitField& f = TraitFieldOf (t);
    const int c = values[size_t (t)].choice;
    return f.kind == TraitKind::Choice && c >= 0 && c < f.choiceCount ? f.choices[c] : nullptr;
}

bool PlantTraits::AnyGiven () const
{
    return !evidence.empty () ||
           std::any_of (values.begin (), values.end (), [] (const TraitValue& v) { return v.Given (); });
}

void ParseTraits (const json::JsonValue& plant, const std::set<std::string>& sourceIds, const TraitFail& fail,
                  PlantTraits& out)
{
    // A misspelt key inside a trait block would otherwise read as "not given".
    std::set<std::string> groups;
    for (const TraitField& f : kFields)
        groups.insert (f.group);
    for (const std::string& group : groups) {
        const json::JsonValue* block = plant.Find (group);
        const json::JsonObject* object = block ? block->AsObject () : nullptr;
        for (const auto& kv : object ? *object : json::JsonObject {})
            if (!FindTrait (group + "." + kv.first))
                fail (group + "." + kv.first, "is not a known trait");
    }
    for (int i = 0; i < kTraitCount; ++i) {
        const TraitField& f = kFields[i];
        const std::string path = TraitPath (Trait (i));
        const json::JsonValue* block = plant.Find (f.group);
        if (!block || block->IsNull ())
            continue;
        if (!block->AsObject ()) {
            if (i == 0 || std::string (kFields[i - 1].group) != f.group)
                fail (f.group, "must be an object");
            continue;
        }
        const json::JsonValue* v = block->Find (f.key);
        if (!v || v->IsNull ())
            continue;
        TraitValue& value = out.values[size_t (i)];
        switch (f.kind) {
            case TraitKind::Choice: {
                std::string s;
                if (!v->AsString (s))
                    fail (path, "must be text");
                else if (!s.empty () && (value.choice = ChoiceIndex (f, s)) < 0)
                    fail (path, "'" + s + "' is not " + ChoiceText (f));
                break;
            }
            case TraitKind::ChoiceList:
            case TraitKind::Months: {
                const json::JsonArray* list = v->AsArray ();
                if (!list) {
                    fail (path, "must be a list");
                    break;
                }
                for (size_t k = 0; k < list->size (); ++k) {
                    const std::string at = path + "[" + std::to_string (k) + "]";
                    if (f.kind == TraitKind::Months) {
                        int64_t m = 0;
                        if (!(*list)[k].AsInteger (m) || m < 1 || m > 12)
                            fail (at, "must be a month 1-12");
                        else
                            value.months.push_back (int (m));
                        continue;
                    }
                    std::string s;
                    int c = -1;
                    if (!(*list)[k].AsString (s))
                        fail (at, "must be text");
                    else if ((c = ChoiceIndex (f, s)) < 0)
                        fail (at, "'" + s + "' is not " + ChoiceText (f));
                    else
                        value.choices.push_back (c);
                }
                break;
            }
            case TraitKind::Number: {
                double d = 0;
                if (!v->AsDouble (d) || !std::isfinite (d))
                    fail (path, "must be a number");
                else if (d < f.lo || d > f.hi)
                    fail (path, "must be " + NumberText (f.lo) + " to " + NumberText (f.hi));
                else
                    value.number = d;
                break;
            }
            case TraitKind::Text:
                if (!v->AsString (value.text))
                    fail (path, "must be text");
                break;
        }
    }
    const auto& lo = out[Trait::PhMin].number;
    const auto& hi = out[Trait::PhMax].number;
    if (lo && hi && *hi < *lo)
        fail ("ecology.pHMax", "must not be below pHMin");

    const json::JsonValue* evidence = plant.Find ("evidence");
    if (!evidence || evidence->IsNull ())
        return;
    const json::JsonArray* list = evidence->AsArray ();
    if (!list) {
        fail ("evidence", "must be a list");
        return;
    }
    for (size_t k = 0; k < list->size (); ++k) {
        const std::string at = "evidence[" + std::to_string (k) + "]";
        const json::JsonValue& one = (*list)[k];
        TraitEvidence e;
        const json::JsonValue* field = one.Find ("field");
        if (!field || !field->AsString (e.field) || !KnownEvidenceField (e.field)) {
            fail (at, "field must name a trait path or a record field");
            continue;
        }
        const json::JsonValue* sid = one.Find ("sourceId");
        if (!sid || !sid->AsString (e.sourceId) || !sourceIds.count (e.sourceId)) {
            fail (at, "sourceId must name an entry of sources");
            continue;
        }
        if (const json::JsonValue* c = one.Find ("confidence"); c && !c->IsNull ()) {
            std::string s;
            const auto* found = c->AsString (s) ? std::find (std::begin (kConfidence), std::end (kConfidence), s)
                                                : std::end (kConfidence);
            if (found == std::end (kConfidence)) {
                fail (at, "confidence must be low, medium or high");
                continue;
            }
            e.confidence = int (found - std::begin (kConfidence));
        }
        if (const json::JsonValue* m = one.Find ("method"); m && !m->IsNull () && !m->AsString (e.method)) {
            fail (at, "method must be text");
            continue;
        }
        out.evidence.push_back (std::move (e));
    }
}

void TraitsToJson (const PlantTraits& traits, json::JsonObject& plant)
{
    std::map<std::string, json::JsonObject> blocks;
    for (int i = 0; i < kTraitCount; ++i) {
        const TraitField& f = kFields[i];
        const TraitValue& v = traits.values[size_t (i)];
        json::JsonValue value;
        switch (f.kind) {
            case TraitKind::Choice:
                if (v.choice >= 0)
                    value = json::JsonValue::String (f.choices[v.choice]);
                break;
            case TraitKind::ChoiceList: {
                json::JsonArray items;
                for (int c : v.choices)
                    items.push_back (json::JsonValue::String (f.choices[c]));
                value = json::JsonValue::Array (std::move (items));
                break;
            }
            case TraitKind::Months: {
                json::JsonArray items;
                for (int m : v.months)
                    items.push_back (json::JsonValue::Integer (m));
                value = json::JsonValue::Array (std::move (items));
                break;
            }
            case TraitKind::Number:
                if (v.number)
                    value = json::JsonValue::Double (*v.number);
                break;
            case TraitKind::Text:
                if (!v.text.empty ())
                    value = json::JsonValue::String (v.text);
                break;
        }
        blocks[f.group][f.key] = std::move (value);
    }
    for (auto& [group, block] : blocks)
        plant[group] = json::JsonValue::Object (std::move (block));
    json::JsonArray evidence;
    for (const TraitEvidence& e : traits.evidence) {
        json::JsonObject o { { "field", json::JsonValue::String (e.field) },
                             { "sourceId", json::JsonValue::String (e.sourceId) } };
        o["confidence"] = e.confidence >= 0 ? json::JsonValue::String (kConfidence[e.confidence]) : json::JsonValue ();
        o["method"] = e.method.empty () ? json::JsonValue () : json::JsonValue::String (e.method);
        evidence.push_back (json::JsonValue::Object (std::move (o)));
    }
    plant["evidence"] = json::JsonValue::Array (std::move (evidence));
}

std::vector<std::string> TraitCsvColumns ()
{
    std::vector<std::string> columns;
    for (int i = 0; i < kTraitCount; ++i)
        columns.push_back (TraitPath (Trait (i)));
    columns.push_back ("evidence");
    return columns;
}

std::string TraitCsvCell (const PlantTraits& traits, Trait t)
{
    const TraitField& f = TraitFieldOf (t);
    const TraitValue& v = traits[t];
    std::string s;
    switch (f.kind) {
        case TraitKind::Choice:
            return v.choice >= 0 ? f.choices[v.choice] : "";
        case TraitKind::ChoiceList:
            for (int c : v.choices)
                s += (s.empty () ? "" : "|") + std::string (f.choices[c]);
            return s;
        case TraitKind::Months:
            for (int m : v.months)
                s += (s.empty () ? "" : "|") + std::to_string (m);
            return s;
        case TraitKind::Number:
            return v.number ? NumberText (*v.number) : "";
        case TraitKind::Text:
            return v.text;
    }
    return s;
}

std::string EvidenceCsvCell (const PlantTraits& traits)
{
    std::string s;
    for (const TraitEvidence& e : traits.evidence)
        s += (s.empty () ? "" : "|") + e.field + ":" + e.sourceId + ":" +
             (e.confidence >= 0 ? kConfidence[e.confidence] : "") + ":" + e.method;
    return s;
}

bool TraitFromText (Trait t, const std::string& text, TraitValue& out, std::string& error)
{
    const TraitField& f = TraitFieldOf (t);
    out = {};
    if (text.empty ())
        return true;
    switch (f.kind) {
        case TraitKind::Choice:
            out.choice = ChoiceIndex (f, text);
            if (out.choice < 0)
                error = "'" + text + "' is not " + ChoiceText (f);
            return out.choice >= 0;
        case TraitKind::ChoiceList:
            for (const std::string& key : SplitOn (text, '|')) {
                if (key.empty ())
                    continue;
                const int c = ChoiceIndex (f, key);
                if (c < 0) {
                    error = "'" + key + "' is not " + ChoiceText (f);
                    return false;
                }
                out.choices.push_back (c);
            }
            return true;
        case TraitKind::Months:
            for (const std::string& key : SplitOn (text, '|')) {
                if (key.empty ())
                    continue;
                char* end = nullptr;
                const long m = std::strtol (key.c_str (), &end, 10);
                if (*end != '\0' || m < 1 || m > 12) {
                    error = "'" + key + "' is not a month 1-12";
                    return false;
                }
                out.months.push_back (int (m));
            }
            return true;
        case TraitKind::Number: {
            char* end = nullptr;
            const double d = std::strtod (text.c_str (), &end);
            if (*end != '\0' || !std::isfinite (d)) {
                error = "'" + text + "' is not a number";
                return false;
            }
            out.number = d; // the range is checked by the JSON reader the import runs through
            return true;
        }
        case TraitKind::Text:
            out.text = text;
            return true;
    }
    return true;
}

bool EvidenceFromText (const std::string& text, std::vector<TraitEvidence>& out, std::string& error)
{
    out.clear ();
    for (const std::string& item : SplitOn (text, '|')) {
        if (item.empty ())
            continue;
        const std::vector<std::string> parts = SplitOn (item, ':');
        if (parts.size () < 2 || parts.size () > 4) {
            error = "'" + item + "' is not field:sourceId[:confidence[:method]]";
            return false;
        }
        TraitEvidence e { parts[0], parts[1], -1, parts.size () > 3 ? parts[3] : "" };
        if (parts.size () > 2 && !parts[2].empty ()) {
            const auto* found = std::find (std::begin (kConfidence), std::end (kConfidence), parts[2]);
            if (found == std::end (kConfidence)) {
                error = "'" + parts[2] + "' is not low, medium or high";
                return false;
            }
            e.confidence = int (found - std::begin (kConfidence));
        }
        out.push_back (std::move (e));
    }
    return true;
}

} // namespace geomsrv::archviz::planting
