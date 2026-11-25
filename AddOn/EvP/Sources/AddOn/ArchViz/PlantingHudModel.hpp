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
#include "SunStudy/SunStudyPlantingAreas.hpp"

#include <cstdint>
#include <string>
#include <vector>

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
    // Climate and catalogue: inputs of later stages, shown now.
    int climate = 0; // 0 geometry only, 1 EPW (later)
    std::string epwPath;
    std::string catalogPath;

    // Run queue: heights still to trace, the one running, and the finished ones.
    std::vector<float> pending;
    float runningHeight = -1.0f;
    uint64_t runningRun = 0;
    int queueTotal = 0;
    std::vector<Layer> layers;
    int shownLayer = -1;
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

// The layer finished for `height`, or -1.
int LayerAt (const PlantingHud& hud, float height);
// Record a finished layer, replacing an older one at the same height.
void KeepLayer (PlantingHud& hud, float height, const std::string& studyId, bool period);

} // namespace geomsrv::archviz::planting

#endif
