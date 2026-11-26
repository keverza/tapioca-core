#include "NativePreview.hpp"
#include "ArchViz/HudVirtualEditorDebug.hpp"
#include "ArchViz/VisibilityStudyCapture.hpp"
#include "ArchViz/ViewpointBake.hpp"
#include "ArchViz/AnalysisPathSource.hpp"
#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/PlantingZoneSource.hpp"
#include "SunStudy/SunPathStudyData.hpp"
#include "SunStudy/SunStudyKernels.hpp"
#include "SunStudy/SunStudyLimits.hpp"
#include "SunStudy/SunStudyWeather.hpp"
#include <algorithm>
#include <cmath>
#include "Geometry/MeshStore.hpp"
#include <stdexcept>

namespace tapioca::playground {
namespace {
MockServices* s_current = nullptr;
}
MockServices& Services ()
{
    if (!s_current)
        throw std::logic_error ("Mock service called outside playground Draw");
    return *s_current;
}
MockScope::MockScope (MockServices& services) : previous (s_current)
{
    s_current = &services;
}
MockScope::~MockScope ()
{
    s_current = previous;
}
void LogMock (const std::string& text)
{
    Event (*Services ().document, "Mock owner: " + text);
}
} // namespace tapioca::playground
namespace pg = tapioca::playground;
namespace evp::sunstudy::kernels {
// The playground has no analysis worker or machine budget for real studies.
AnalysisLimitsReport ReportAnalysisLimits (const MachineResources&, size_t, double, double, std::string*)
{
    return {};
}
std::string SunStudyPresetAdvice (size_t, const std::string&, std::string*)
{
    return {};
}
// The run form's step count for the limits readout (#117): the same slice
// enumeration, without the kernel library the playground does not link.
std::vector<evp::sunstudy::TimeOfDay> EnumerateTimesteps (int timestepMinutes, int hourFrom, int hourTo, std::string*)
{
    std::vector<evp::sunstudy::TimeOfDay> times;
    if (timestepMinutes <= 0)
        return times;
    for (int minute = hourFrom * 60; minute < hourTo * 60; minute += timestepMinutes)
        times.push_back ({ minute / 60, minute % 60 });
    return times;
}
std::vector<uint32_t> FanSteps (const std::vector<uint16_t>& minutes, double interval, std::string*)
{
    std::vector<uint32_t> steps;
    double next = 0;
    for (uint32_t i = 0; i < minutes.size (); ++i) {
        if (minutes[i] >= next) {
            steps.push_back (i);
            next = minutes[i] + interval;
        }
    }
    return steps;
}
} // namespace evp::sunstudy::kernels
namespace geomsrv::sunstudyhud {
bool Submit (const Request& request, std::string& error, uint64_t* acceptedRun)
{
    auto& m = pg::Services ();
    error.clear ();
    m.sun.stage = "done";
    m.sun.studyId = "fixture-sun";
    ++m.sun.runs;
    if (acceptedRun)
        *acceptedRun = m.sun.runs;
    char date[24];
    std::snprintf (date, sizeof (date), "%04d-%02d-%02d", request.year, request.month, request.day);
    m.sun.dayDate = date;
    m.scene.sunStudy.drawing = true;
    m.scene.sunStudy.analysisKind = 0;
    m.scene.sunStudy.studyId = "fixture-sun";
    pg::LogMock ("Run sun study -> deterministic fixture; no analysis worker");
    return true;
}
void Cancel ()
{
    pg::Services ().sun.running = false;
    pg::LogMock ("Cancel sun study");
}
Status Current ()
{
    return pg::Services ().sun;
}
// The HUD's one control owner: the mock has no follower, so the manual status is the job's.
Status CurrentJob ()
{
    return pg::Services ().sun;
}
void CancelActive ()
{
    Cancel ();
}
void CancelRun (uint64_t run)
{
    if (pg::Services ().sun.runs == run)
        Cancel ();
}
bool ResumeFollowing (std::string& error)
{
    error = "No follower in the playground.";
    return false;
}
// No period run is ever kept: the period views show their empty state.
PeriodInfo CurrentPeriod ()
{
    return {};
}
std::shared_ptr<const std::vector<KeptDay>> KeptPeriodDays ()
{
    return nullptr;
}
std::shared_ptr<const PeriodProfile> LatestPeriodProfile ()
{
    return nullptr;
}
bool ShowPeriodView (const PeriodView&, std::string& error)
{
    error = "No period run in the playground.";
    return false;
}
bool ReadPeriodPoints (const std::vector<size_t>&, PeriodPoint&)
{
    return false;
}
void RequestPeriodProfile (const std::vector<size_t>&)
{
}
// Kept planting layers (#278): the mock keeps none, so a layer click re-traces
// through the deterministic Submit above.
bool IsPlantingLayerKept (const std::string&)
{
    return false;
}
void ForgetPlantingLayers ()
{
    pg::LogMock ("Forget planting layers (mock; nothing kept)");
}
// The Planting tab's area table (#278): a fixed two-host courtyard, the west bed
// partly under a balcony. No store or period is read.
bool ReadPlantingAreas (const std::string& studyId, const std::vector<double>& edges, PlantingAreas& out,
                        std::string& error)
{
    error.clear ();
    out = PlantingAreas ();
    out.studyId = studyId;
    out.period = true;
    const size_t classes = edges.size () + 1;
    const auto row = [classes] (uint32_t group, std::vector<double> areas, double steep, double blocked) {
        evp::sunstudy::PlantingAreaRow r;
        r.group = group;
        areas.resize (classes, 0.0);
        r.classAreas = areas;
        for (double a : areas)
            r.evaluatedArea += a;
        r.steepArea = steep;
        r.blockedArea = blocked;
        return r;
    };
    out.table.hosts = { row (0, { 122.0, 190.0, 108.0 }, 2.0, 3.0), row (1, { 0.0, 12.0, 64.0 }, 0.0, 0.0) };
    out.table.total = row (0, { 122.0, 202.0, 172.0 }, 2.0, 3.0);
    out.hostGuids = { "FIXTURE-COURTYARD-BED-0A1B", "FIXTURE-ROOF-TERRACE-9C2D" };
    pg::LogMock ("Read planting areas -> fixed courtyard fixture; no store read");
    return true;
}
// The planting scheme's region (#278): the same courtyard as a 20 x 12 m bed at
// 0.5 m, shaded towards its north edge. No store is read.
bool ReadPlantingSite (const std::string& studyId, double layerHeight, PlantingSite& out, std::string& error)
{
    error.clear ();
    out = PlantingSite ();
    out.studyId = studyId;
    for (int i = 0; i < 40; ++i)
        for (int j = 0; j < 24; ++j) {
            const double x = 0.25 + 0.5 * i, y = 0.25 + 0.5 * j;
            out.x.push_back (x);
            out.y.push_back (y);
            out.groundZ.push_back (0.0);
            out.hours.push_back (1.0 + 8.0 * (1.0 - y / 12.0) + 0.05 * (x - 10.0));
            out.groups.push_back (x < 14 ? 0u : 1u);
            out.areas.push_back (0.25);
        }
    out.meshGuids = { "FIXTURE-COURTYARD-BED-0A1B", "FIXTURE-ROOF-TERRACE-9C2D" };
    pg::LogMock ("Read planting site -> fixed courtyard bed; layer " + std::to_string (layerHeight) + " m");
    return true;
}
// The courtyard's room (#278 Stage 2): walls on all four edges, and a balcony
// 3 m deep along the north edge with its underside at 2.8 m. Ready at once.
namespace {
std::string s_roomStudy;
}
void RequestPlantingRoom (const std::string& studyId, double)
{
    s_roomStudy = studyId;
}
PlantingRoom CurrentPlantingRoom ()
{
    static std::shared_ptr<const evp::sunstudy::CrownClearance> s_room = [] {
        PlantingSite site;
        std::string error;
        ReadPlantingSite ("mock", 0.5, site, error);
        auto room = std::make_shared<evp::sunstudy::CrownClearance> ();
        const evp::sunstudy::CrownClearanceOptions options;
        room->levels = options.levels;
        room->reach = options.reach;
        room->directions = options.directions;
        for (size_t i = 0; i < site.x.size (); ++i) {
            const double x = site.x[i], y = site.y[i];
            const bool underBalcony = y > 9.0;
            room->overhead.push_back (underBalcony ? 2.8f : -1.0f);
            const double wall = (std::min) ((std::min) (x, 20.0 - x), (std::min) (y, 12.0 - y));
            for (double level : room->levels)
                room->free.push_back (underBalcony && level >= 2.8 ? 0.0f : float ((std::min) (wall, room->reach)));
        }
        return std::shared_ptr<const evp::sunstudy::CrownClearance> (std::move (room));
    }();
    PlantingRoom room;
    room.state = PlantingRoom::State::Ready;
    room.studyId = s_roomStudy;
    room.clearance = s_room;
    return room;
}
bool ClearPeriodCache (std::string& error)
{
    error.clear ();
    pg::LogMock ("Clear period cache (mock; nothing kept)");
    return true;
}
} // namespace geomsrv::sunstudyhud
namespace geomsrv::archviz::plantinghud {
// The playground has no scene: the scheme's viewer geometry is not built.
void PublishSchemeScene (HudState&)
{
}
} // namespace geomsrv::archviz::plantinghud
namespace geomsrv::archviz::plantingzonesource {
// Zones from polylines (#278): no Archicad selection here. An import returns two
// fixed beds over the mock courtyard: a rectangle and an L with a hole.
namespace {
Imported s_zones;
}
void RequestImport ()
{
    planting::PlantingZone bed;
    bed.guid = "FIXTURE-ZONE-BED-1111";
    bed.name = "Bed A";
    bed.rings = { { 1, 1, 7, 1, 7, 5, 1, 5 } };
    planting::PlantingZone lawn;
    lawn.guid = "FIXTURE-ZONE-LAWN-2222";
    lawn.name = "Lawn";
    lawn.kind = "Fill";
    lawn.rings = { { 9, 2, 19, 2, 19, 11, 14, 11, 14, 6, 9, 6 }, { 15, 3, 17, 3, 17, 5, 15, 5 } };
    s_zones.zones = { bed, lawn };
    s_zones.error.clear ();
    ++s_zones.serial;
    pg::LogMock ("Zones from polylines -> two fixed beds");
}
Imported Latest ()
{
    return s_zones;
}
} // namespace geomsrv::archviz::plantingzonesource
namespace evp::sunstudy {
// EPW weather (#155 P1): the Playground has no Rust parser. Any picked file
// reads as one synthetic Vilnius-like year, so the Climate section can be laid
// out offline; nothing on disk is parsed.
bool ReadWeatherFile (const std::filesystem::path& path, WeatherData& out, std::string& error)
{
    error.clear ();
    out = WeatherData ();
    auto& id = out.identity;
    id.city = "Synthetic station";
    id.country = "LTU";
    id.source = "Playground mock";
    id.wmo = "000000";
    id.latitude = 54.6364;
    id.longitude = 25.2908;
    id.timeZoneHours = 2.0;
    id.elevationM = 196.9;
    id.sha256 = std::string (64, '0');
    id.missing[size_t (WeatherField::DiffuseHorizontal)] = 12;
    static constexpr int kDays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int n = 0;
    for (int m = 0; m < 12; ++m) {
        auto& month = out.months[size_t (m)];
        month.records = size_t (kDays[m]) * 24;
        double sum = 0;
        for (int d = 1; d <= kDays[m]; ++d, ++n) {
            const double t = 7.0 - 12.0 * std::cos (n / 365.0 * 6.283185307);
            out.days.push_back ({ uint8_t (m + 1), uint8_t (d), t, 0.0, 0 });
            sum += t;
        }
        month.meanDryBulbC = sum / kDays[m];
        month.minDryBulbC = month.meanDryBulbC - 6.0;
        month.maxDryBulbC = month.meanDryBulbC + 6.0;
        month.globalHorizontalKwhM2 = 10.0 + 150.0 * std::max (0.0, std::sin ((m + 0.5) / 12.0 * 3.14159265));
    }
    out.records.resize (8760);
    tapioca::playground::LogMock ("Read weather file (mock): " + path.filename ().string ());
    return true;
}
bool FindGrowingSeason (const std::vector<WeatherDay>& days, int windowDays, double thresholdC, bool,
                        GrowingSeason& out, std::string& error)
{
    error.clear ();
    out = GrowingSeason ();
    if (windowDays < 1 || windowDays % 2 == 0) {
        error = "WeatherSeasonRule";
        return false;
    }
    size_t run = 0;
    for (size_t i = 0; i < days.size (); ++i) {
        run = days[i].meanDryBulbC >= thresholdC ? run + 1 : 0;
        if (run > 0 && run > (out.found ? out.last - out.first + 1 : 0))
            out = { true, i + 1 - run, i };
    }
    return true;
}
bool StationDistance (double latA, double lonA, double latB, double lonB, double& metres, std::string& error)
{
    error.clear ();
    const double k = 3.14159265358979 / 180.0;
    const double h = std::pow (std::sin ((latB - latA) * k / 2), 2) +
                     std::cos (latA * k) * std::cos (latB * k) * std::pow (std::sin ((lonB - lonA) * k / 2), 2);
    metres = 2.0 * 6371008.8 * std::asin (std::min (1.0, std::sqrt (h)));
    return true;
}
} // namespace evp::sunstudy
namespace geomsrv::sunstudyhud {
// No completed study is stored here: the sun path has no day to show.
bool ReadSunPathStudyDay (const std::string&, SunPathStudyDay&)
{
    return false;
}
} // namespace geomsrv::sunstudyhud
namespace geomsrv::archviz {
void ArchVizLog (const std::string& message)
{
    pg::LogMock (message);
}
// The period, point and obstruction views read the study store and its workers, which the
// playground does not link; their place in the Analysis page says so.
void DrawSunStudyPeriodView (HudState&)
{
    ImGui::TextDisabled ("Period view: needs a stored study (not in the playground).");
}
void DrawSunStudyPointDay (HudState&, const std::string&, uint64_t)
{
    ImGui::TextDisabled ("Point day: needs a stored study (not in the playground).");
}
void DrawSunStudyObstructionControls (HudState&, const std::string&)
{
    ImGui::TextDisabled ("Obstruction: needs the study workers (not in the playground).");
}
namespace edithud {
bool RequestDebugExport (Session& s)
{
    s.debugExportRequested = false;
    s.message = "Mock export recorded; no file written.";
    pg::LogMock (s.message);
    return true;
}
} // namespace edithud
namespace viewpointstudy {
void Configure (Settings settings)
{
    settings.revision = pg::Services ().viewpoint.revision + 1;
    pg::Services ().viewpoint = std::move (settings);
}
std::shared_ptr<const Settings> GetSettings ()
{
    return std::make_shared<const Settings> (pg::Services ().viewpoint);
}
Status GetStatus ()
{
    return pg::Services ().viewpointStatus;
}
} // namespace viewpointstudy
namespace visibilitystudy {
bool Submit (Request request, std::string& error)
{
    auto& m = pg::Services ();
    error.clear ();
    m.visibility.complete = true;
    m.visibility.running = false;
    m.visibility.studyId = "fixture-visibility";
    m.visibility.origin = int (request.options.origin);
    m.visibility.meanVisibility = 0.64;
    m.visibility.sampleCount = 256;
    m.scene.sunStudy.drawing = true;
    m.scene.sunStudy.analysisKind = 1;
    m.scene.sunStudy.studyId = m.visibility.studyId;
    m.hud.analysisFocus.shownOrigin = m.visibility.origin;
    pg::LogMock ("Run visibility -> deterministic 64% fixture; no rays traced");
    return true;
}
Status GetStatus ()
{
    return pg::Services ().visibility;
}
void Cancel ()
{
    pg::Services ().visibility.running = false;
    pg::LogMock ("Cancel visibility");
}
void ClearDisplay ()
{
    pg::Services ().scene.sunStudy.drawing = false;
    pg::Services ().visibility = {};
    pg::LogMock ("Clear visibility");
}
} // namespace visibilitystudy
namespace analysispathsource {
void RequestImport ()
{
    auto& h = pg::Services ().hud;
    h.analysisPath = std::make_shared<analysispath::Path> (
        analysispath::Build ("fixture-path", { 0, 0, 12, 0, 12, 8 }, {}, false, 0));
    h.analysisProfile = analysisprofile::Flat (h.analysisPath->Length (), 1.6);
    pg::LogMock ("Import analysis path -> deterministic fixture; no Archicad read");
}
void RequestSave (std::string, analysisprofile::Profile)
{
    pg::Services ().hud.analysisProfileDirty = false;
    pg::Services ().hud.analysisProfileNote = "Saved in mock owner only.";
    pg::LogMock ("Save analysis profile (mock; no project write)");
}
void RequestRefresh (std::string, uint64_t)
{
}
Imported Latest ()
{
    return {};
}
Saved LastSave ()
{
    return {};
}
} // namespace analysispathsource
namespace viewpointbake {
void RequestBake (evp::sunstudy::ViewpointPlanContour)
{
    pg::LogMock ("Bake viewpoint (mock; no Fill created)");
}
Baked LastBake ()
{
    return {};
}
} // namespace viewpointbake
} // namespace geomsrv::archviz
