#ifndef EVP_ARCHVIZ_PLANTCATALOGUE_HPP
#define EVP_ARCHVIZ_PLANTCATALOGUE_HPP

// ArchViz/PlantCatalogue -- the local, user-editable plant catalogue (#278
// Stage 3): plants.json and profiles.json, parsed and validated as plain data.
// No files here (PlantCatalogueStore.cpp reads and writes them), no ImGui.
//
// ⚠️ UNKNOWN IS NOT A MATCH. A plant without light classes, or one measured at
// no layer, is "unknown", never compatible. Empty lists are not wildcards.
//
// ⚠️ A FILE WITH ANY PROBLEM IS REFUSED WHOLE. The previous catalogue stays in
// use and every problem is listed with its record; nothing is half-loaded.
//
// ⚠️ THE SEED IS AN EXAMPLE. Its light classes are common garden guidance, not
// a sourced catalogue; it carries no heights. A regional catalogue is imported
// by replacing plants.json.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace geomsrv::archviz::planting {

struct PlantingHud; // ArchViz/PlantingHudModel.hpp

// The catalogue's light classes. Deep shade, when the tab shows it, is a
// display subdivision of shade and counts as shade.
enum class LightClass : uint8_t { Shade, Partial, Full };
constexpr int kLightClassCount = 3;
const char* LightKey (LightClass c);  // "shade", "partial", "full"
const char* LightName (LightClass c); // "Shade", "Partial shade", "Full sun"

enum class Habit : uint8_t { Unknown, Groundcover, Perennial, Grass, Fern, Bulb, Annual, Climber, Shrub, Tree };
constexpr int kHabitCount = 10;
const char* HabitKey (Habit h);  // "" for Unknown, else "groundcover", ...
const char* HabitName (Habit h); // "Unknown", "Ground cover", ...

struct Range {
    double min = 0, max = 0;
};

struct PlantSourceRef {
    std::string sourceId;
    int page = 0; // 0 when not given
};

struct Plant {
    std::string id;
    std::string scientificName;
    std::string cultivar;
    std::map<std::string, std::string> localNames; // "lt" -> "Melsvė"
    Habit habit = Habit::Unknown;
    std::array<bool, kLightClassCount> light {}; // accepted classes
    std::optional<Range> matureHeightM;
    std::optional<Range> matureSpreadM;
    std::optional<double> receiverHeightM; // preferredReceiverHeightM
    std::optional<Range> densityPerM2;     // plantingDensityPerM2, plants per m²
    std::vector<std::string> moistureClasses;
    std::vector<std::string> plantingLayers;
    std::vector<int> floweringMonths;
    std::optional<bool> deciduous;
    std::vector<PlantSourceRef> sources;

    bool HasLight () const
    {
        return light[0] || light[1] || light[2];
    }
    // "Hosta sieboldiana 'Elegans'"
    std::string DisplayName () const;
};

struct CatalogueSource {
    std::string id, title, note, url;
    int year = 0;
};

struct Catalogue {
    std::string catalogueId;
    std::string locale;
    std::vector<CatalogueSource> sources;
    std::vector<Plant> plants;
};

// A regional threshold profile.
struct Profile {
    std::string id, name, source, edition;
    float partialEdgeH = 3.0f, fullEdgeH = 6.0f;
    std::optional<float> deepShadeEdgeH;
    // Season as month/day, when the profile sets one.
    bool hasSeason = false;
    int fromMonth = 0, fromDay = 0, toMonth = 0, toDay = 0;
    std::string seasonNote;
    std::vector<float> receiverHeightsM; // empty: keep the form's
};

// Problems read "plants[3] 'hosta': light.acceptedClasses[1]: 'sunny' is not
// shade, partial or full". A JSON syntax error reads "line 12, column 5: ...".
struct ParseProblems {
    std::vector<std::string> items;
    bool Empty () const
    {
        return items.empty ();
    }
};

constexpr int kSchemaVersion = 1;

// False with every problem; `out` is untouched then.
bool ParseCatalogue (const std::string& text, Catalogue& out, ParseProblems& problems);
bool ParseProfiles (const std::string& text, std::vector<Profile>& out, ParseProblems& problems);

// The shipped seed files' text (UTF-8 JSON).
const std::string& SeedPlantsJson ();
const std::string& SeedProfilesJson ();

// The height a plant's light is read at: its preferred receiver height, else
// its habit's tier (ground cover 0.15 m, low planting 0.5 m, shrubs 1.5 m,
// trees 3 m), else 0.5 m.
float ReceiverHeightOf (const Plant& plant);

// Put a profile's edges, season and heights into the form.
void ApplyProfile (PlantingHud& hud, const Profile& profile);

// ---- fit ----------------------------------------------------------------------

// Where a plant's light classes are found, at the measured layer nearest its
// receiver height.
struct PlantFit {
    enum class Status : uint8_t { Unknown, None, Fits };
    Status status = Status::Unknown;
    std::string reason; // why unknown: "no light classes", "no layer measured"
    int layer = -1;     // index into PlantingHud::layers
    float layerHeight = 0;
    float wantedHeight = 0;
    double compatibleArea = 0, evaluatedArea = 0; // m²
    double Share () const
    {
        return evaluatedArea > 0 ? compatibleArea / evaluatedArea : 0.0;
    }
};
PlantFit FitOf (const PlantingHud& hud, const Plant& plant);

// Plants for a fit's compatible area at the plant's sourced density, rounded
// up; nothing when the catalogue gives no density (never a default).
std::optional<Range> PlantQuantity (const Plant& plant, const PlantFit& fit);

// The tab's class indices (lowest first, deep shade first when shown) a plant
// accepts, as a bit mask for the highlight.
uint32_t AcceptedClassMask (const PlantingHud& hud, const Plant& plant);

// Search and filters: the text matches scientific, cultivar, local names or
// id, case-insensitively; habit -1 and light -1 mean any.
struct PlantFilter {
    std::string text;
    int habit = -1;
    int light = -1;
};
bool Matches (const Plant& plant, const PlantFilter& filter);

} // namespace geomsrv::archviz::planting

#endif
