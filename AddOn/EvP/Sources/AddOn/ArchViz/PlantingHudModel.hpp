#ifndef EVP_ARCHVIZ_PLANTINGHUDMODEL_HPP
#define EVP_ARCHVIZ_PLANTINGHUDMODEL_HPP

// ArchViz/PlantingHudModel -- the Analysis page's Planting tab (#278 Stage 1C):
// its form, its run queue and its colours, as plain data and pure functions.
// No ImGui, no scene, no runner: DiligentHudPlanting.cpp draws it and talks to
// the services; this file is what the offline tests pin.
//
// ⚠️ THE CLASSES HERE ARE THE DISPLAY'S. The class areas and labels a report
// states come from SunStudy/SunStudyLightClass (exact `H >= edge`, raw hours);
// the tint's stepped bands widen each edge by kSunBandSlack. Both read the
// same edges, so the legend and the numbers agree at every edge but the slack.

#include "ArchViz/GradientLibrary.hpp"
#include "ArchViz/PlantCatalogue.hpp"
#include "ArchViz/PlantingScheme.hpp"
#include "SunStudy/SunStudyPlantingAreas.hpp"
#include "SunStudy/SunStudyWeather.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv::archviz {
struct SunStudyAtlasUpload; // ArchViz/SunStudyOverlay.hpp
}

namespace geomsrv::archviz::planting {

// ---- the study period -------------------------------------------------------
// Proposed regional defaults (Vilnius climatological seasons, 1991-2020), not
// law; Custom is any range within one year.
enum Season : int {
    kMidsummer = 0,    // June 21, one day: the conventional sunlight class
    kActiveGrowth = 1, // April 28 - October 1, the 10 C season (default)
    kExtended = 2,     // April 4 - October 22, the 5 C season
    kSpring = 3,       // April - May
    kSummer = 4,       // June - August
    kAutumn = 5,       // September - October
    kYear = 6,         // the whole year: evergreens
    kCustom = 7,
};
constexpr int kSeasonCount = 8;

struct SeasonDates {
    int fromMonth = 4, fromDay = 28, toMonth = 10, toDay = 1;
    bool singleDay = false;
    bool wholeYear = false;
};

const char* SeasonName (int season);
const char* SeasonTip (int season);
// The dates a season stands for; Custom returns the form's own.
SeasonDates DatesOf (int season, const SeasonDates& custom);
// Days in the range, both ends included; 0 when it ends before it starts.
int SeasonDays (const SeasonDates& dates, int year);

// The run form's timesteps, minutes: the Sun tab's list.
constexpr int kTimestepMinutes[] = { 5, 10, 15, 20, 30 };
constexpr int kTimestepCount = 5;

// ---- the layers -------------------------------------------------------------
// The receptor heights (#278 Stage 1A): one study per ticked height.
constexpr int kHeightCount = 4;
constexpr float kDefaultHeights[kHeightCount] = { 0.15f, 0.50f, 1.50f, 3.00f };
const char* HeightName (int slot); // "Ground cover", "Low planting", "Shrubs", "Trees"
constexpr float kMaxHeight = 100.0f;

// Finished layers the runner keeps for the layer switch: its
// kPlantingLayersKept (asserted equal in DiligentHudPlanting.cpp).
constexpr size_t kLayersKept = 8;

// ---- the classes ------------------------------------------------------------
// Edges in hours a day, ascending: [deep shade,] shade|partial, partial|sun.
constexpr float kDefaultDeepShade = 1.0f;
constexpr float kDefaultPartial = 3.0f;
constexpr float kDefaultSun = 6.0f;
constexpr int kMaxClasses = 4;

// What the Results section shows.
enum Metric : int {
    kSunClass = 0,  // the stepped classes
    kHoursADay = 1, // the smooth hours ramp
    kDaysAtSun = 2, // share of days at or above the sun edge (placeholder)
};

// One finished layer: its height and the study that measured it.
struct Layer {
    float height = 0.0f;
    std::string studyId;
    bool period = false;
    std::string label; // a period's "2026-04-28 to 2026-10-01", with " (estimate)"
    // Its display as last shown, for the Layout Builder's planting page (#278):
    // kept by the frame loop whenever this layer is the study on screen.
    std::shared_ptr<const SunStudyAtlasUpload> display;
    // Its whole-layer class areas (m²) and the edges they were read at, for
    // the Plants tab's fit; empty until read.
    std::vector<double> classAreas;
    double evaluatedArea = 0.0;
    std::vector<double> areaEdges;
};

// The local plant catalogue as the Plants tab holds it (#278 Stage 3).
struct CatalogueState {
    bool attempted = false; // the folder was seeded and read once
    std::string folder;
    Catalogue plants;
    std::vector<Profile> profiles;
    bool plantsLoaded = false, profilesLoaded = false;
    std::vector<std::string> problems; // the last read's, both files
    std::string status;                // "Read 8 plants and 1 profile."
    PlantFilter filter;
    int selected = -1; // index into plants.plants
    bool highlight = false;
    bool openPlantsTab = false;       // one frame: the inspector's "Find plants here"
    float filterFromPointHeight = -1; // the layer a point's light filter was read at
};

// The weather file the Climate section shows (#155 P1). Not a light input yet.
struct ClimateState {
    bool scanned = false;
    std::string folder;
    std::vector<std::string> files; // file names in the folder
    int selected = -1;
    std::shared_ptr<const evp::sunstudy::WeatherData> data;
    std::string error;
    float thresholdC = 5.0f;
    int windowDays = 7;
    evp::sunstudy::GrowingSeason season;
};

// The Scheme sub-tab (#278): a mix of catalogue plants placed on one kept
// layer's samples. Held for the session only; nothing is written to the model.
struct SchemeState {
    std::vector<SchemeSpecies> species;
    int layer = -1;     // kept layer the region comes from; -1 = the shown one
    int highlight = -1; // species row the viewer emphasises
    int marker = 0;     // SchemeMarker
    float crownOverlap = 0.0f;
    bool show = true;
    // The region read for `siteStudy`, as classes at the edges it was read with.
    std::string siteStudy;
    std::vector<double> siteEdges;
    std::vector<double> siteHours; // raw, so new edges reclassify without a read
    SchemeSite site;
    std::vector<std::string> siteHosts;
    std::string siteError;
    // The last placement and what it was placed from; `revision` bumps on change.
    SchemeResult result;
    std::string placedFrom;
    uint64_t revision = 0;
};

struct PlantingHud {
    // Run form.
    std::vector<std::string> hosts;   // measured: the planting beds
    std::vector<std::string> context; // casts shadow, not measured
    int season = kActiveGrowth;
    SeasonDates custom;
    int year = 2026;
    bool everyDay = false; // false: Preview (one middle day a month)
    float heights[kHeightCount] = { kDefaultHeights[0], kDefaultHeights[1], kDefaultHeights[2], kDefaultHeights[3] };
    bool heightOn[kHeightCount] = { true, false, true, false };
    int timestep = 2;  // index into the run form's timesteps (15 min)
    float grid = 0.5f; // cell size, metres
    float partialEdge = kDefaultPartial;
    float sunEdge = kDefaultSun;
    bool deepShade = false;
    float deepShadeEdge = kDefaultDeepShade;
    float minHostUp = 0.5f; // steeper hosts are unavailable (cos of the slope)
    int backend = 0;        // 0 CPU, 1 GPU
    // Climate and catalogue.
    int climate = 0; // 0 geometry only; weather-weighted hours wait for #155 P2
    ClimateState climateState;
    CatalogueState catalogue;
    SchemeState scheme;
    std::string profileId = "vilnius-2024";

    // Run queue: heights still to trace, the one running, and the finished ones.
    std::vector<float> pending;
    float runningHeight = -1.0f;
    uint64_t runningRun = 0;
    int queueTotal = 0;
    std::vector<Layer> layers;
    int shownLayer = -1;
    // A kept layer being put back on screen (no tracing): its run and height.
    uint64_t showingRun = 0;
    float showingHeight = -1.0f;
    std::string runError;

    // Display.
    int metric = kSunClass;
    // Class colours, lowest class first: deep shade, shade, partial, sun.
    uint32_t classRgba[kMaxClasses] = { 0x2E4766FFu, 0x4F6D8FFFu, 0xC9B98AFFu, 0xF2C230FFu };
    // Empty: the class swatches above; else a gradient library preset (or
    // kCustomGradientId and classCustom) sampled at each band's centre.
    std::string classPreset;
    bool classFlipped = false;
    std::vector<overlaylayers::ColourStop> classCustom;
    // The hours ramp, when the metric is hours a day.
    // Classes the selected plant accepts (bit per class); others are drawn
    // dimmed. 0: no highlight.
    uint32_t highlightMask = 0;
    std::string hoursPreset = "sun-hours-classic";
    bool hoursFlipped = false;
    std::vector<overlaylayers::ColourStop> hoursCustom;
    // The area table on screen, and what it was read for: the study, its
    // version (a day study converges) and the class edges.
    struct Areas {
        std::string studyId;
        uint64_t version = 0;
        std::vector<double> edges;
        bool period = false;
        evp::sunstudy::PlantingAreaTable table;
        std::vector<std::string> hostGuids; // parallel to table.hosts
        std::string error;
        bool showShares = false; // percent of each row's measured area
    } areas;

    // The Sun tab's display, held while this tab drives the shared tint.
    struct SunDisplayHold {
        bool held = false;
        std::string preset;
        bool flipped = false;
        std::vector<overlaylayers::ColourStop> custom;
        bool stepped = true;
        int bands = 0;
        std::vector<float> edges;
        bool lowBlue = false;
        int view = 0;
    } sunHold;
};

// The ticked heights, ascending, duplicates and out-of-range values dropped.
std::vector<float> ChosenHeights (const PlantingHud& hud);

// The class edges in force, ascending: 2 or 3 of them.
std::vector<float> ClassEdges (const PlantingHud& hud);
int ClassCount (const PlantingHud& hud);
// Edges forced into order with a quarter hour between them, inside 0..24.
void ClampEdges (PlantingHud& hud);
// A class's name, `cls` from 0 (lowest) to ClassCount-1.
const char* ClassName (const PlantingHud& hud, int cls);
// The class of `hours`, exact (`H >= edge`): the reading's, not the tint's.
int ClassOf (const PlantingHud& hud, double hours);
// The class colour, from the deep-shade slot up when deep shade is on.
uint32_t ClassRgba (const PlantingHud& hud, int cls);

// The stepped tint the Sun class metric draws: band edges for the shared
// tint (count - 1 of them, the rest at `rampTop`), and stops that put each
// class colour at its band's centre on the hours ramp (SunStudyRampColour's
// 0.5 h .. 9.5 h), so the stepped tint samples exactly it.
void ClassBands (const PlantingHud& hud, float rampTop, int& count, float edges[], int edgeCapacity);
std::vector<overlaylayers::ColourStop> ClassStops (const PlantingHud& hud, float rampTop);

// The class edges as the area table asks for them.
std::vector<double> AreaEdges (const PlantingHud& hud);
// Whether the table must be read again for `studyId` at `version`.
bool AreasStale (const PlantingHud& hud, const std::string& studyId, uint64_t version);
// A short host label: its 1-based row and the GUID's last four characters.
std::string HostLabel (size_t row, const std::string& guid);

// A layer's caption: its height and, when it is one of the form's heights, that
// slot's name ("0.15 m Ground cover").
std::string LayerName (const PlantingHud& hud, float height);
// The layers for a planting page: those with a display, lowest first, at most
// `limit`.
std::vector<size_t> PageLayers (const PlantingHud& hud, size_t limit);
// "Full sun 35% | Partial shade 41% | Shade 24%" from class areas, highest class
// first; "" when nothing was measured.
std::string ClassShares (const PlantingHud& hud, const std::vector<double>& classAreas);

// Record a layer's whole-layer class areas read at `edges`.
void KeepLayerAreas (PlantingHud& hud, const std::string& studyId, const std::vector<double>& edges,
                     const std::vector<double>& classAreas, double evaluatedArea);
// A colour drawn for a class outside the highlight: towards grey, faded.
uint32_t DimmedRgba (uint32_t rgba);

// The weather file's growing season as form dates; false when there is none
// or it crosses New Year (a custom season stays within one year).
bool SeasonDatesFromWeather (const std::vector<evp::sunstudy::WeatherDay>& days,
                             const evp::sunstudy::GrowingSeason& season, SeasonDates& out);

// How far a weather station is from the project: within 30 km and 50 m of
// height it is near; beyond 150 km or 300 m it is far. Proposed defaults.
enum class StationMatch : int { Unknown, Near, Warn, Far };
StationMatch MatchStation (double distanceM, double heightDifferenceM);

// The layer finished for `height`, or -1.
int LayerAt (const PlantingHud& hud, float height);
// Record a finished layer, replacing an older one at the same height.
void KeepLayer (PlantingHud& hud, float height, const std::string& studyId, bool period, const std::string& label = {});

} // namespace geomsrv::archviz::planting

#endif
