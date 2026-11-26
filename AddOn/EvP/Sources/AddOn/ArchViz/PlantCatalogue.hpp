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
// ⚠️ THE SEED IS THE VILNIUS 2024 CATALOGUE. It is transcribed from the city's
// planting and maintenance standard (pp. 6-34), as printed. Its species data is
// the standard's, including the source's spelling. The standard is an official
// municipal document, not subject to copyright (the project owner's
// determination), so it ships in full. A regional catalogue is imported by
// replacing plants.json. The trait blocks (PlantTraits.hpp) are not the
// standard's: they ship empty, to be filled with sourced values.

#include "ArchViz/PlantTraits.hpp"

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

// The standard's section the plant was listed in. It is not the same as the
// habit: section 1.3 mixes perennials, grasses, ferns and subshrubs.
enum class PlantSection : uint8_t { Unknown, Trees, Shrubs, Perennials };
constexpr int kSectionCount = 4;
const char* SectionKey (PlantSection s);
const char* SectionName (PlantSection s);

// Salt tolerance: M low, H high, N none. The standard's "-" is not a class.
enum class SaltTolerance : uint8_t { Unknown, None, Low, High };
const char* SaltToleranceKey (SaltTolerance s);
const char* SaltToleranceName (SaltTolerance s);

// Growth intensity of a herbaceous plant: L slow, G fast, A aggressive.
enum class GrowthIntensity : uint8_t { Unknown, Slow, Fast, Aggressive };
const char* GrowthIntensityKey (GrowthIntensity g);
const char* GrowthIntensityName (GrowthIntensity g);

// Water need: an empty droplet is "no extra watering", one filled droplet is
// "water as needed", two are "wet sites or continuous watering".
enum class MoistureClass : uint8_t { Dry, Moist, Wet };
constexpr int kMoistureClassCount = 3;
const char* MoistureClassKey (MoistureClass m);
const char* MoistureClassName (MoistureClass m);

// The plant's layer in a herbaceous bed: cover (D), seasonal (S), structural
// (Str). A source cell of "S/Str" lists both.
enum class PlantingLayer : uint8_t { Cover, Seasonal, Structural };
constexpr int kPlantingLayerCount = 3;
const char* PlantingLayerKey (PlantingLayer l);
const char* PlantingLayerName (PlantingLayer l);

// Where the standard recommends planting. Trees use the first, second and last
// seven; shrubs use the last eight (streets replaces the green strip/roadside).
enum class Site : uint8_t {
    GreenStrip,
    Roadside,
    Streets,
    Alleys,
    SquaresParks,
    Roundabouts,
    UrbanForest,
    Riverside,
    OnSlabs,
    Containers
};
constexpr int kSiteCount = 10;
const char* SiteKey (Site s);
const char* SiteName (Site s);

// A mature size or density. "16+" has no stated upper bound and "up to 8" no
// stated lower bound; the flags keep the print rather than inventing a number.
struct Range {
    double min = 0, max = 0;
    bool openMin = false;
    bool openMax = false;
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
    PlantSection section = PlantSection::Unknown;
    std::string sourceNumber; // the standard's own row number, as printed
    Habit habit = Habit::Unknown;
    bool breeam = false;
    bool ornamentalFlowering = false;            // the standard's flower mark (trees)
    bool ornithological = false;                 // the standard's bird mark (trees)
    std::array<bool, kLightClassCount> light {}; // accepted classes
    SaltTolerance saltTolerance = SaltTolerance::Unknown;
    GrowthIntensity growthIntensity = GrowthIntensity::Unknown;
    std::vector<MoistureClass> moistureClasses;
    std::vector<PlantingLayer> plantingLayers;
    std::optional<Range> matureHeightM;
    std::optional<Range> matureSpreadM;
    std::optional<double> receiverHeightM; // preferredReceiverHeightM
    std::optional<Range> densityPerM2;     // plantingDensityPerM2, plants per m²
    std::vector<int> floweringMonths;
    std::vector<std::string> soilTypes; // A/B/C/D ground types
    std::array<bool, kSiteCount> siteSuitable {};
    std::array<std::string, kSiteCount> siteSoil; // "" when not given
    std::optional<bool> deciduous;
    std::vector<PlantSourceRef> sources;
    // Ecology, competition, roots, canopy, phenology, lifecycle, regulatory and
    // evidence (PlantTraits.hpp): not in the regional standard; filled by hand.
    PlantTraits traits;

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
// trees 3 m), else 0.5 m. A herbaceous plant listed only in the cover layer
// (D: the filler between taller plants) reads at the ground-cover tier.
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

// The catalogue's light class for one of the tab's classes (lowest first);
// deep shade, when shown, counts as shade.
LightClass LightOfClass (const PlantingHud& hud, int cls);

// The tab's class indices (lowest first, deep shade first when shown) a plant
// accepts, as a bit mask for the highlight.
uint32_t AcceptedClassMask (const PlantingHud& hud, const Plant& plant);

// Search and filters: the text matches scientific, cultivar, local names or
// id, case-insensitively (Latin-1 and Latin Extended-A capitals fold too, so
// "švedinis" finds "Švedinis"); -1 means any. A plant whose record leaves a
// field blank does not match a filter on that field: blank is not a yes.
struct PlantFilter {
    std::string text;
    int habit = -1;
    int light = -1;
    int section = -1;  // PlantSection
    int site = -1;     // Site: recommended there
    int moisture = -1; // MoistureClass: listed among its classes
    bool breeamOnly = false;
    bool Any () const
    {
        return habit >= 0 || light >= 0 || section >= 0 || site >= 0 || moisture >= 0 || breeamOnly;
    }
};
bool Matches (const Plant& plant, const PlantFilter& filter);

} // namespace geomsrv::archviz::planting

#endif
