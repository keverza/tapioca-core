// The viewport panel's "Run sun study" form: what the palette's Native sun
// study asks for, without the palette. Date, timestep and cell size; Context and
// Ignored roles (analysis is everything else); and the analysis cylinder, a size
// safeguard drawn in the view.
//
// ⚠️ ITS OWN TRANSLATION UNIT, for the reason DiligentHudSunStudy.cpp is: the
// HUD files sit near the size cap, and this is one concern -- asking for a
// study, where that file is about reading one.
//
// RENDER THREAD. It reads values and hands a Request to the runner
// (NativeCommands/SunStudyHudRunner), which owns the command sequence and every
// host call.

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentHudSunControls.hpp"
#include "ArchViz/HudGumball.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/InputRingBuffer.hpp"
#include "ArchViz/MatrixMath.hpp"
#include "ArchViz/PointGumball.hpp"
#include "SunStudy/SunPathStudyData.hpp"
#include "Geometry/MeshStore.hpp"
#include "NativeCommands/SunStudyHudRunner.hpp"
#include "ArchViz/TapiocaUI/Foundation/AnalysisRunControl.hpp"
#include "SunStudy/SunStudyBackend.hpp"
#include "SunStudy/SunStudyBounds.hpp"
#include "SunStudy/SunStudyPreset.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace geomsrv {
namespace archviz {

namespace {

// The slider's stops. Finer than 5 min adds steps without changing the hours.
constexpr int kTimesteps[] = { 5, 10, 15, 20, 30 };
constexpr int kTimestepCount = 5;
constexpr const char* kMonths[] = { "January", "February", "March",     "April",   "May",      "June",
                                    "July",    "August",   "September", "October", "November", "December" };

bool Leap (int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int DaysIn (int year, int month)
{
    constexpr int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return month == 2 && Leap (year) ? 29 : days[month - 1];
}

// 0 = Monday. Sakamoto's method.
int Weekday (int year, int month, int day)
{
    constexpr int offsets[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (month < 3)
        --year;
    const int sunday0 = (year + year / 4 - year / 100 + year / 400 + offsets[month - 1] + day) % 7;
    return (sunday0 + 6) % 7;
}

// The widest label beside a field, so every field leaves room for its label
// at any font size -- a fixed margin cut "timestep" off at the panel's edge.
float LabelColumn ()
{
    float widest = 0.0f;
    for (const char* label : { "date", "from", "to", "year", "hours", "timestep", "cell size", "centre", "radius" })
        widest = (std::max) (widest, ImGui::CalcTextSize (label).x);
    return widest + ImGui::GetStyle ().ItemInnerSpacing.x + 2.0f;
}

// A button showing the date; the popup is a month calendar with the solstices
// and the equinox a click away -- the dates a study is usually asked for.
// `lockYear` keeps the year: a period's end shares its start's year.
void DatePicker (const char* id, const char* label, int& year, int& month, int& day, bool lockYear)
{
    ImGui::PushID (id);
    char text[32];
    std::snprintf (text, sizeof (text), "%04d-%02d-%02d##date", year, month, day);
    if (ImGui::Button (text, ImVec2 (-LabelColumn (), 0.0f)))
        ImGui::OpenPopup ("calendar");
    ImGui::SameLine ();
    ImGui::TextUnformatted (label);
    if (!ImGui::BeginPopup ("calendar")) {
        ImGui::PopID ();
        return;
    }
    if (ImGui::ArrowButton ("##prev", ImGuiDir_Left) && --month < 1) {
        month = lockYear ? 1 : 12;
        if (!lockYear)
            --year;
    }
    ImGui::SameLine ();
    ImGui::Text ("%-9s %d", kMonths[month - 1], year);
    ImGui::SameLine ();
    if (ImGui::ArrowButton ("##next", ImGuiDir_Right) && ++month > 12) {
        month = lockYear ? 12 : 1;
        if (!lockYear)
            ++year;
    }
    day = (std::min) (day, DaysIn (year, month));
    constexpr const char* kDays[] = { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" };
    if (ImGui::BeginTable ("##days", 7, ImGuiTableFlags_SizingFixedSame)) {
        for (const char* name : kDays) {
            ImGui::TableNextColumn ();
            ImGui::TextDisabled ("%s", name);
        }
        for (int blank = Weekday (year, month, 1); blank > 0; --blank)
            ImGui::TableNextColumn ();
        for (int d = 1; d <= DaysIn (year, month); ++d) {
            ImGui::TableNextColumn ();
            char number[8];
            std::snprintf (number, sizeof (number), "%d", d);
            if (ImGui::Selectable (number, d == day)) {
                day = d;
                ImGui::CloseCurrentPopup ();
            }
        }
        ImGui::EndTable ();
    }
    const auto preset = [&] (const char* name, int presetMonth, int presetDay) {
        if (ImGui::SmallButton (name)) {
            month = presetMonth;
            day = presetDay;
            ImGui::CloseCurrentPopup ();
        }
    };
    preset ("Mar 21", 3, 21);
    ImGui::SameLine ();
    preset ("Jun 21", 6, 21);
    ImGui::SameLine ();
    preset ("Sep 23", 9, 23);
    ImGui::SameLine ();
    preset ("Dec 21", 12, 21);
    ImGui::EndPopup ();
    ImGui::PopID ();
}

// Days a period covers, for the sampling choice's label.
int PeriodDays (const HudState& state)
{
    if (state.sunRunPeriod == 2)
        return Leap (state.sunRunYear) ? 366 : 365;
    const auto ordinal = [&state] (int month, int day) {
        int total = day;
        for (int m = 1; m < month; ++m)
            total += DaysIn (state.sunRunYear, m);
        return total;
    };
    return ordinal (state.sunRunToMonth, state.sunRunToDay) - ordinal (state.sunRunMonth, state.sunRunDay) + 1;
}

// Single day / Period / Year, the date(s) each needs, and a period's sampling.
void PeriodControls (HudState& state)
{
    ImGui::RadioButton ("Single day##sunrunperiod", &state.sunRunPeriod, 0);
    ImGui::SetItemTooltip ("One date: hours of direct sun that day, with every step's shadows.");
    ImGui::SameLine ();
    ImGui::RadioButton ("Period##sunrunperiod", &state.sunRunPeriod, 1);
    ImGui::SetItemTooltip ("A date range within one year, traced day by day on the same model.");
    ImGui::SameLine ();
    ImGui::RadioButton ("Year##sunrunperiod", &state.sunRunPeriod, 2);
    ImGui::SetItemTooltip ("January 1 to December 31, traced day by day on the same model.");
    if (state.sunRunPeriod == 0) {
        DatePicker ("sunrundate", "date", state.sunRunYear, state.sunRunMonth, state.sunRunDay, false);
        return;
    }
    if (state.sunRunPeriod == 1) {
        DatePicker ("sunrunfrom", "from", state.sunRunYear, state.sunRunMonth, state.sunRunDay, false);
        DatePicker ("sunrunto", "to", state.sunRunYear, state.sunRunToMonth, state.sunRunToDay, true);
        state.sunRunToDay = (std::min) (state.sunRunToDay, DaysIn (state.sunRunYear, state.sunRunToMonth));
        if (PeriodDays (state) < 1)
            hudshell::ErrorNotice ("The period ends before it starts");
    }
    else {
        ImGui::SetNextItemWidth (-LabelColumn ());
        ImGui::InputInt ("year##sunrunyear", &state.sunRunYear, 1, 10);
        state.sunRunYear = std::clamp (state.sunRunYear, 1, 9999);
    }
    const int days = (std::max) (0, PeriodDays (state));
    int sampling = state.sunRunDailyResolved ? 1 : 0;
    ImGui::RadioButton ("Preview##sunrunsampling", &sampling, 0);
    ImGui::SetItemTooltip ("One middle day per month, weighted by the days it stands for. Fast, and an estimate: a "
                           "shadow that comes and goes within a month can be missed.");
    ImGui::SameLine ();
    char every[48];
    std::snprintf (every, sizeof (every), "Every day (%d)##sunrunsampling", days);
    ImGui::RadioButton (every, &sampling, 1);
    ImGui::SetItemTooltip ("Every day of the period traced: the reference. Still limited by the timestep and the "
                           "cell size, and about %d times the preview's work.",
                           (std::max) (1, days / (std::max) (1, (std::min) (12, days))));
    state.sunRunDailyResolved = sampling == 1;
    ImGui::Checkbox ("Keep whole-map days for playback (RAM)##sunrunkeep", &state.sunRunKeepDays);
    ImGui::SetItemTooltip (
        "Keeps every traced day's lit/shaded steps for the WHOLE MAP in memory, so it can be played step by step, at "
        "a fixed hour through the days, or day by day. No file is saved; the archive is released with the period. "
        "Off: only monthly totals are kept and the map player steps month by month.\n\nMemory: about 11 bytes per "
        "cell per traced day at 15-minute steps (100 000 cells over 365 days is about 400 MB). For one cell or study "
        "point, leave this off and use Detailed point playback after the run: it re-traces only that point.");
    SunStudyNote ("Shown as mean hours of direct sun a day. Edits do not rerun it.",
                  "A period is traced on one pinned copy of the model; later edits do not update it.");
}

// The time slice: the hours of each day that count, whole hours, end exclusive.
void HourSlice (HudState& state)
{
    ImGui::SetNextItemWidth (-LabelColumn ());
    ImGui::DragIntRange2 ("hours##sunrunhours", &state.sunRunHourFrom, &state.sunRunHourTo, 0.1f, 0, 24, "from %02d:00",
                          "to %02d:00", ImGuiSliderFlags_AlwaysClamp);
    state.sunRunHourFrom = std::clamp (state.sunRunHourFrom, 0, 23);
    if (state.sunRunHourTo <= state.sunRunHourFrom)
        state.sunRunHourTo = (std::min) (24, state.sunRunHourFrom + 1);
    ImGui::SetItemTooltip ("Only sun in this slice of each day counts; 00:00 to 24:00 is the whole day. Times are "
                           "the project's clock as Archicad gives it.");
}

// One role per element, as the palette command's exclusive sets: putting an
// element in one list takes it out of the other.
void RoleRow (const char* label, std::vector<std::string>& role, std::vector<std::string>& other,
              const std::vector<std::string>& selection)
{
    ImGui::PushID (label);
    ImGui::Text ("%s  %u element(s)", label, unsigned (role.size ()));
    ImGui::BeginDisabled (selection.empty ());
    const auto take = [&] (bool replace) {
        if (replace)
            role.clear ();
        for (const auto& guid : selection) {
            if (std::find (role.begin (), role.end (), guid) == role.end ())
                role.push_back (guid);
            other.erase (std::remove (other.begin (), other.end (), guid), other.end ());
        }
    };
    if (ImGui::SmallButton ("Replace"))
        take (true);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Add"))
        take (false);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Remove"))
        for (const auto& guid : selection)
            role.erase (std::remove (role.begin (), role.end (), guid), role.end ());
    ImGui::EndDisabled ();
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Clear"))
        role.clear ();
    ImGui::PopID ();
}

// The scene's bounds, recomputed once per capture rather than every frame.
//
// ⚠️ THE VIEWER'S SHARED CAPTURE WHEN THERE IS NO SNAPSHOT. Current() is
// published only by BuildSnapshot, which the run itself calls; reading it alone
// left the form "waiting for the model" with the model on screen.
bool SceneBounds (double min[3], double max[3])
{
    static const void* seen = nullptr;
    static uint64_t seenId = 0;
    static bool valid = false;
    static double lo[3] = {}, hi[3] = {};
    auto snapshot = MeshStore::Get ().Current ();
    if (snapshot == nullptr)
        snapshot = MeshStore::Get ().Shared ();
    if (snapshot == nullptr)
        return false;
    if (snapshot.get () != seen || snapshot->id != seenId) {
        seen = snapshot.get ();
        seenId = snapshot->id;
        valid = evp::sunstudy::SnapshotBounds (*snapshot, lo, hi);
    }
    std::copy (lo, lo + 3, min);
    std::copy (hi, hi + 3, max);
    return valid;
}

void CentreOnScene (HudState& state, const double min[3], const double max[3])
{
    state.sunRunCentre[0] = 0.5 * (min[0] + max[0]);
    state.sunRunCentre[1] = 0.5 * (min[1] + max[1]);
    // Half the plan diagonal: the whole scene, so turning the bound on changes
    // nothing until the radius is pulled in.
    state.sunRunRadius = float (0.5 * std::hypot (max[0] - min[0], max[1] - min[1]));
}

bool Project (const double point[3], const float viewProj[16], uint32_t width, uint32_t height, ImVec2& screen)
{
    const float input[4] = { float (point[0]), float (point[1]), float (point[2]), 1.0f };
    float clip[4];
    TransformPoint (clip, input, viewProj);
    if (!std::isfinite (clip[0]) || !std::isfinite (clip[1]) || !std::isfinite (clip[2]) || !std::isfinite (clip[3]) ||
        clip[3] <= 1.0e-6f || clip[2] < 0.0f || clip[2] > clip[3])
        return false;
    screen.x = (clip[0] / clip[3] * 0.5f + 0.5f) * float (width);
    screen.y = (0.5f - clip[1] / clip[3] * 0.5f) * float (height);
    return std::isfinite (screen.x) && std::isfinite (screen.y);
}

bool LeftReleased (const InputSnapshot& input)
{
    for (int i = 0; i < input.transitionCount; ++i)
        if (input.transitions[i].button == kMouseLeft && !input.transitions[i].down)
            return true;
    return false;
}

bool LeftPressed (const InputSnapshot& input)
{
    for (int i = 0; i < input.transitionCount; ++i)
        if (input.transitions[i].button == kMouseLeft && input.transitions[i].down)
            return true;
    return false;
}

// Where a ray meets the scene's floor plane, in plan.
bool FloorPoint (const HudState& state, const double origin[3], const double direction[3], double out[2])
{
    if (std::abs (direction[2]) < 1.0e-9)
        return false;
    const double t = (state.sunRunFloor - origin[2]) / direction[2];
    if (!(t > 0.0))
        return false;
    out[0] = origin[0] + t * direction[0];
    out[1] = origin[1] + t * direction[1];
    return true;
}

// The cylinder's handles: the gumball at the centre of its floor ring (plan
// arrows only -- the cylinder is as tall as the scene, there is no height to
// move), and a radius grip on the ring at +X.
constexpr int kRadiusGrip = 4;

Gumball CylinderGumball (const HudState& state, const float viewProj[16], uint32_t width, uint32_t height)
{
    const double centre[3] = { state.sunRunCentre[0], state.sunRunCentre[1], state.sunRunFloor };
    Gumball gizmo = MakeGumball (centre, viewProj, width, height);
    gizmo.visible[2] = false;
    return gizmo;
}

bool RadiusGrip (const HudState& state, const float viewProj[16], uint32_t width, uint32_t height, ImVec2& at)
{
    const double grip[3] = { state.sunRunCentre[0] + state.sunRunRadius, state.sunRunCentre[1], state.sunRunFloor };
    return Project (grip, viewProj, width, height, at);
}

// The form as a Request. Every input comes from `state`, so a rerun after
// switching the backend changes nothing else.
void SubmitRun (HudState& state)
{
    sunstudyhud::Request request;
    request.year = state.sunRunYear;
    request.month = state.sunRunMonth;
    request.day = state.sunRunDay;
    request.period = state.sunRunPeriod == 2 ? "year" : state.sunRunPeriod == 1 ? "period" : "day";
    request.toMonth = state.sunRunToMonth;
    request.toDay = state.sunRunToDay;
    request.dailyResolved = state.sunRunDailyResolved;
    request.keepDays = state.sunRunPeriod != 0 && state.sunRunKeepDays;
    request.hourFrom = state.sunRunHourFrom;
    request.hourTo = state.sunRunHourTo;
    request.timestep = kTimesteps[std::clamp (state.sunRunTimestep, 0, kTimestepCount - 1)];
    request.grid = state.sunRunGrid;
    request.preset = state.sunRunPreset == 1 ? "late" : "early";
    request.contextElements = state.sunRunContext;
    request.ignoredElements = state.sunRunIgnored;
    request.bounded = state.sunRunBounded;
    request.centreX = state.sunRunCentre[0];
    request.centreY = state.sunRunCentre[1];
    request.radius = state.sunRunRadius;
    request.backend = evp::sunstudy::SunStudyBackendForChoice (state.sunRunBackend);
    state.sunRunPickingCentre = false;
    state.sunRunSubmitError.clear ();
    sunstudyhud::Submit (request, state.sunRunSubmitError);
}

} // namespace

void RunSunStudyDay (HudState& state, int month, int day)
{
    // The form's own settings, for one day: the same grid, hours, step, roles
    // and bounds, so the result belongs to the same scene. The form keeps the
    // period and date it had, so running it again restores them.
    const int period = state.sunRunPeriod, formMonth = state.sunRunMonth, formDay = state.sunRunDay;
    state.sunRunPeriod = 0;
    state.sunRunMonth = month;
    state.sunRunDay = day;
    SubmitRun (state);
    state.sunRunPeriod = period;
    state.sunRunMonth = formMonth;
    state.sunRunDay = formDay;
}

void DrawSunStudySunPathStatus (HudState& state, const std::string& studyId)
{
    sunstudyhud::ReadSunPathStudyDay (studyId, state.sunPathDay);
    const auto& day = state.sunPathDay;
    state.sunShownDateFor = day.available ? studyId : std::string ();
    state.sunShownDate = day.available ? day.year * 10000 + day.month * 100 + day.day : 0;
    if (state.sunPathShown)
        SunStudyNote ("Sun path is display-only",
                      "Use the existing clocks, shadow slider and players to select time. Hovering the sun shows "
                      "its date/time and analysis period; the path never changes the viewer's sun or study inputs.");
}

// The Shadows view of a period (#152 follow-up): which day's shadows. A kept
// day plays straight from the archive through the period player; any other
// date of the period is traced as a single day with the period's own settings.
void DrawSunStudyShadowDate (HudState& state)
{
    // Default and sync: the point player's traced day, else the carrier date.
    const auto profile = sunstudyhud::LatestPeriodProfile ();
    const bool playerDay = profile != nullptr && profile->ready && state.sunPointPlayDay >= 0 &&
                           size_t (state.sunPointPlayDay) < profile->dayMonth.size ();
    if (state.sunShadowMonth == 0) {
        int year = 0, month = 0, day = 0;
        if (std::sscanf (state.sunPeriodCarrierDate.c_str (), "%d-%d-%d", &year, &month, &day) == 3) {
            state.sunShadowMonth = std::clamp (month, 1, 12);
            state.sunShadowDay = std::clamp (day, 1, 31);
        }
        else {
            state.sunShadowMonth = state.sunRunMonth;
            state.sunShadowDay = state.sunRunDay;
        }
    }
    int year = state.sunRunYear;
    DatePicker ("##shadowdate", "shadows on", year, state.sunShadowMonth, state.sunShadowDay, true);
    if (playerDay) {
        const int month = profile->dayMonth[size_t (state.sunPointPlayDay)];
        const int day = profile->dayOfMonth[size_t (state.sunPointPlayDay)];
        if (month != state.sunShadowMonth || day != state.sunShadowDay) {
            char label[48];
            std::snprintf (label, sizeof (label), "Use player day %s %d##shadowsync", kMonths[month - 1], day);
            if (ImGui::SmallButton (label)) {
                state.sunShadowMonth = month;
                state.sunShadowDay = day;
            }
            ImGui::SetItemTooltip ("The study point's detailed playback is on this day.");
        }
    }
    // A kept day needs no trace: show it through the period player.
    int kept = -1;
    if (const auto days = sunstudyhud::KeptPeriodDays (); days != nullptr)
        for (size_t d = 0; d < days->size (); ++d)
            if ((*days)[d].month == state.sunShadowMonth && (*days)[d].day == state.sunShadowDay)
                kept = int (d);
    char text[64];
    if (kept >= 0) {
        std::snprintf (text, sizeof (text), "Show kept %s %d##shadowkept", kMonths[state.sunShadowMonth - 1],
                       state.sunShadowDay);
        if (ImGui::Button (text, ImVec2 (-1.0f, 0.0f))) {
            state.sunPeriodPlayMode = 1; // fixed hour: that day at the shadow clock
            state.sunPeriodDay = kept;
            if (size_t (state.sunStep) < state.sunStepMinutes.size ())
                state.sunPeriodClock = state.sunStepMinutes[size_t (state.sunStep)];
            state.sunPeriodPlay.playing = false;
        }
        ImGui::SetItemTooltip ("The run kept this day's whole-map steps: the period player shows it at the fixed "
                               "hour, lit or shadowed, without a new trace.");
        return;
    }
    std::snprintf (text, sizeof (text), "Trace shadows for %s %d##shadowtrace", kMonths[state.sunShadowMonth - 1],
                   state.sunShadowDay);
    ImGui::BeginDisabled (state.sunRunBusy);
    if (ImGui::Button (text, ImVec2 (-1.0f, 0.0f)))
        RunSunStudyDay (state, state.sunShadowMonth, state.sunShadowDay);
    ImGui::EndDisabled ();
    ImGui::SetItemTooltip ("Traces that one day with the period's settings and shows its shadows, steps and study "
                           "point clock. It replaces the period result on screen; run the period again from the run "
                           "form to return. Keep whole-map days for playback to switch days without a trace.");
}

// What the finished run put on screen, in words: what a colour means, over
// which days and hours, how it was sampled, and what the shadow views show.
static void DrawRunResult (const sunstudyhud::Status& status)
{
    // ⚠️ ONE LINE EACH, the explanation on hover (SunStudyNote): wrapped
    // sentences changed the panel's height with its width and pushed
    // everything below down.
    char line[160];
    if (!status.period) {
        // A follower rerun reports through the same status without a date.
        if (status.dayDate.empty ())
            std::snprintf (line, sizeof (line), "Result: hours of direct sun on the study's day");
        else
            std::snprintf (line, sizeof (line), "Result: hours of direct sun on %s, %s", status.dayDate.c_str (),
                           status.sliceLabel.c_str ());
        SunStudyNote (line, "Sunstudy view: each surface coloured by how long it had an unobstructed view of the sun "
                            "that day. Shadows view: where and when it was in shadow, step by step.");
        return;
    }
    std::snprintf (line, sizeof (line), "Result: mean h a day of direct sun, %s, %s (%u days)",
                   status.periodLabel.c_str (), status.sliceLabel.c_str (), status.representedDays);
    const std::string sampling =
        status.estimate ? "Preview estimate: " + std::to_string (status.periodDays) +
                              " traced days (the middle of each month), each weighted by the days it stands for."
                        : "Every day traced (" + std::to_string (status.periodDays) + " days).";
    const std::string meaning =
        sampling +
        "\n\nSunstudy view: a surface at 4 h received 4 h of unobstructed sun on an average day of the "
        "period; times " +
        std::to_string (status.representedDays) +
        " for the total. Time facing away from the sun counts as no sun, not as shade.\n\nShadows view: the single "
        "traced day " +
        status.carrierDate + ", not the period.";
    SunStudyNote (line, meaning.c_str ());
    std::snprintf (line, sizeof (line), "Mean %.2f h a day (%.0f h total); sun up %.2f h a day (%.0f h)%s",
                   status.meanDailyHours, status.meanTotalHours, status.daylightDailyHours, status.daylightTotalHours,
                   status.estimate ? ", estimate" : "");
    SunStudyNote (line, meaning.c_str ());
}

bool DrawSunStudyRunAction (HudState& state)
{
    using Stage = tapioca::ui::AnalysisRunStage;
    const auto status = sunstudyhud::CurrentJob ();
    const bool active = status.running;
    const bool cancelling = status.cancelling;
    const std::string stage = status.stage;
    tapioca::ui::AnalysisRunView view;
    view.runLabel = status.stage == "failed"  ? "Retry sun study"
                    : state.sunRunPeriod == 2 ? "Run year study"
                    : state.sunRunPeriod == 1 ? "Run period study"
                                              : "Run sun study";
    view.resolved = status.resolvedSteps;
    view.total = status.totalSteps;
    if (cancelling)
        view.stage = Stage::Cancelling;
    else if (active) {
        if (stage == "calculating")
            view.stage = Stage::Calculating;
        else if (stage == "snapshot")
            view.stage = Stage::Capturing;
        else if (stage == "starting" || stage == "preparing")
            view.stage = Stage::Preparing;
        else if (stage == "showing" || stage == "displaying")
            view.stage = Stage::Publishing;
        else
            view.stage = Stage::Waiting;
    }
    // ⚠️ A PERIOD IS ONE RUN ON THE BUTTON: each day passes through capture,
    // preparation and its own steps, and showing those per day made the label
    // flicker and the bar fill hundreds of times. Days done and the overall
    // share instead, until the final publication.
    char periodProgress[48];
    if (active && !cancelling && status.period && status.periodDays > 0 && stage != "showing") {
        const double overall = sunstudyhud::PeriodProgress (status);
        view.stage = Stage::Calculating;
        view.resolved = size_t (overall * 1000.0 + 0.5);
        view.total = 1000;
        std::snprintf (periodProgress, sizeof (periodProgress), "Day %zu / %zu  (%d%%)",
                       (std::min) (status.periodDaysDone + 1, status.periodDays), status.periodDays,
                       int (overall * 100.0));
        view.progressText = periodProgress;
    }
    state.sunRunBusy = active;
    // The finished period, for the study panel's readings (#152).
    if (!active && status.stage == "done" && status.period) {
        state.sunPeriodStudyId = status.studyId;
        state.sunPeriodDays = double (status.representedDays);
        state.sunPeriodLabel = status.periodLabel;
        state.sunPeriodCarrierDate = status.carrierDate;
        state.sunPeriodEstimate = status.estimate;
    }
    else if (!active && status.stage == "done")
        state.sunPeriodStudyId.clear ();
    const bool reopen = state.sunRunWasCancelling && !active;
    state.sunRunWasCancelling = cancelling;
    switch (tapioca::ui::AnalysisRunControl ("##sunrunstart", view)) {
        case tapioca::ui::AnalysisRunAction::Run:
            SubmitRun (state);
            state.sunRunBusy = sunstudyhud::Current ().running;
            break;
        case tapioca::ui::AnalysisRunAction::Cancel:
            sunstudyhud::CancelActive ();
            state.sunRunWasCancelling = true;
            break;
        case tapioca::ui::AnalysisRunAction::None:
            break;
    }
    ImGui::SetItemTooltip (
        "Stops this run. The last complete study stays visible. Following stays paused until Resume.");
    // ⚠️ FIXED HEIGHT WHILE RUNNING: these lines are reserved for the whole
    // run, written or blank, so the panel below never jumps. A note that came
    // and went with every day's capture read as a glitch on a year study.
    if (active) {
        if (status.period)
            ImGui::TextDisabled ("Tracing %s", status.currentDate.empty () ? "..." : status.currentDate.c_str ());
        const bool capturing = stage == "snapshot" || (cancelling && view.total == 0);
        if (status.period)
            ImGui::TextDisabled ("Cancel stops after the current capture or step.");
        else if (capturing)
            ImGui::TextDisabled ("Cancellation waits for any in-flight host capture to return.");
        else
            ImGui::Dummy (ImVec2 (0.0f, ImGui::GetTextLineHeight ()));
    }
    if (status.paused) {
        ImGui::TextDisabled ("Following paused - last complete study retained");
        ImGui::BeginDisabled (active);
        if (ImGui::SmallButton ("Resume following"))
            sunstudyhud::ResumeFollowing (state.sunRunSubmitError);
        ImGui::EndDisabled ();
    }
    if (status.stale)
        ImGui::TextDisabled ("Stale: showing the last complete study");
    if (!state.sunRunSubmitError.empty ())
        hudshell::ErrorNotice ("Not started: " + state.sunRunSubmitError);
    else if (!active && status.stage == "failed")
        hudshell::ErrorNotice ("Sun study failed: " + status.error);
    else if (!active && status.stage == "cancelled")
        ImGui::TextDisabled ("Cancelled");
    else if (!active && status.stage == "done")
        ImGui::TextDisabled ("Done in %.1f s", status.wallMs / 1000.0);
    if (!active && status.stage == "done")
        DrawRunResult (status);
    if (!active && status.stage == "done" && status.period) {
        const sunstudyhud::PeriodInfo period = sunstudyhud::CurrentPeriod ();
        if (period.available && period.studyId == status.studyId) {
            ImGui::BeginDisabled (period.busy);
            const bool clear = ImGui::SmallButton ("Free period cache");
            ImGui::EndDisabled ();
            std::string explanation =
                "Releases the period totals, carrier, point profile and optional whole-map playback archive. The "
                "current frame stays on screen; browsing and point reads need a new run.";
            if (period.daysKept) {
                char bytes[96];
                std::snprintf (bytes, sizeof (bytes), "\nWhole-map playback archive reported by the core: %.2f MiB.",
                               period.keptBytes / (1024.0 * 1024.0));
                explanation += bytes;
                explanation += " This is not the total memory used by the period or renderer.";
            }
            ImGui::SetItemTooltip ("%s", explanation.c_str ());
            if (clear) {
                std::string error;
                if (!sunstudyhud::ClearPeriodCache (error))
                    hudshell::ErrorNotice ("Period cache not freed: " + error);
                else {
                    state.sunPeriodPlay.playing = false;
                    state.sunPointPlay.playing = false;
                    state.sunPeriodProfile = false;
                    state.sunClockBrushActive = state.sunClockBrushDragged = false;
                    state.sunClockBrushId = 0;
                    state.sunClockBrushFrame = -1;
                }
            }
        }
        else if (state.sunShowsPeriod)
            SunStudyNote (
                "Period frame kept; run again to browse it or read a study point.",
                "Free period cache released the totals, carrier, point profile and optional playback archive. "
                "The renderer still owns the published frame and its GPU textures.");
    }
    return reopen;
}

void DrawSunStudyRunControls (HudState& state, const SunStudyOverlayStatus& study)
{
    (void) study;
    double min[3], max[3];
    const bool haveScene = SceneBounds (min, max);
    if (haveScene) {
        state.sunRunFloor = min[2];
        state.sunRunTop = max[2];
        if (!state.sunRunBoundsSeeded) {
            CentreOnScene (state, min, max);
            state.sunRunBoundsSeeded = true;
        }
    }
    const sunstudyhud::Status status = sunstudyhud::Current ();

    ImGui::BeginDisabled (state.sunRunBusy);
    PeriodControls (state);
    HourSlice (state);
    ImGui::SetNextItemWidth (-LabelColumn ());
    state.sunRunTimestep = std::clamp (state.sunRunTimestep, 0, kTimestepCount - 1);
    char minutes[16];
    std::snprintf (minutes, sizeof (minutes), "%d min", kTimesteps[state.sunRunTimestep]);
    ImGui::SliderInt ("timestep##sunrun", &state.sunRunTimestep, 0, kTimestepCount - 1, minutes,
                      ImGuiSliderFlags_NoInput);
    ImGui::SetNextItemWidth (-LabelColumn ());
    ImGui::SliderFloat ("cell size##sunrun", &state.sunRunGrid, 0.25f, 10.0f, "%.2f m", ImGuiSliderFlags_Logarithmic);

    ImGui::RadioButton ("Early model##sunrun", &state.sunRunPreset, 0);
    ImGui::SetItemTooltip ("Early measures all eligible faces.");
    ImGui::SameLine ();
    ImGui::RadioButton ("Late model##sunrun", &state.sunRunPreset, 1);
    ImGui::SetItemTooltip ("Late measures faces with at least 40%% transparency; other faces still cast shadow.");
    auto snapshot = MeshStore::Get ().Current ();
    const uint64_t captureStamp = MeshStore::Get ().CaptureStamp ();
    if (snapshot == nullptr || snapshot->scope != "all" || snapshot->captureStamp != captureStamp ||
        !snapshot->completeModel)
        snapshot = MeshStore::Get ().Shared ();
    const size_t triangleCount = snapshot != nullptr && snapshot->scope == "all" &&
                                         snapshot->captureStamp == captureStamp && snapshot->completeModel
                                     ? snapshot->TotalTriangles ()
                                     : 0;
    const std::string advice =
        evp::sunstudy::SunStudyPresetAdvice (triangleCount, state.sunRunPreset == 1 ? "late" : "early");
    if (!advice.empty ())
        SunStudyNote (advice.c_str ());

    ImGui::RadioButton ("CPU##sunrunbackend", &state.sunRunBackend, 0);
    ImGui::SetItemTooltip ("The exact CPU tracer, on every machine. Edits rerun on CPU too.");
    ImGui::SameLine ();
    ImGui::RadioButton ("GPU##sunrunbackend", &state.sunRunBackend, 1);
    ImGui::SetItemTooltip ("FP64 D3D11 compute with exact CPU checks. Without a capable GPU the study finishes on CPU "
                           "and says why.");

    // ⚠️ "##sunrunroles": the view selector below the run form has a "Roles"
    // button in the same window, and the two labels hashed to one ID (#116).
    const bool rolesOpen = ImGui::TreeNode ("Roles##sunrunroles");
    ImGui::SetItemTooltip ("Analysis: everything not in Context or Ignored. Select elements to assign them a role.");
    if (rolesOpen) {
        RoleRow ("Context", state.sunRunContext, state.sunRunIgnored, state.visibilitySelection);
        RoleRow ("Ignored", state.sunRunIgnored, state.sunRunContext, state.visibilitySelection);
        ImGui::TreePop ();
    }

    ImGui::Checkbox ("Limit to cylinder", &state.sunRunBounded);
    if (state.sunRunBounded) {
        if (ImGui::Checkbox ("Show cylinder", &state.sunRunShowBounds) && !state.sunRunShowBounds) {
            state.sunRunPickingCentre = false;
            state.sunRunDragging = false;
        }
        ImGui::SetNextItemWidth (-LabelColumn ());
        double centre[2] = { state.sunRunCentre[0], state.sunRunCentre[1] };
        if (ImGui::DragScalarN ("centre##sunrun", ImGuiDataType_Double, centre, 2, 0.1f, nullptr, nullptr, "%.1f"))
            std::copy (centre, centre + 2, state.sunRunCentre);
        ImGui::SetNextItemWidth (-LabelColumn ());
        ImGui::DragFloat ("radius##sunrun", &state.sunRunRadius, 0.25f, 1.0f, 100000.0f, "%.1f m");
        state.sunRunRadius = (std::max) (state.sunRunRadius, 1.0f);
        if (state.sunRunShowBounds) {
            if (ImGui::SmallButton (state.sunRunPickingCentre ? "Click the model..." : "Pick centre"))
                state.sunRunPickingCentre = !state.sunRunPickingCentre;
            ImGui::SetItemTooltip ("Only faces inside are measured; everything still casts shadow. Drag the arrows "
                                   "or disc to move it, the grip on the ring to resize.");
            ImGui::SameLine ();
            ImGui::BeginDisabled (!haveScene);
            if (ImGui::SmallButton ("Centre on scene"))
                CentreOnScene (state, min, max);
            ImGui::EndDisabled ();
        }
    }
    else
        state.sunRunPickingCentre = false;
    ImGui::EndDisabled ();

    if (!status.requestedBackend.empty () && (status.running || status.stage == "done")) {
        evp::sunstudy::SunStudyBackendReport report;
        report.requested = status.requestedBackend;
        report.resolved = status.resolvedBackend;
        report.fallbackReason = status.backendFallbackReason;
        const std::string backend = "Backend: " + evp::sunstudy::DescribeSunStudyBackend (report);
        if (report.fallbackReason.empty ())
            ImGui::TextDisabled ("%s", backend.c_str ());
        else {
            hudshell::ErrorNotice (backend);
            ImGui::TextDisabled ("Choose CPU above to rerun without fallback.");
        }
    }
}

void DrawSunStudyBoundsOverlay (const HudState& state, const float viewProj[16], uint32_t width, uint32_t height)
{
    if (!state.sunRunBounded || !state.sunRunShowBounds || viewProj == nullptr || width == 0 || height == 0)
        return;
    constexpr int kSegments = 64;
    const double z[2] = { state.sunRunFloor, (std::max) (state.sunRunTop, state.sunRunFloor + 0.01) };
    ImVec2 ring[2][kSegments];
    bool visible[2][kSegments];
    for (int level = 0; level < 2; ++level)
        for (int i = 0; i < kSegments; ++i) {
            const double angle = 2.0 * 3.14159265358979323846 * double (i) / double (kSegments);
            const double point[3] = { state.sunRunCentre[0] + state.sunRunRadius * std::cos (angle),
                                      state.sunRunCentre[1] + state.sunRunRadius * std::sin (angle), z[level] };
            visible[level][i] = Project (point, viewProj, width, height, ring[level][i]);
        }
    ImDrawList* draw = ImGui::GetBackgroundDrawList ();
    const ImU32 colour = IM_COL32 (245, 158, 36, 220); // the roles view's analysis orange
    for (int level = 0; level < 2; ++level)
        for (int i = 0; i < kSegments; ++i) {
            const int next = (i + 1) % kSegments;
            if (visible[level][i] && visible[level][next])
                draw->AddLine (ring[level][i], ring[level][next], colour, 2.0f);
        }
    for (int i = 0; i < kSegments; i += kSegments / 8)
        if (visible[0][i] && visible[1][i])
            draw->AddLine (ring[0][i], ring[1][i], colour, 1.0f);
    DrawGumball (CylinderGumball (state, viewProj, width, height), ImGui::GetForegroundDrawList ());
    ImVec2 grip;
    if (RadiusGrip (state, viewProj, width, height, grip)) {
        ImGui::GetForegroundDrawList ()->AddCircleFilled (grip, 6.0f, colour);
        ImGui::GetForegroundDrawList ()->AddCircle (grip, 6.0f, IM_COL32 (40, 40, 40, 220), 0, 1.5f);
    }
}

void ServiceSunStudyBoundsGumball (HudState& state, const InputSnapshot& input, const float viewProj[16],
                                   uint32_t width, uint32_t height, const float rayOrigin[3],
                                   const float rayDirection[3])
{
    if (state.sunRunBusy || !state.sunRunBounded || !state.sunRunShowBounds || state.readOnly || viewProj == nullptr ||
        width == 0 || height == 0) {
        state.sunRunDragging = false;
        return;
    }
    const double origin[3] = { rayOrigin[0], rayOrigin[1], rayOrigin[2] };
    const double direction[3] = { rayDirection[0], rayDirection[1], rayDirection[2] };
    const float x = float (CursorTargetX (input, width)), y = float (CursorTargetY (input, height));
    int hit = -1;
    if (input.inside) {
        ImVec2 grip;
        if (RadiusGrip (state, viewProj, width, height, grip) && std::hypot (x - grip.x, y - grip.y) <= 9.0f)
            hit = kRadiusGrip;
        else
            hit = HitGumball (CylinderGumball (state, viewProj, width, height), x, y);
    }
    if (!state.sunRunDragging && !state.wantsMouse && hit >= 0 && LeftPressed (input)) {
        state.sunRunDragAxis = hit;
        state.sunRunDragPoint[0] = state.sunRunCentre[0];
        state.sunRunDragPoint[1] = state.sunRunCentre[1];
        state.sunRunDragPoint[2] = state.sunRunFloor;
        state.sunRunDragging = hit == kRadiusGrip || PointGumballParameter (state.sunRunDragPoint, hit, origin,
                                                                            direction, state.sunRunDragParameter);
        state.sunRunPickingCentre = false;
    }
    // Navigation must not take the same press: the viewpoint gumball's flag is
    // the one NavigateViewer already honours.
    if (state.sunRunDragging || (!state.wantsMouse && hit >= 0 && (input.buttons & kMouseLeft)))
        state.viewpointOwnsMouse = true;
    if (!state.sunRunDragging)
        return;
    if (state.sunRunDragAxis == kRadiusGrip) {
        double floor[2];
        if (FloorPoint (state, origin, direction, floor))
            state.sunRunRadius =
                (std::max) (1.0f,
                            float (std::hypot (floor[0] - state.sunRunCentre[0], floor[1] - state.sunRunCentre[1])));
    }
    else {
        double parameter[3];
        if (PointGumballParameter (state.sunRunDragPoint, state.sunRunDragAxis, origin, direction, parameter)) {
            state.sunRunCentre[0] = state.sunRunDragPoint[0] + parameter[0] - state.sunRunDragParameter[0];
            state.sunRunCentre[1] = state.sunRunDragPoint[1] + parameter[1] - state.sunRunDragParameter[1];
        }
    }
    if (LeftReleased (input) || !(input.buttons & kMouseLeft))
        state.sunRunDragging = false;
}

void ServiceSunStudyCentrePick (HudState& state, const InputSnapshot& input, const float origin[3],
                                const float direction[3])
{
    if (state.sunRunBusy || !state.sunRunBounded || !state.sunRunShowBounds || !state.sunRunPickingCentre ||
        state.wantsMouse || !input.inside || !LeftReleased (input))
        return;
    // On the scene's floor plane: the centre is a plan position, and the floor
    // is under every click that looks down at the model.
    if (std::abs (direction[2]) < 1.0e-6f)
        return;
    const double t = (state.sunRunFloor - double (origin[2])) / double (direction[2]);
    if (!(t > 0.0))
        return;
    state.sunRunCentre[0] = double (origin[0]) + t * double (direction[0]);
    state.sunRunCentre[1] = double (origin[1]) + t * double (direction[1]);
    state.sunRunPickingCentre = false;
}

} // namespace archviz
} // namespace geomsrv
