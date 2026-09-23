// The viewport panel's "sun study" section -- the web study's hours-range filter
// and legend, beside the model they describe.
//
// ⚠️ ITS OWN TRANSLATION UNIT because DiligentHud.cpp sits at the size cap, and
// because this is one concern: what the tint on screen means, and which part of
// it is shown. AnnotationHudControls is the precedent.
//
// RENDER THREAD, like the rest of the HUD. It reads the scene's overlay status
// (what the renderer is drawing) and writes HudState; the frame loop hands the
// range to DiligentScene::SetSunStudyFilter.

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/SunStudyOverlay.hpp"
#include "Geometry/MeshStore.hpp"
#include "Geometry/QueryEngine.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <string>

namespace geomsrv {
namespace archviz {

namespace {

// The web study's SUN_COLORS (Commands/SunStudy/sunpalette.py), sRGB, as the
// tint shader's kSunBins. ⚠️ THE SAME TEN, OR THE LEGEND LIES ABOUT THE MODEL.
constexpr unsigned kSunBinColours[10] = { 0x6b3d18, 0x8a4f1f, 0x9c5a23, 0xb06a28, 0xc07d33,
                                          0xcf8f44, 0xdca157, 0xe7b674, 0xf0cb96, 0xf7e3c2 };
constexpr const char* kSunBinLabels[10] = { "0 - 1 h", "1 - 2 h", "2 - 3 h", "3 - 4 h", "4 - 5 h",
                                            "5 - 6 h", "6 - 7 h", "7 - 8 h", "8 - 9 h", "9+ h" };

ImVec4 Rgb (unsigned hex)
{
    return ImVec4 (float ((hex >> 16) & 0xff) / 255.0f, float ((hex >> 8) & 0xff) / 255.0f, float (hex & 0xff) / 255.0f,
                   1.0f);
}

// The web slider's step. Finer than any timestep a study is run at, so a range
// edge can sit exactly on a value the study can report.
float Snap (float hours)
{
    return std::round (hours * 4.0f) / 4.0f;
}

void Swatch (const char* id, const ImVec4& colour, const char* tooltip)
{
    ImGui::ColorButton (id, colour, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                        ImVec2 (16.0f, 16.0f));
    if (ImGui::IsItemHovered ())
        ImGui::SetTooltip ("%s", tooltip);
}

// The inspector's reading as text, shared by the tooltip and the panel line so
// the two can never say different things. Returns false when there is nothing
// to say.
bool ReadingLines (const HudState& state, char* first, char* second, size_t size)
{
    second[0] = 0;
    switch (state.sunReadingState) {
        case 1:
            std::snprintf (first, size, "%s%.2f h direct sun", state.sunReadingNearest ? "~ " : "",
                           state.sunReadingHours);
            if (state.sunReadingDaylight > 0.0)
                std::snprintf (second, size, "%.2f h shadow of %.2f h daylight",
                               (std::max) (0.0, state.sunReadingDaylight - state.sunReadingHours),
                               state.sunReadingDaylight);
            return true;
        case 2:
            std::snprintf (first, size, "%s",
                           state.sunReadingRole == 1   ? "context: casts shadow, not measured"
                           : state.sunReadingRole == 2 ? "ignored: not in the study"
                                                       : "not measured here");
            return true;
        case 3:
            std::snprintf (first, size, "computing...");
            return true;
        default:
            return false;
    }
}

// The section's view list and the tint mode each one selects. Index 0 follows
// whatever ShowSunStudy asked for. ⚠️ AN ABI with SunStudyDebugMode.
constexpr const char* kViewNames[] = { "as shown", "direct sun hours", "single shadow", "AM / PM shadows", "roles" };
constexpr int kViewModes[] = { -1, 0, 5, 6, 4 };
constexpr int kViewCount = 5;

void Clock (uint16_t minutes, char* out, size_t size)
{
    std::snprintf (out, size, "%02u:%02u", unsigned (minutes / 60), unsigned (minutes % 60));
}

} // namespace

SunStudyViewSettings SunStudyViewOf (const HudState& state)
{
    SunStudyViewSettings view;
    view.lo = state.sunFilterLo;
    view.hi = state.sunFilterHi;
    view.hide = state.sunFilterHide;
    view.viewOverride = state.sunView > 0 && state.sunView < kViewCount ? kViewModes[state.sunView] : -1;
    view.step = state.sunStep > 0 ? uint32_t (state.sunStep) : 0u;
    return view;
}

void ServiceSunStudyInspector (HudState& state, const DiligentScene& scene, const float origin[3],
                               const float direction[3])
{
    // ⚠️ THROTTLED TO THE CURSOR. A still cursor over a converged study asks the
    // same question every frame; the ray is compared, and re-asked at most four
    // times a second otherwise, so a study still converging updates under a
    // cursor that is not moving.
    static float lastRay[6] = { 0, 0, 0, 0, 0, 0 };
    static std::string lastStudy;
    static std::chrono::steady_clock::time_point lastAsked;
    if (state.sunInspect == 0) {
        state.sunReadingState = 0;
        return;
    }
    const std::string id = scene.ShownSunStudyId ();
    const auto now = std::chrono::steady_clock::now ();
    const float ray[6] = { origin[0], origin[1], origin[2], direction[0], direction[1], direction[2] };
    if (id == lastStudy && std::equal (ray, ray + 6, lastRay) && now - lastAsked < std::chrono::milliseconds (250))
        return;
    std::copy (ray, ray + 6, lastRay);
    lastStudy = id;
    lastAsked = now;

    state.sunReadingState = 0;
    if (id.empty ())
        return;
    const std::shared_ptr<const Snapshot> snapshot = MeshStore::Get ().Current ();
    if (snapshot == nullptr)
        return;
    // ⚠️ PEEK, NEVER For: this is the render thread, and For builds a BVH --
    // under a lock another thread may be holding for exactly that -- on a miss.
    const std::shared_ptr<const QueryEngine> engine = QueryIndexCache::Get ().Peek (snapshot->id);
    if (engine == nullptr)
        return;

    const double org[3] = { origin[0], origin[1], origin[2] };
    const double dir[3] = { direction[0], direction[1], direction[2] };
    const QueryEngine::RayHit hit = engine->Raycast (org, dir, 1.0e6);
    if (!hit.hit)
        return;

    evp::sunstudy::SunStudyReading reading;
    uint8_t role = 0xff;
    double daylight = 0.0;
    std::string error;
    if (!evp::sunstudy::SunStudyStore::Get ().ReadAt (id, snapshot->id, hit.tri, hit.meshIndex, hit.point, reading,
                                                      role, daylight, error)) {
        state.sunReadingState = error == "computing" ? 3 : 0;
        return;
    }
    state.sunReadingRole = role == 0xff ? -1 : int (role);
    state.sunReadingDaylight = daylight;
    state.sunReadingHours = reading.hours;
    state.sunReadingNearest = reading.nearest;
    state.sunReadingState = reading.measured ? 1 : 2;
}

void DrawSunStudyInspectorTooltip (const HudState& state, bool cursorInside)
{
    char first[96];
    char second[96];
    if (state.sunInspect != 1 || !cursorInside || !ReadingLines (state, first, second, sizeof (first)))
        return;
    // A plain tooltip: it follows the cursor and never takes the mouse, so the
    // camera and the pick keep working under it.
    ImGui::BeginTooltip ();
    ImGui::TextUnformatted (first);
    if (second[0] != 0)
        ImGui::TextDisabled ("%s", second);
    ImGui::EndTooltip ();
}

void DrawSunStudyHudSection (HudState& state, const DiligentSceneStats& scene)
{
    const SunStudyOverlayStatus& study = scene.sunStudy;
    // ⚠️ ONLY WHILE A STUDY IS ON SCREEN. A range slider with nothing to filter
    // reads as a control that does nothing.
    if (!study.drawing)
        return;
    if (!ImGui::CollapsingHeader ("sun study", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::TextDisabled ("%s, %u element(s)", study.studyId.c_str (), unsigned (study.elementsAttached));

    // ---- the view ---------------------------------------------------------------
    //
    // ⚠️ THE COMMAND WINS ONLY WHEN IT CHANGES (DiligentViewport.cpp's rule). A
    // new COMMANDED mode resets the choice to "as shown"; a follower rerun, which
    // re-shows with the same mode, leaves the person's choice alone.
    if (study.debugMode != state.sunSeenCommandedMode) {
        state.sunSeenCommandedMode = study.debugMode;
        state.sunView = 0;
    }
    state.sunStepCount = study.stepCount;
    ImGui::SetNextItemWidth (-60.0f);
    ImGui::Combo ("view##sunview", &state.sunView, kViewNames, kViewCount);
    const int mode = state.sunView > 0 ? kViewModes[state.sunView] : int (study.debugMode);
    const bool shadowView = mode == 5 || mode == 6;
    if (shadowView && study.stepCount == 0)
        ImGui::TextColored (ImVec4 (1.0f, 0.6f, 0.4f, 1.0f), "this study carries no per-step bits");

    // ---- the time of day, for the single shadow -----------------------------
    if (mode == 5 && study.stepCount > 0) {
        const int last = int (study.stepCount) - 1;
        if (state.sunStep < 0 || state.sunStep > last)
            state.sunStep = (std::min) (int (study.noonStep), last);
        // Play the day: one step every 0.4 s, wrapping -- the web page's button.
        if (state.sunPlaying && ImGui::GetTime () - state.sunPlayedAt > 0.4) {
            state.sunStep = state.sunStep >= last ? 0 : state.sunStep + 1;
            state.sunPlayedAt = ImGui::GetTime ();
        }
        char clock[16] = "--:--";
        if (size_t (state.sunStep) < study.stepMinutes.size ())
            Clock (study.stepMinutes[size_t (state.sunStep)], clock, sizeof (clock));
        ImGui::SetNextItemWidth (-60.0f);
        ImGui::SliderInt ("time##sunstep", &state.sunStep, 0, last, clock);
        if (ImGui::SmallButton (state.sunPlaying ? "pause" : "play the day")) {
            state.sunPlaying = !state.sunPlaying;
            state.sunPlayedAt = ImGui::GetTime ();
        }
        ImGui::SameLine ();
        if (ImGui::SmallButton ("noon"))
            state.sunStep = (std::min) (int (study.noonStep), last);
        Swatch ("##lit", Rgb (0xd2d2d2), "sunlit at this time");
        ImGui::SameLine ();
        ImGui::TextUnformatted ("sunlit");
        ImGui::SameLine ();
        Swatch ("##shadowed", Rgb (0x707071), "shadowed at this time");
        ImGui::SameLine ();
        ImGui::TextUnformatted ("shadowed");
    }
    else {
        state.sunPlaying = false;
    }
    if (mode == 6 && study.stepCount > 0) {
        char noon[16] = "--:--";
        if (study.noonStep < study.stepMinutes.size ())
            Clock (study.stepMinutes[study.noonStep], noon, sizeof (noon));
        ImGui::TextDisabled ("split at solar noon, %s", noon);
        const unsigned colours[4] = { 0xd4d6d8, 0x796ab1, 0xdf9792, 0xbba0b2 };
        const char* labels[4] = { "never shadowed", "AM only", "PM only", "AM + PM" };
        for (int i = 0; i < 4; ++i) {
            ImGui::PushID (100 + i);
            Swatch ("##ampm", Rgb (colours[i]), labels[i]);
            ImGui::PopID ();
            ImGui::SameLine ();
            ImGui::TextUnformatted (labels[i]);
            if (i % 2 == 0)
                ImGui::SameLine ();
        }
    }

    // ---- the hover inspector ----------------------------------------------------
    static const char* const kInspectModes[] = { "off", "tooltip at cursor", "in this panel" };
    ImGui::SetNextItemWidth (-60.0f);
    ImGui::Combo ("inspect##suninspect", &state.sunInspect, kInspectModes, 3);
    if (state.sunInspect == 2) {
        char first[96];
        char second[96];
        if (ReadingLines (state, first, second, sizeof (first))) {
            ImGui::TextUnformatted (first);
            if (second[0] != 0)
                ImGui::TextDisabled ("%s", second);
        }
        else {
            ImGui::TextDisabled ("hover the model");
        }
    }

    // ---- the hours range ------------------------------------------------------
    //
    // The compliance question -- "which surfaces get more than 2.5 h" -- is a
    // RANGE on the value, not a bin of the colour scale, so the two ends move
    // independently and in quarter hours. The colours stay binned; the
    // selection does not.
    const float top = kSunHoursFilterOpenTop;
    ImGui::SetNextItemWidth (-60.0f);
    ImGui::SliderFloat ("from##sunlo", &state.sunFilterLo, 0.0f, top, "%.2f h");
    ImGui::SetNextItemWidth (-60.0f);
    ImGui::SliderFloat ("to##sunhi", &state.sunFilterHi, 0.0f, top, state.sunFilterHi >= top ? "9+ h" : "%.2f h");
    state.sunFilterLo = Snap (std::clamp (state.sunFilterLo, 0.0f, top));
    state.sunFilterHi = Snap (std::clamp (state.sunFilterHi, 0.0f, top));
    // Crossing ends is a person dragging one past the other; the one that
    // moved pushes, the way the web page's pair behaves.
    if (state.sunFilterLo > state.sunFilterHi)
        state.sunFilterHi = state.sunFilterLo;
    ImGui::Checkbox ("hide the rest", &state.sunFilterHide);
    if (state.sunFilterLo > 0.0f || state.sunFilterHi < top) {
        ImGui::SameLine ();
        if (ImGui::SmallButton ("all")) {
            state.sunFilterLo = 0.0f;
            state.sunFilterHi = top;
        }
    }

    // ---- the legend -----------------------------------------------------------
    ImGui::TextDisabled ("direct sun hours");
    for (int bin = 0; bin < 10; ++bin) {
        if (bin > 0)
            ImGui::SameLine (0.0f, 2.0f);
        ImGui::PushID (bin);
        Swatch ("##sunbin", Rgb (kSunBinColours[bin]), kSunBinLabels[bin]);
        ImGui::PopID ();
    }
    ImGui::TextDisabled ("0 h %*s 9+ h", 22, "");

    // The role view's three colours -- the tint shader's RoleColor.
    ImGui::TextDisabled ("roles view");
    Swatch ("##roleA", Rgb (0xf59e24), "analysis: measured");
    ImGui::SameLine ();
    ImGui::TextUnformatted ("analysis");
    ImGui::SameLine ();
    Swatch ("##roleC", Rgb (0x8599b3), "context: casts shadow, not measured");
    ImGui::SameLine ();
    ImGui::TextUnformatted ("context");
    ImGui::SameLine ();
    Swatch ("##roleI", Rgb (0xd65cb8), "ignored: absent from the study");
    ImGui::SameLine ();
    ImGui::TextUnformatted ("ignored");
}

} // namespace archviz
} // namespace geomsrv
