#include "ArchViz/PlantingHudModel.hpp"
#include "ArchViz/SunStudyPreview.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::planting {

namespace {

constexpr float kEdgeGap = 0.25f;

bool Leap (int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int DayOfYear (int year, int month, int day)
{
    constexpr int before[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    month = std::clamp (month, 1, 12);
    return before[month - 1] + day + (month > 2 && Leap (year) ? 1 : 0);
}

} // namespace

const char* SeasonName (int season)
{
    switch (season) {
        case kMidsummer:
            return "Midsummer";
        case kActiveGrowth:
            return "Active growth";
        case kExtended:
            return "Extended season";
        case kSpring:
            return "Spring";
        case kSummer:
            return "Summer";
        case kAutumn:
            return "Autumn";
        case kYear:
            return "Whole year";
        default:
            return "Custom";
    }
}

const char* SeasonTip (int season)
{
    switch (season) {
        case kMidsummer:
            return "June 21 alone: the conventional single-day sunlight class. Not a seasonal suitability.";
        case kActiveGrowth:
            return "April 28 to October 1: the Vilnius 10 C climatological season (1991-2020 normals). The default.";
        case kExtended:
            return "April 4 to October 22: the Vilnius 5 C climatological season.";
        case kSpring:
            return "April 1 to May 31.";
        case kSummer:
            return "June 1 to August 31.";
        case kAutumn:
            return "September 1 to October 31.";
        case kYear:
            return "January 1 to December 31: evergreens and year-round use.";
        default:
            return "Any range within one year.";
    }
}

SeasonDates DatesOf (int season, const SeasonDates& custom)
{
    switch (season) {
        case kMidsummer:
            return { 6, 21, 6, 21, true, false };
        case kActiveGrowth:
            return { 4, 28, 10, 1, false, false };
        case kExtended:
            return { 4, 4, 10, 22, false, false };
        case kSpring:
            return { 4, 1, 5, 31, false, false };
        case kSummer:
            return { 6, 1, 8, 31, false, false };
        case kAutumn:
            return { 9, 1, 10, 31, false, false };
        case kYear:
            return { 1, 1, 12, 31, false, true };
        default: {
            SeasonDates dates = custom;
            dates.singleDay = dates.fromMonth == dates.toMonth && dates.fromDay == dates.toDay;
            dates.wholeYear = false;
            return dates;
        }
    }
}

int SeasonDays (const SeasonDates& dates, int year)
{
    const int days =
        DayOfYear (year, dates.toMonth, dates.toDay) - DayOfYear (year, dates.fromMonth, dates.fromDay) + 1;
    return (std::max) (0, days);
}

const char* HeightName (int slot)
{
    constexpr const char* names[kHeightCount] = { "Ground cover", "Low planting", "Shrubs", "Trees" };
    return slot >= 0 && slot < kHeightCount ? names[slot] : "Layer";
}

std::vector<float> ChosenHeights (const PlantingHud& hud)
{
    std::vector<float> heights;
    for (int i = 0; i < kHeightCount; ++i)
        if (hud.heightOn[i] && std::isfinite (hud.heights[i]) && hud.heights[i] >= 0.0f && hud.heights[i] <= kMaxHeight)
            heights.push_back (hud.heights[i]);
    std::sort (heights.begin (), heights.end ());
    heights.erase (
        std::unique (heights.begin (), heights.end (), [] (float a, float b) { return std::fabs (a - b) < 0.005f; }),
        heights.end ());
    return heights;
}

std::vector<float> ClassEdges (const PlantingHud& hud)
{
    std::vector<float> edges;
    if (hud.deepShade)
        edges.push_back (hud.deepShadeEdge);
    edges.push_back (hud.partialEdge);
    edges.push_back (hud.sunEdge);
    return edges;
}

int ClassCount (const PlantingHud& hud)
{
    return int (ClassEdges (hud).size ()) + 1;
}

void ClampEdges (PlantingHud& hud)
{
    hud.sunEdge = std::clamp (hud.sunEdge, 2.0f * kEdgeGap, 24.0f);
    hud.partialEdge = std::clamp (hud.partialEdge, kEdgeGap, hud.sunEdge - kEdgeGap);
    hud.deepShadeEdge = std::clamp (hud.deepShadeEdge, 0.0f, hud.partialEdge - kEdgeGap);
}

const char* ClassName (const PlantingHud& hud, int cls)
{
    const int first = hud.deepShade ? 0 : 1;
    constexpr const char* names[kMaxClasses] = { "Deep shade", "Shade", "Partial shade", "Full sun" };
    const int slot = std::clamp (cls + first, 0, kMaxClasses - 1);
    return names[slot];
}

int ClassOf (const PlantingHud& hud, double hours)
{
    int cls = 0;
    for (float edge : ClassEdges (hud))
        if (hours >= double (edge))
            ++cls;
    return cls;
}

uint32_t ClassRgba (const PlantingHud& hud, int cls)
{
    const int first = hud.deepShade ? 0 : 1;
    return hud.classRgba[std::clamp (cls + first, 0, kMaxClasses - 1)];
}

void ClassBands (const PlantingHud& hud, float rampTop, int& count, float edges[], int edgeCapacity)
{
    const std::vector<float> classEdges = ClassEdges (hud);
    count = int (classEdges.size ()) + 1;
    for (int i = 0; i < edgeCapacity; ++i)
        edges[i] = i < int (classEdges.size ()) ? (std::min) (classEdges[size_t (i)], rampTop) : rampTop;
}

std::vector<overlaylayers::ColourStop> ClassStops (const PlantingHud& hud, float rampTop)
{
    const std::vector<float> edges = ClassEdges (hud);
    std::vector<overlaylayers::ColourStop> stops;
    for (int cls = 0; cls <= int (edges.size ()); ++cls) {
        const float lo = cls > 0 ? (std::min) (edges[size_t (cls - 1)], rampTop) : 0.0f;
        const float hi = cls < int (edges.size ()) ? (std::min) (edges[size_t (cls)], rampTop) : rampTop;
        overlaylayers::ColourStop stop;
        // ⚠️ THE RAMP'S OWN POSITION, not hours / top: SunStudyRampColour (and the
        // tint's SunWarm) put the first stop at 0.5 h and the last at 9.5 h.
        stop.at = std::clamp ((0.5f * (lo + hi) - 0.5f) / (kSunRampTop - 1.0f), 0.0f, 1.0f);
        stop.rgba = ClassRgba (hud, cls);
        stops.push_back (stop);
    }
    return stops;
}

int LayerAt (const PlantingHud& hud, float height)
{
    for (size_t i = 0; i < hud.layers.size (); ++i)
        if (std::fabs (hud.layers[i].height - height) < 0.005f)
            return int (i);
    return -1;
}

void KeepLayer (PlantingHud& hud, float height, const std::string& studyId, bool period)
{
    const int at = LayerAt (hud, height);
    if (at >= 0) {
        hud.layers[size_t (at)].studyId = studyId;
        hud.layers[size_t (at)].period = period;
        hud.shownLayer = at;
        return;
    }
    hud.layers.push_back ({ height, studyId, period });
    std::sort (hud.layers.begin (), hud.layers.end (),
               [] (const Layer& a, const Layer& b) { return a.height < b.height; });
    hud.shownLayer = LayerAt (hud, height);
}

} // namespace geomsrv::archviz::planting
