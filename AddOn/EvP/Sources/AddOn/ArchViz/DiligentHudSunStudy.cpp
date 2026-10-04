// The viewport panel's "sun study" section -- the web study's hours-range filter
// and legend, beside the model they describe.
//
// âš ï¸ ITS OWN TRANSLATION UNIT because DiligentHud.cpp sits at the size cap, and
// because this is one concern: what the tint on screen means, and which part of
// it is shown. AnnotationHudControls is the precedent.
//
// RENDER THREAD, like the rest of the HUD. It reads the scene's overlay status
// (what the renderer is drawing) and writes HudState; the frame loop hands the
// range to DiligentScene::SetSunStudyFilter.

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentHudSunControls.hpp"
#include "ArchViz/SunStudyPreview.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/HudShell.hpp" // the tips
#include "ArchViz/InputRingBuffer.hpp"
#include "ArchViz/SunStudyOverlay.hpp"
#include "Geometry/MeshStore.hpp"
#include "Geometry/QueryEngine.hpp"
#include "SunStudy/SunStudyLimits.hpp"
#include "SunStudy/SunStudyRoles.hpp"
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

ImVec4 Rgb (unsigned hex)
{
    return ImVec4 (float ((hex >> 16) & 0xff) / 255.0f, float ((hex >> 8) & 0xff) / 255.0f, float (hex & 0xff) / 255.0f,
                   1.0f);
}

void Swatch (const char* id, const ImVec4& colour, const char* tooltip)
{
    ImGui::ColorButton (id, colour, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                        ImVec2 (16.0f, 16.0f));
    // Above it: the swatches stand in rows, and a tip beside one would cover its neighbours.
    hudshell::Tip (tooltip, hudshell::TipSide::Above);
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
        case 4:
            std::snprintf (first, size, "cursor ray missed the hovered element");
            return true;
        default:
            return false;
    }
}

// The section's view list and the tint mode each one selects. Index 0 follows
// whatever ShowSunStudy asked for. âš ï¸ AN ABI with SunStudyDebugMode.
constexpr const char* kViewNames[] = { "as shown", "direct sun hours", "single shadow", "multiple shadows", "roles" };
constexpr int kViewModes[] = { -1, 0, 5, 7, 4 };
constexpr int kViewCount = 5;
constexpr int kMultipleView = 3;

// The multiple-shadows intervals, the web page's list plus EVERY STEP -- the
// finest the study measured. AM / PM is the last and switches to its own mode.
constexpr const char* kFanNames[] = { "every step", "30 min", "1 hour", "2 hours", "3 hours", "AM / PM" };
constexpr double kFanMinutes[] = { 0.0, 30.0, 60.0, 120.0, 180.0, -1.0 };
constexpr int kFanCount = 6;
constexpr int kFanAmPm = 5;

// The tint mode the HUD's choices select, or -1 to follow the command.
// The mode ShowSunStudy asked for, 0 before any study.
int CommandedMode (const HudState& state)
{
    return state.sunSeenCommandedMode == 0xffffffffu ? 0 : int (state.sunSeenCommandedMode);
}

// Whether the view on screen is the multiple-shadows fan -- chosen here, OR
// commanded and followed ("as shown"). ⚠️ BOTH: live, a fan commanded by the
// smoke drew every surface "never shadowed", because only a HUD-chosen fan
// ever built the chosen-step mask.
bool FanOnScreen (const HudState& state)
{
    return state.sunView == kMultipleView || (state.sunView == 0 && CommandedMode (state) == 7);
}

int ChosenMode (const HudState& state)
{
    if (FanOnScreen (state) && state.sunFanInterval == kFanAmPm)
        return 6;
    if (state.sunView <= 0 || state.sunView >= kViewCount)
        return -1;
    return kViewModes[state.sunView];
}

void Clock (uint16_t minutes, char* out, size_t size)
{
    std::snprintf (out, size, "%02u:%02u", unsigned (minutes / 60), unsigned (minutes % 60));
}

// Millions, for counts a person reads at a glance.
double Mega (double value)
{
    return value / 1.0e6;
}

// ---- this machine's limits ----------------------------------------------------
//
// What the start command enforces (SunStudy/SunStudyLimits.hpp), shown before
// anyone asks for a grid it will refuse: the ceiling, the resource that sets it,
// the finest grid the shown model allows, and how much of the ceiling the
// current study uses. Free memory is re-read once a second.
void DrawMachineLimits (const SunStudyOverlayStatus& study)
{
    static double lastRead = -10.0;
    static uint64_t freeRam = 0;
    if (ImGui::GetTime () - lastRead > 1.0) {
        freeRam = evp::sunstudy::AvailableSystemMemory ();
        lastRead = ImGui::GetTime ();
    }
    const evp::sunstudy::MachineResources machine = evp::sunstudy::CurrentMachineResources (freeRam);
    // Without a study, a 15-minute day of ~49 daylight steps in the patch
    // domain -- the common case -- so the readout is useful before the first run.
    const size_t steps = study.stepCount > 0 ? study.stepCount : 49;
    const bool patch = study.stepCount > 0 ? study.patchDomain : true;
    const evp::sunstudy::AnalysisLimits limits = evp::sunstudy::ComputeAnalysisLimits (
        machine, steps, patch ? evp::sunstudy::kPatchAtlasOccupancy : evp::sunstudy::kTriangleAtlasOccupancy);

    if (machine.gpuKnown)
        ImGui::TextDisabled ("GPU %s, %.1f GB, textures up to %u", machine.adapter.c_str (),
                             double (machine.gpuMemoryBytes) / 1.0e9, machine.maxTextureDimension);
    else
        ImGui::TextDisabled ("GPU not known yet");
    ImGui::TextDisabled ("free memory %.1f GB", double (freeRam) / 1.0e9);
    ImGui::Text ("max %.2f M samples, set by %s", Mega (double (limits.maxSamples)),
                 evp::sunstudy::LimitBindingName (limits.binding));
    // Indented, not led by spaces: a wrapped line keeps the indent.
    ImGui::Indent ();
    ImGui::TextDisabled ("sampler %.1f M | memory %.1f M | GPU %.1f M | texture %.1f M",
                         Mega (double (limits.samplerCap)), Mega (double (limits.ramCap)),
                         Mega (double (limits.gpuCap)), Mega (double (limits.textureCap)));
    ImGui::Unindent ();
    ImGui::Text ("max %.0f M rays per study (%u steps, %s domain)", Mega (double (limits.maxRays)), unsigned (steps),
                 patch ? "patch" : "triangle");
    if (study.analysedArea > 0.0) {
        ImGui::Text ("finest grid on this model: %.2f m (%.0f m2 analysed)",
                     evp::sunstudy::FinestGridSpacing (study.analysedArea, limits.maxSamples), study.analysedArea);
    }
    if (study.sampleCount > 0 && limits.maxSamples > 0) {
        const double used = double (study.sampleCount) / double (limits.maxSamples);
        const ImVec4 colour = used > 0.8 ? ImVec4 (1.0f, 0.55f, 0.35f, 1.0f) : ImVec4 (0.6f, 0.85f, 0.6f, 1.0f);
        ImGui::TextColored (colour, "this study: %.2f M samples, %.0f%% of the limit",
                            Mega (double (study.sampleCount)), used * 100.0);
    }
}

} // namespace

SunStudyViewSettings SunStudyViewOf (const HudState& state)
{
    SunStudyViewSettings view;
    view.lo = state.sunFilterLo;
    view.hi = state.sunFilterHi;
    view.blueThreshold = state.sunLowBlue ? state.sunBlueThreshold : -1.0f;
    const auto interval = state.sunStepMinutes.empty ()
                              ? std::make_pair (0u, 0xffffffffu)
                              : SunStudyPreviewSteps (state.sunStepMinutes, state.sunTimeFrom, state.sunTimeTo);
    view.firstShadowStep = interval.first;
    view.endShadowStep = interval.second;
    view.viewOverride = ChosenMode (state);
    view.step = state.sunStep > 0 ? uint32_t (state.sunStep) : 0u;
    const int effective = view.viewOverride >= 0 ? view.viewOverride : CommandedMode (state);
    if (effective == 7 && state.sunFanInterval >= 0 && state.sunFanInterval < kFanCount) {
        // The same picking as the web study (SunStudyStepAtlas::FanSteps),
        // capped at the 96 steps the constant buffer carries.
        for (const uint32_t step : evp::sunstudy::FanSteps (state.sunStepMinutes, kFanMinutes[state.sunFanInterval])) {
            if (step >= evp::sunstudy::kFanMaxSteps)
                break;
            if (step < interval.first || step >= interval.second)
                continue;
            view.fanMask[step >> 5] |= 1u << (step & 31u);
            ++view.fanCount;
        }
    }
    return view;
}

void ServiceSunStudyInspector (HudState& state, const DiligentScene& scene, const float origin[3],
                               const float direction[3])
{
    // âš ï¸ THROTTLED TO THE CURSOR. A still cursor over a converged study asks the
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
    // âš ï¸ PEEK, NEVER For: this is the render thread, and For builds a BVH --
    // under a lock another thread may be holding for exactly that -- on a miss.
    const std::shared_ptr<const QueryEngine> engine = QueryIndexCache::Get ().Peek (snapshot->id);
    if (engine == nullptr)
        return;

    // âš ï¸ THE GPU PICK DECIDES WHICH ELEMENT IS UNDER THE CURSOR, NOT THE RAY.
    // The snapshot's BVH holds elements the viewer never draws (zone volumes,
    // hidden layers), and the first thing a ray meets can be one of them -- live,
    // the inspector "did not track the model". The pick renders what is ON
    // SCREEN, so the reading is taken from the first hit on the element it
    // names, walking past anything invisible in front.
    if (!state.hover.valid)
        return;
    const std::string picked = evp::sunstudy::CanonicalGuid (state.hover.guid);
    const double org[3] = { origin[0], origin[1], origin[2] };
    const double dir[3] = { direction[0], direction[1], direction[2] };
    const QueryEngine::PierceResult hits = engine->RaycastAll (org, dir, 1.0e6, 64);
    const QueryEngine::PierceHit* hit = nullptr;
    for (const QueryEngine::PierceHit& candidate : hits.hits) {
        if (candidate.meshIndex < snapshot->meshes.size () &&
            evp::sunstudy::CanonicalGuid (snapshot->meshes[candidate.meshIndex].guid) == picked) {
            hit = &candidate;
            break;
        }
    }
    if (hit == nullptr) {
        // Said, not hidden: the ray and the picture disagree about this pixel.
        state.sunReadingState = 4;
        return;
    }

    evp::sunstudy::SunStudyReading reading;
    uint8_t role = 0xff;
    double daylight = 0.0;
    std::string error;
    if (!evp::sunstudy::SunStudyStore::Get ().ReadAt (id, snapshot->id, hit->tri, hit->meshIndex, hit->point, reading,
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

void DrawSunStudyInspectorTooltip (const HudState& state, const InputSnapshot& input, uint32_t width, uint32_t height)
{
    (void) width;
    (void) height;
    if (state.sunInspect != 1 || !input.inside || state.sunReadingState != 1)
        return;
    const std::string text = SunStudyClock (state.sunReadingHours, true);
    ImGui::PushStyleColor (ImGuiCol_Text, ImVec4 (0.0f, 0.0f, 0.0f, 1.0f));
    ImGui::PushStyleColor (ImGuiCol_PopupBg, ImVec4 (0.78f, 0.78f, 0.78f, 1.0f));
    ImGui::PushStyleVar (ImGuiStyleVar_PopupRounding, 4.0f);
    ImGui::PushStyleVar (ImGuiStyleVar_PopupBorderSize, 0.0f);
    ImGui::BeginTooltip ();
    ImGui::TextUnformatted (text.c_str ());
    ImGui::EndTooltip ();
    ImGui::PopStyleVar (2);
    ImGui::PopStyleColor (2);
}

void DrawSunStudyHudSection (HudState& state, const DiligentSceneStats& scene)
{
    const SunStudyOverlayStatus& study = scene.sunStudy;
    // âš ï¸ ONLY WHILE A STUDY IS ON SCREEN. A range slider with nothing to filter
    // reads as a control that does nothing -- but the machine's LIMITS are worth
    // reading before the first study, so they alone stay reachable.
    if (!study.drawing) {
        ImGui::TextWrapped ("Run Native sun study in the Tapioca panel, then set the preview here.");
        if (ImGui::CollapsingHeader ("Machine limits"))
            DrawMachineLimits (study);
        return;
    }
    ImGui::TextDisabled ("%s, %u element(s)", study.studyId.c_str (), unsigned (study.elementsAttached));
    if (study.preview)
        ImGui::TextUnformatted ("Coarse preview: complete day, not final grid");

    // ---- the view ---------------------------------------------------------------
    //
    // âš ï¸ THE COMMAND WINS ONLY WHEN IT CHANGES (DiligentViewport.cpp's rule). A
    // new COMMANDED mode resets the choice to "as shown"; a follower rerun, which
    // re-shows with the same mode, leaves the person's choice alone.
    if (study.debugMode != state.sunSeenCommandedMode) {
        state.sunSeenCommandedMode = study.debugMode;
        state.sunView = 0;
    }
    state.sunStepCount = study.stepCount;
    state.sunStepMinutes = study.stepMinutes;
    const int current = ChosenMode (state) >= 0 ? ChosenMode (state) : int (study.debugMode);
    const bool shadows = current == 5 || current == 6 || current == 7;
    const float buttonWidth = (ImGui::GetContentRegionAvail ().x - ImGui::GetStyle ().ItemSpacing.x) * 0.5f;
    ImGui::PushStyleColor (ImGuiCol_Button,
                           ImGui::GetStyleColorVec4 (shadows ? ImGuiCol_FrameBg : ImGuiCol_ButtonActive));
    if (ImGui::Button ("Sun hours", ImVec2 (buttonWidth, 0)))
        state.sunView = 1;
    ImGui::PopStyleColor ();
    ImGui::SameLine ();
    ImGui::PushStyleColor (ImGuiCol_Button,
                           ImGui::GetStyleColorVec4 (shadows ? ImGuiCol_ButtonActive : ImGuiCol_FrameBg));
    if (ImGui::Button ("Shadows", ImVec2 (buttonWidth, 0))) {
        state.sunView = kMultipleView;
        state.sunFanInterval = kFanAmPm;
    }
    ImGui::PopStyleColor ();
    if (shadows) {
        const char* styles[] = { "Single", "Morning / evening", "Time fan (diagnostic)" };
        int style = current == 5 ? 0 : current == 6 ? 1 : 2;
        ImGui::SetNextItemWidth (-60.0f);
        if (ImGui::Combo ("view##shadowstyle", &style, styles, 3)) {
            state.sunView = style == 0 ? 2 : kMultipleView;
            state.sunFanInterval = style == 1 ? kFanAmPm : 2;
        }
        if (style == 2) {
            ImGui::SetNextItemWidth (-60.0f);
            ImGui::Combo ("every##sunfan", &state.sunFanInterval, kFanNames, kFanCount);
        }
    }
    const int chosen = ChosenMode (state);
    const int mode = chosen >= 0 ? chosen : int (study.debugMode);
    const bool shadowView = mode == 5 || mode == 6 || mode == 7;
    if (shadowView && study.stepCount > 0 && !study.stepMinutes.empty ()) {
        ImGui::TextUnformatted ("Hours");
        DrawSunStudyRange ("##shadow-hours", state.sunTimeFrom, state.sunTimeTo,
                           float (study.stepMinutes.front ()) / 60.0f,
                           (std::min) (24.0f, float (study.stepMinutes.back ()) / 60.0f + study.quantumHours), false);
        if (SunStudyViewOf (state).firstShadowStep == SunStudyViewOf (state).endShadowStep)
            ImGui::TextDisabled ("No measured step in this interval");
    }
    if (shadowView && study.stepCount == 0)
        ImGui::TextColored (ImVec4 (1.0f, 0.6f, 0.4f, 1.0f), "this study carries no per-step bits");

    // ---- the time of day, for the single shadow -----------------------------
    const SunStudyViewSettings preview = SunStudyViewOf (state);
    if (mode == 5 && study.stepCount > 0 && preview.firstShadowStep < preview.endShadowStep) {
        const int first = int (preview.firstShadowStep);
        const int last = int ((std::min) (preview.endShadowStep, study.stepCount)) - 1;
        if (state.sunStep < first || state.sunStep > last)
            state.sunStep = std::clamp (int (study.noonStep), first, last);
        // Play the day: one step every 0.4 s, wrapping -- the web page's button.
        if (state.sunPlaying && ImGui::GetTime () - state.sunPlayedAt > 0.4) {
            state.sunStep = state.sunStep >= last ? first : state.sunStep + 1;
            state.sunPlayedAt = ImGui::GetTime ();
        }
        char clock[16] = "--:--";
        if (size_t (state.sunStep) < study.stepMinutes.size ())
            Clock (study.stepMinutes[size_t (state.sunStep)], clock, sizeof (clock));
        ImGui::SetNextItemWidth (-60.0f);
        ImGui::SliderInt ("time##sunstep", &state.sunStep, first, last, clock);
        if (ImGui::SmallButton (state.sunPlaying ? "pause" : "play the day")) {
            state.sunPlaying = !state.sunPlaying;
            state.sunPlayedAt = ImGui::GetTime ();
        }
        ImGui::SameLine ();
        if (ImGui::SmallButton ("noon"))
            state.sunStep = std::clamp (int (study.noonStep), first, last);
        Swatch ("##lit", Rgb (0xede8db), "sunlit at this time");
        ImGui::SameLine ();
        ImGui::TextUnformatted ("sunlit");
        ImGui::SameLine ();
        Swatch ("##shadowed", Rgb (0x4770cc), "shadowed at this time");
        ImGui::SameLine ();
        ImGui::TextUnformatted ("shadowed");
    }
    else {
        state.sunPlaying = false;
    }
    if (mode == 7 && study.stepCount > 0) {
        if (study.stepCount > evp::sunstudy::kFanMaxSteps)
            ImGui::TextDisabled ("Time fan shows only the first %u measured steps", evp::sunstudy::kFanMaxSteps);
        // The fan's legend: the first and last chosen times at the ramp's ends,
        // and the never-shadowed swatch. Every-step fans have dozens of steps,
        // so the ramp is drawn as swatches without a label each.
        const SunStudyViewSettings view = SunStudyViewOf (state);
        std::vector<uint32_t> picks;
        for (uint32_t step = 0; step < evp::sunstudy::kFanMaxSteps; ++step)
            if ((view.fanMask[step >> 5] >> (step & 31u)) & 1u)
                picks.push_back (step);
        if (!picks.empty ()) {
            const size_t shown = (std::min) (picks.size (), size_t (24));
            for (size_t i = 0; i < shown; ++i) {
                const float t = shown > 1 ? float (i) / float (shown - 1) : 0.0f;
                const ImVec4 am (0.475f, 0.416f, 0.694f, 1.0f);
                const ImVec4 mid (0.733f, 0.627f, 0.698f, 1.0f);
                const ImVec4 pm (0.875f, 0.592f, 0.573f, 1.0f);
                const ImVec4& a = t <= 0.5f ? am : mid;
                const ImVec4& b = t <= 0.5f ? mid : pm;
                const float u = t <= 0.5f ? t / 0.5f : (t - 0.5f) / 0.5f;
                const ImVec4 c (a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u, 1.0f);
                if (i > 0)
                    ImGui::SameLine (0.0f, 1.0f);
                ImGui::PushID (int (200 + i));
                ImGui::ColorButton ("##fan", c, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                                    ImVec2 (9.0f, 14.0f));
                ImGui::PopID ();
            }
            char first[16] = "--:--";
            char last[16] = "--:--";
            if (picks.front () < study.stepMinutes.size ())
                Clock (study.stepMinutes[picks.front ()], first, sizeof (first));
            if (picks.back () < study.stepMinutes.size ())
                Clock (study.stepMinutes[picks.back ()], last, sizeof (last));
            ImGui::TextDisabled ("%s ... %s, %u shadow(s): colour of the LAST one", first, last,
                                 unsigned (picks.size ()));
        }
        Swatch ("##never", Rgb (0xd4d6d8), "never shadowed at the chosen times");
        ImGui::SameLine ();
        ImGui::TextUnformatted ("never shadowed");
    }
    if (mode == 6 && study.stepCount > 0) {
        char noon[16] = "--:--";
        if (study.noonStep < study.stepMinutes.size ())
            Clock (study.stepMinutes[study.noonStep], noon, sizeof (noon));
        ImGui::TextDisabled ("Solar noon %s; colour intensity = shadow duration", noon);
        const unsigned colours[4] = { 0xe8e8e6, 0x796ab1, 0xdf9792, 0xac80a2 };
        const char* labels[4] = { "sunlit", "morning", "evening", "overlap" };
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

    if (!shadowView) {
        ImGui::TextUnformatted ("Direct sunlight range");
        // Full daylight range, rather than a 9+ endpoint pretending to be a clock.
        DrawSunStudyRange ("##sun-hours", state.sunFilterLo, state.sunFilterHi, 0.0f, kSunHoursFilterOpenTop, true);
        if (ImGui::SmallButton ("All hours")) {
            state.sunFilterLo = 0.0f;
            state.sunFilterHi = kSunHoursFilterOpenTop;
        }
        ImGui::Checkbox ("Low-sun blue", &state.sunLowBlue);
        if (state.sunLowBlue)
            ImGui::TextDisabled ("Below %s", SunStudyClock (state.sunBlueThreshold, true).c_str ());
        DrawSunStudyGradient (state.sunLowBlue, state.sunBlueThreshold);
    }

    if (ImGui::TreeNode ("machine limits")) {
        DrawMachineLimits (study);
        ImGui::TreePop ();
    }

    if (!ImGui::TreeNode ("Diagnostic views"))
        return;
    ImGui::SetNextItemWidth (-60.0f);
    ImGui::Combo ("view##sunview", &state.sunView, kViewNames, kViewCount);
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
    ImGui::TreePop ();
}

} // namespace archviz
} // namespace geomsrv
