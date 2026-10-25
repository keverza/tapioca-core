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
#include "ArchViz/HudShell.hpp"
#include "ArchViz/InputRingBuffer.hpp"
#include "ArchViz/MatrixMath.hpp"
#include "Geometry/MeshStore.hpp"
#include "NativeCommands/SunStudyHudRunner.hpp"
#include "SunStudy/SunStudyBounds.hpp"

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

// A disabled-colour note that wraps at the panel edge instead of running past it.
void NoteWrapped (const char* text)
{
    ImGui::PushStyleColor (ImGuiCol_Text, ImGui::GetStyleColorVec4 (ImGuiCol_TextDisabled));
    ImGui::TextWrapped ("%s", text);
    ImGui::PopStyleColor ();
}

// The widest label beside a field, so every field leaves room for its label
// at any font size -- a fixed margin cut "timestep" off at the panel's edge.
float LabelColumn ()
{
    float widest = 0.0f;
    for (const char* label : { "date", "timestep", "cell size", "centre", "radius" })
        widest = (std::max) (widest, ImGui::CalcTextSize (label).x);
    return widest + ImGui::GetStyle ().ItemInnerSpacing.x + 2.0f;
}

// A button showing the date; the popup is a month calendar with the solstices
// and the equinox a click away -- the dates a study is usually asked for.
void DatePicker (HudState& state)
{
    char label[32];
    std::snprintf (label, sizeof (label), "%04d-%02d-%02d##sunrundate", state.sunRunYear, state.sunRunMonth,
                   state.sunRunDay);
    if (ImGui::Button (label, ImVec2 (-LabelColumn (), 0.0f)))
        ImGui::OpenPopup ("sunrun-calendar");
    ImGui::SameLine ();
    ImGui::TextUnformatted ("date");
    if (!ImGui::BeginPopup ("sunrun-calendar"))
        return;
    if (ImGui::ArrowButton ("##prev", ImGuiDir_Left) && --state.sunRunMonth < 1) {
        state.sunRunMonth = 12;
        --state.sunRunYear;
    }
    ImGui::SameLine ();
    ImGui::Text ("%-9s %d", kMonths[state.sunRunMonth - 1], state.sunRunYear);
    ImGui::SameLine ();
    if (ImGui::ArrowButton ("##next", ImGuiDir_Right) && ++state.sunRunMonth > 12) {
        state.sunRunMonth = 1;
        ++state.sunRunYear;
    }
    state.sunRunDay = (std::min) (state.sunRunDay, DaysIn (state.sunRunYear, state.sunRunMonth));
    constexpr const char* kDays[] = { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" };
    if (ImGui::BeginTable ("##days", 7, ImGuiTableFlags_SizingFixedSame)) {
        for (const char* name : kDays) {
            ImGui::TableNextColumn ();
            ImGui::TextDisabled ("%s", name);
        }
        for (int blank = Weekday (state.sunRunYear, state.sunRunMonth, 1); blank > 0; --blank)
            ImGui::TableNextColumn ();
        for (int day = 1; day <= DaysIn (state.sunRunYear, state.sunRunMonth); ++day) {
            ImGui::TableNextColumn ();
            char text[8];
            std::snprintf (text, sizeof (text), "%d", day);
            if (ImGui::Selectable (text, day == state.sunRunDay)) {
                state.sunRunDay = day;
                ImGui::CloseCurrentPopup ();
            }
        }
        ImGui::EndTable ();
    }
    const auto preset = [&state] (const char* name, int month, int day) {
        if (ImGui::SmallButton (name)) {
            state.sunRunMonth = month;
            state.sunRunDay = day;
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

const char* StageText (const sunstudyhud::Status& status)
{
    if (status.stage == "snapshot")
        return "Capturing the model...";
    if (status.stage == "starting")
        return "Preparing samples...";
    if (status.stage == "showing")
        return "Publishing to the viewer...";
    return "Queued...";
}

} // namespace

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

    ImGui::BeginDisabled (status.running);
    DatePicker (state);
    ImGui::SetNextItemWidth (-LabelColumn ());
    state.sunRunTimestep = std::clamp (state.sunRunTimestep, 0, kTimestepCount - 1);
    char minutes[16];
    std::snprintf (minutes, sizeof (minutes), "%d min", kTimesteps[state.sunRunTimestep]);
    ImGui::SliderInt ("timestep##sunrun", &state.sunRunTimestep, 0, kTimestepCount - 1, minutes,
                      ImGuiSliderFlags_NoInput);
    ImGui::SetNextItemWidth (-LabelColumn ());
    ImGui::SliderFloat ("cell size##sunrun", &state.sunRunGrid, 0.25f, 10.0f, "%.2f m", ImGuiSliderFlags_Logarithmic);

    if (ImGui::TreeNode ("Roles")) {
        NoteWrapped ("Analysis: everything not in Context or Ignored");
        RoleRow ("Context", state.sunRunContext, state.sunRunIgnored, state.visibilitySelection);
        RoleRow ("Ignored", state.sunRunIgnored, state.sunRunContext, state.visibilitySelection);
        if (state.visibilitySelection.empty ())
            NoteWrapped ("Select elements to assign them a role");
        ImGui::TreePop ();
    }

    ImGui::Checkbox ("Limit to cylinder", &state.sunRunBounded);
    if (state.sunRunBounded) {
        ImGui::SetNextItemWidth (-LabelColumn ());
        double centre[2] = { state.sunRunCentre[0], state.sunRunCentre[1] };
        if (ImGui::DragScalarN ("centre##sunrun", ImGuiDataType_Double, centre, 2, 0.1f, nullptr, nullptr, "%.1f"))
            std::copy (centre, centre + 2, state.sunRunCentre);
        ImGui::SetNextItemWidth (-LabelColumn ());
        ImGui::DragFloat ("radius##sunrun", &state.sunRunRadius, 0.25f, 1.0f, 100000.0f, "%.1f m");
        state.sunRunRadius = (std::max) (state.sunRunRadius, 1.0f);
        if (ImGui::SmallButton (state.sunRunPickingCentre ? "Click the model..." : "Pick centre"))
            state.sunRunPickingCentre = !state.sunRunPickingCentre;
        ImGui::SameLine ();
        ImGui::BeginDisabled (!haveScene);
        if (ImGui::SmallButton ("Centre on scene"))
            CentreOnScene (state, min, max);
        ImGui::EndDisabled ();
        NoteWrapped ("Outside: casts shadow, not measured");
    }
    else
        state.sunRunPickingCentre = false;
    ImGui::EndDisabled ();

    if (!status.running) {
        if (ImGui::Button ("Run sun study##sunrunstart", ImVec2 (-1.0f, 0.0f))) {
            sunstudyhud::Request request;
            request.year = state.sunRunYear;
            request.month = state.sunRunMonth;
            request.day = state.sunRunDay;
            request.timestep = kTimesteps[std::clamp (state.sunRunTimestep, 0, kTimestepCount - 1)];
            request.grid = state.sunRunGrid;
            request.contextElements = state.sunRunContext;
            request.ignoredElements = state.sunRunIgnored;
            request.bounded = state.sunRunBounded;
            request.centreX = state.sunRunCentre[0];
            request.centreY = state.sunRunCentre[1];
            request.radius = state.sunRunRadius;
            state.sunRunPickingCentre = false;
            state.sunRunSubmitError.clear ();
            sunstudyhud::Submit (request, state.sunRunSubmitError);
        }
    }
    else if (status.stage == "calculating" && status.totalSteps > 0) {
        char overlay[48];
        std::snprintf (overlay, sizeof (overlay), "%u / %u steps", unsigned (status.resolvedSteps),
                       unsigned (status.totalSteps));
        ImGui::ProgressBar (float (status.resolvedSteps) / float (status.totalSteps), ImVec2 (-1.0f, 0.0f), overlay);
    }
    else
        ImGui::TextUnformatted (StageText (status));
    if (status.running && ImGui::SmallButton ("Cancel"))
        sunstudyhud::Cancel ();

    if (!state.sunRunSubmitError.empty ())
        hudshell::ErrorNotice ("Not started: " + state.sunRunSubmitError);
    else if (!status.running && status.stage == "failed")
        hudshell::ErrorNotice ("Sun study failed: " + status.error);
    else if (!status.running && status.stage == "cancelled")
        ImGui::TextDisabled ("Cancelled");
    else if (!status.running && status.stage == "done")
        ImGui::TextDisabled ("Done in %.1f s", status.wallMs / 1000.0);
}

void DrawSunStudyBoundsOverlay (const HudState& state, const float viewProj[16], uint32_t width, uint32_t height)
{
    if (!state.sunRunBounded || viewProj == nullptr || width == 0 || height == 0)
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
    const double centre[3] = { state.sunRunCentre[0], state.sunRunCentre[1], state.sunRunFloor };
    ImVec2 dot;
    if (Project (centre, viewProj, width, height, dot))
        draw->AddCircleFilled (dot, 4.0f, colour);
}

void ServiceSunStudyCentrePick (HudState& state, const InputSnapshot& input, const float origin[3],
                                const float direction[3])
{
    if (!state.sunRunPickingCentre || state.wantsMouse || !input.inside || !LeftReleased (input))
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
