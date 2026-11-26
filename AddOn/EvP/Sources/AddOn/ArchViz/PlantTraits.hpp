#ifndef TAPIOCA_ARCHVIZ_PLANTTRAITS_HPP
#define TAPIOCA_ARCHVIZ_PLANTTRAITS_HPP

// ArchViz/PlantTraits -- a plant record's ecological and competition traits
// (#278): the fields a community compatibility check needs beyond the regional
// standard's own columns.
//
// Pure and DevKit-free. One descriptor table drives the JSON reader, the JSON
// writer and the CSV columns, so a trait cannot be read but not written back:
// a CSV import rewrites plants.json from the parsed records, and a trait the
// writer forgot would be deleted by the import.
//
// ⚠️ UNKNOWN STAYS UNKNOWN. Every trait is optional; null, absent or [] reads
// as "not given", never as a default. The writer emits every field, null when
// not given, so a catalogue shows the lines still to fill.
//
// ⚠️ NOT A COMPATIBILITY MATRIX. These are per-plant traits. Documented
// plant-to-plant exceptions (allelopathy, disease hosts) belong in a separate
// interactions file, sourced, and most plants never appear there.

#include "NodeGraph/Json.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace geomsrv::archviz::planting {

enum class TraitKind : uint8_t {
    Choice,     // one key of the field's list
    ChoiceList, // several keys
    Number,     // within [lo, hi]
    Months,     // months 1-12
    Text        // free text
};

// In table order; the index is the trait's slot in PlantTraits::values.
enum class Trait : uint8_t {
    EllenbergLight,
    EllenbergMoisture,
    EllenbergReaction,
    EllenbergNutrients,
    EllenbergSalinity,
    PhMin,
    PhMax,
    NutrientPreference,
    Drainage,
    Hardiness,
    EstablishmentSpeed,
    CompetitiveVigour,
    SpreadMode,
    LateralSpread,
    SelfSeeding,
    RootDepth,
    RootArchitecture,
    RootCompetitiveIntensity,
    CanopyDensity,
    CanopyTransmittance,
    EmergenceMonths,
    SenescenceMonths,
    LeafOnMonths,
    Lifespan,
    MaintenanceSensitivity,
    RegulatoryStatus,
    RegulatoryScope,
    RegulatoryNote,
};
constexpr int kTraitCount = 28;

struct TraitField {
    const char* group; // JSON block: "ecology", "competition", ...
    const char* key;   // field in the block
    TraitKind kind;
    const char* const* choices = nullptr;
    int choiceCount = 0;
    double lo = 0, hi = 0; // Number bounds, inclusive
};

const TraitField& TraitFieldOf (Trait t);
std::string TraitPath (Trait t); // "ecology.ellenbergLight"
std::optional<Trait> FindTrait (const std::string& path);

struct TraitValue {
    int choice = -1;          // Choice: index into choices, -1 not given
    std::vector<int> choices; // ChoiceList
    std::optional<double> number;
    std::vector<int> months;
    std::string text;
    bool Given () const
    {
        return choice >= 0 || !choices.empty () || number.has_value () || !months.empty () || !text.empty ();
    }
};

// Which source a trait value came from, and how sure it is. `field` is a trait
// path or one of the standard's own fields ("light", "matureHeightM", ...).
struct TraitEvidence {
    std::string field;
    std::string sourceId;
    int confidence = -1; // index into TraitConfidenceKeys, -1 not given
    std::string method;
};
const char* const* TraitConfidenceKeys (); // "low", "medium", "high"

struct PlantTraits {
    std::array<TraitValue, kTraitCount> values {};
    std::vector<TraitEvidence> evidence;

    const TraitValue& operator[] (Trait t) const
    {
        return values[size_t (t)];
    }
    TraitValue& operator[] (Trait t)
    {
        return values[size_t (t)];
    }
    // The chosen key of a Choice trait, or nullptr when not given.
    const char* ChoiceKey (Trait t) const;
    bool AnyGiven () const;
};

// Reads the trait blocks of one plant record. `fail(field, what)` is called
// once per problem, with the trait path as the field.
using TraitFail = std::function<void (const std::string& field, const std::string& what)>;
void ParseTraits (const evp::nodegraph::json::JsonValue& plant, const std::set<std::string>& sourceIds,
                  const TraitFail& fail, PlantTraits& out);

// Writes every trait block into a plant object: null or [] when not given.
void TraitsToJson (const PlantTraits& traits, evp::nodegraph::json::JsonObject& plant);

// CSV: one column per trait path, plus "evidence" as
// "field:sourceId:confidence:method|...". Lists join with '|'.
std::vector<std::string> TraitCsvColumns ();
std::string TraitCsvCell (const PlantTraits& traits, Trait t);
std::string EvidenceCsvCell (const PlantTraits& traits);
// False with `error` when the text is not a value of that trait.
bool TraitFromText (Trait t, const std::string& text, TraitValue& out, std::string& error);
bool EvidenceFromText (const std::string& text, std::vector<TraitEvidence>& out, std::string& error);

} // namespace geomsrv::archviz::planting

#endif
