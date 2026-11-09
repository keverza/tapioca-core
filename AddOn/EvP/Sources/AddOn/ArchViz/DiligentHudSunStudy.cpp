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
#include "ArchViz/GradientPresetChoice.hpp"
#include "ArchViz/SunStudyPreview.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/HudShell.hpp" // the tips
#include "ArchViz/InputRingBuffer.hpp"
#include "ArchViz/SunStudyOverlay.hpp"
#include "ArchViz/SunStudyObstructionGeometry.hpp"
#include "ArchViz/ViewerScreenshot.hpp"
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
#include <utility>
#include <vector>

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

// The section's view list and the tint mode each one selects. Index 0 follows
// whatever ShowSunStudy asked for. âš ï¸ AN ABI with SunStudyDebugMode.
constexpr int kViewModes[] = { -1, 0, 5, 7, 4, 5 };
constexpr int kViewCount = 6;
constexpr int kMultipleView = 3;
constexpr int kObstructionView = kSunObstructionView;

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

// A measured reading in a shadow view (#116): WHEN the surface was in shadow,
// from the sample's lit flags -- a person hovering a blue patch asks "is it
// shadowed now", not "how many hours of sun".
bool ShadowReadingLines (const HudState& state, int mode, char* first, char* second, size_t size)
{
    const std::vector<uint8_t>& lit = state.sunReadingLit;
    const std::vector<uint16_t>& minutes = state.sunStepMinutes;
    if (lit.empty () || minutes.empty ()) {
        std::snprintf (first, size, "no per-step shadow here");
        return true;
    }
    const char* nearest = state.sunReadingNearest ? "~ " : "";
    if (mode == 5) {
        const size_t step = size_t ((std::max) (state.sunStep, 0));
        if (step >= lit.size () || step >= minutes.size ())
            return false;
        std::snprintf (first, size, "%s%s at %s", nearest, lit[step] != 0 ? "Sunlit" : "Shadowed",
                       SunStudyTimeOfDay (minutes[step]).c_str ());
        return true;
    }
    const SunStudyViewSettings view = SunStudyViewOf (state);
    const uint32_t steps = uint32_t ((std::min) (lit.size (), minutes.size ()));
    const uint32_t from = (std::min) (view.firstShadowStep, steps);
    const uint32_t to = (std::min) (view.endShadowStep, steps);
    const uint32_t quantum = uint32_t ((std::max) (1L, std::lround (state.sunQuantumHours * 60.0f)));
    const auto runs = SunStudyShadowRuns (lit, minutes, from, to, quantum);
    const uint32_t rangeFrom = from < to ? minutes[from] : 0u;
    const uint32_t rangeTo = from < to ? minutes[to - 1] + quantum : 0u;
    std::snprintf (first, size, "%s%s", nearest, SunStudyShadowTimes (runs, rangeFrom, rangeTo).c_str ());
    if (mode == 6 && !runs.empty ()) {
        const uint32_t split = std::clamp (state.sunNoonStep, from, (std::max) (from, to));
        std::snprintf (second, size, "morning %s, evening %s",
                       SunStudyClock (SunStudyShadowedSteps (lit, from, split) * quantum / 60.0, true).c_str (),
                       SunStudyClock (SunStudyShadowedSteps (lit, split, to) * quantum / 60.0, true).c_str ());
    }
    return true;
}

// The inspector's reading as text, shared by the tooltip and the panel line so
// the two can never say different things. Returns false when there is nothing
// to say. ⚠️ IT SAYS WHAT THE VIEW ON SCREEN SHOWS: hours of direct sun in the
// hours view, shadow times in the shadow views.
bool ReadingLines (const HudState& state, char* first, char* second, size_t size)
{
    second[0] = 0;
    const int chosen = ChosenMode (state);
    const int mode = chosen >= 0 ? chosen : CommandedMode (state);
    switch (state.sunReadingState) {
        case 1:
            if (mode == 5 || mode == 6 || mode == 7)
                return ShadowReadingLines (state, mode, first, second, size);
            if (state.sunShowsPeriod) {
                // What the view on screen measures. A mean a day, never "shadow":
                // that would count back-facing time as shade.
                const char* near = state.sunReadingNearest ? "~ " : "";
                const int share = int (state.sunReadingHours / double (kSunRampTop) * 100.0 + 0.5);
                switch (state.sunPeriodQuantity) {
                    case 1:
                        std::snprintf (first, size, "%s%s a day the sun is in front", near,
                                       SunStudyClock (state.sunReadingHours, true).c_str ());
                        break;
                    case 2:
                        std::snprintf (first, size, "%s%s a day lost to obstruction", near,
                                       SunStudyClock (state.sunReadingHours, true).c_str ());
                        break;
                    case 3:
                        std::snprintf (first, size, "%s%d%% of its possible sun", near, share);
                        break;
                    case 4:
                        std::snprintf (first, size, "%s%d%% of days at or above the threshold", near, share);
                        break;
                    default:
                        std::snprintf (first, size, "%s%s a day direct sun", near,
                                       SunStudyClock (state.sunReadingHours, true).c_str ());
                        break;
                }
                std::snprintf (second, size, "%s; its year is in the panel",
                               state.sunPeriodMonth == 0 ? "mean of the period" : "mean of the month shown");
                return true;
            }
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

// An inline colour swatch beside a toggle: a click opens the picker. 0xRRGGBBAA;
// without `alpha` the colour is opaque and the alpha byte is left alone.
bool InlineColour (const char* id, uint32_t& rgba, bool alpha, const char* tip)
{
    // Cancel or Escape in its popup puts back the colour it opened on (#102).
    const bool changed = gradientchoice::ColourChoice (id, rgba, alpha);
    ImGui::SetItemTooltip ("%s", tip);
    return changed;
}

// What the overlay does to a white surface, for the legend's swatch: white
// moved toward the shadow colour by its opacity. sRGB, so approximate.
unsigned ShadowOnWhite (uint32_t rgba)
{
    const float strength = float (rgba & 0xffu) / 255.0f;
    unsigned hex = 0;
    for (int channel = 0; channel < 3; ++channel) {
        const float colour = float ((rgba >> (24 - 8 * channel)) & 0xffu);
        hex |= unsigned (std::lround (255.0f + (colour - 255.0f) * strength)) << (16 - 8 * channel);
    }
    return hex;
}

// ---- the hours view's display postprocess (PLAT-RE156) ------------------------
//
// The shadow overlay, the gradient style and its legend. DISPLAY ONLY: every
// control here changes SunStudyViewOf and nothing else, so no study reruns and
// the measured hours never change.
void DrawSunStudyHoursDisplay (HudState& state)
{
    // ⚠️ THE OVERLAY IS THE SCENE'S OWN PCF MAP, so it is only offered when that
    // map exists; a toggle that silently draws nothing reads as a broken one.
    const bool pcf = state.shadowsEnabled && state.shadowMode == int (DiligentShadowMode::Pcf);
    ImGui::BeginDisabled (!pcf);
    bool overlay = state.sunShadowOverlay && pcf;
    if (ImGui::Checkbox ("Shadow overlay", &overlay))
        state.sunShadowOverlay = overlay;
    if (overlay) {
        ImGui::SameLine ();
        InlineColour ("##sunshadowcolour", state.sunShadowRgba, true,
                      "Shadow colour, and its opacity: how far a shadowed patch moves toward it");
    }
    ImGui::EndDisabled ();
    ImGui::SetItemTooltip ("%s", pcf ? "Darkens the analysis colour where the viewer's current sun casts shadow. "
                                       "Display only."
                                 : !state.shadowsEnabled ? "Shadows are off in the Render tab."
                                                         : "Needs PCF shadows (Render tab).");
    if (!pcf) {
        ImGui::SameLine ();
        ImGui::TextDisabled (state.shadowsEnabled ? "needs PCF shadows" : "shadows are off");
    }

    // The colours (#102): any gradient preset, the sun ones listed first. Display only.
    ImGui::TextUnformatted ("Colours");
    ImGui::SameLine ();
    gradientchoice::GradientPresetChoice ("##sungradientpreset", state.sunGradientPreset, state.sunGradientCustom,
                                          state.sunGradientFlipped, ImGui::GetContentRegionAvail ().x,
                                          overlaylayers::GradientCategory::Sun);
    const auto ramp = SunStudyRampStops (state.sunGradientPreset, state.sunGradientCustom, state.sunGradientFlipped);

    // Stepped first: it is the default (#102).
    const char* styles[] = { "Stepped", "Smooth" };
    int style = state.sunGradientStepped ? 0 : 1;
    if (viewerhud::Choice ("gradient##sungradient", style, styles, 2))
        state.sunGradientStepped = style == 0;
    if (!state.sunGradientStepped) {
        DrawSunStudyGradient (state.sunLowBlue, state.sunBlueThreshold, 0, nullptr, state.sunLowSunRgba >> 8, ramp);
        return;
    }
    int count = std::clamp (state.sunBandCount, kSunBandsMin, kSunBandsMax);
    ImGui::BeginDisabled (count <= kSunBandsMin);
    if (ImGui::SmallButton ("-##sunbands"))
        --count;
    ImGui::EndDisabled ();
    ImGui::SameLine ();
    ImGui::Text ("%d steps", count);
    ImGui::SameLine ();
    ImGui::BeginDisabled (count >= kSunBandsMax);
    if (ImGui::SmallButton ("+##sunbands"))
        ++count;
    ImGui::EndDisabled ();
    ImGui::SameLine ();
    const bool reset = ImGui::SmallButton ("even##sunbands");
    ImGui::SetItemTooltip ("Spreads the steps evenly again, the last one open at 9:00h+.");
    // A new count re-spreads the edges: grips tuned for four bands mean nothing
    // for five.
    if (count != state.sunBandCount || reset) {
        state.sunBandCount = count;
        SunStudyDefaultBandEdges (count, state.sunBandEdges);
    }
    SunStudyClampBandEdges (count, state.sunBandEdges);
    DrawSunStudyGradient (state.sunLowBlue, state.sunBlueThreshold, count, state.sunBandEdges, state.sunLowSunRgba >> 8,
                          ramp);
}

// The screenshot row: the size presets, and what the last one did. The frame
// loop takes the request (ArchViz/ViewerScreenshot.cpp); this only asks.
void DrawSunStudyScreenshotControls (HudState& state)
{
    state.sunShotPreset = std::clamp (state.sunShotPreset, 0, kViewerScreenshotScaleCount - 1);
    char names[kViewerScreenshotScaleCount][48];
    const char* labels[kViewerScreenshotScaleCount];
    for (int i = 0; i < kViewerScreenshotScaleCount; ++i) {
        const float scale = kViewerScreenshotScales[i];
        std::snprintf (names[i], sizeof (names[i]), "%gx  %u x %u", double (scale),
                       unsigned (ViewerScreenshotSize (state.width, scale)),
                       unsigned (ViewerScreenshotSize (state.height, scale)));
        labels[i] = names[i];
    }
    ImGui::BeginDisabled (state.sunShotRequested || state.sunShotBusy);
    ImGui::SetNextItemWidth (ImGui::GetContentRegionAvail ().x * 0.55f);
    ImGui::Combo ("##sunshotscale", &state.sunShotPreset, labels, kViewerScreenshotScaleCount);
    ImGui::SameLine ();
    if (ImGui::Button ("Screenshot##sunshot", ImVec2 (-1.0f, 0.0f))) {
        state.sunShotRequested = true;
        state.sunShotResult.clear ();
        state.sunShotFailed = false;
    }
    ImGui::EndDisabled ();
    ImGui::SetItemTooltip ("Renders the view at the chosen size, without the panels or the axis gizmo, adds the "
                           "legend as a white strip below and saves a PNG to Tapioca\\Export.");
    if (state.sunShotRequested || state.sunShotBusy)
        ImGui::TextDisabled ("Rendering screenshot...");
    else if (state.sunShotFailed)
        hudshell::ErrorNotice ("Screenshot not saved: " + state.sunShotResult);
    else if (!state.sunShotResult.empty ()) {
        ImGui::PushStyleColor (ImGuiCol_Text, ImGui::GetStyleColorVec4 (ImGuiCol_TextDisabled));
        ImGui::TextWrapped ("Saved %s", state.sunShotResult.c_str ());
        ImGui::PopStyleColor ();
    }
}

// One legend row: a flat swatch and its words, on the draw list rather than a
// ColorButton, so the image carries no widget frame.
void LegendSwatch (unsigned hex, const char* label)
{
    const float size = ImGui::GetTextLineHeight ();
    const ImVec2 at = ImGui::GetCursorScreenPos ();
    ImGui::GetWindowDrawList ()->AddRectFilled (at, ImVec2 (at.x + size, at.y + size), ImGui::GetColorU32 (Rgb (hex)));
    ImGui::Dummy (ImVec2 (size, size));
    ImGui::SameLine ();
    ImGui::TextUnformatted (label);
}

// One editable colour of a shadow view's palette (#116).
struct ShadowRole {
    uint32_t* rgba;
    uint32_t fallback; // the default, for "reset"
    const char* label;
    const char* tip;
};

// A shadow view's legend as pickers: a swatch per role that opens the colour
// picker, its word, and one "reset" when any differs from its default.
// DISPLAY ONLY, like the hours colours: no study reruns.
//
// ⚠️ EVERY ROLE IS ITS OWN ID SCOPE, under the view's. `##` hides a suffix but
// leaves the whole label in the ID, so a bare "##sunlit" here and one anywhere
// else in the window would collide -- as the view selector's "Roles" button
// and the run form's "Roles" node did.
void DrawShadowPalette (const char* view, std::initializer_list<ShadowRole> roles)
{
    ImGui::PushID (view);
    bool edited = false;
    int i = 0;
    for (const ShadowRole& role : roles) {
        if (i++ > 0)
            ImGui::SameLine ();
        ImGui::PushID (role.label);
        InlineColour ("##colour", *role.rgba, false, role.tip);
        ImGui::PopID ();
        ImGui::SameLine ();
        ImGui::TextUnformatted (role.label);
        edited = edited || *role.rgba != role.fallback;
    }
    if (edited) {
        ImGui::SameLine ();
        if (ImGui::SmallButton ("reset"))
            for (const ShadowRole& role : roles)
                *role.rgba = role.fallback;
        ImGui::SetItemTooltip ("Puts this view's default colours back.");
    }
    ImGui::PopID ();
}

} // namespace

void DrawSunStudyCaptureLegend (const HudState& state, const SunStudyOverlayStatus& study, uint32_t width,
                                uint32_t height)
{
    if (!study.drawing || study.analysisKind != 0)
        return;
    const int chosen = ChosenMode (state);
    const int mode = chosen >= 0 ? chosen : int (study.debugMode);
    const float font = ImGui::GetFontSize ();
    // The whole strip: the caller cleared it white and it is the image's width.
    ImGui::SetNextWindowPos (ImVec2 (0.0f, 0.0f));
    ImGui::SetNextWindowSize (ImVec2 (float (width), float (height)));
    ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (font, 0.8f * font));
    ImGui::PushStyleVar (ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor (ImGuiCol_WindowBg, ImVec4 (1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::PushStyleColor (ImGuiCol_Text, ImVec4 (0.12f, 0.12f, 0.12f, 1.0f));
    ImGui::PushStyleColor (ImGuiCol_TextDisabled, ImVec4 (0.38f, 0.38f, 0.38f, 1.0f));
    ImGui::Begin ("##sun-capture-legend", nullptr,
                  ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar);
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    if (mode == 0) {
        const SunStudyViewSettings view = SunStudyViewOf (state);
        ImGui::TextUnformatted ("Direct sun hours");
        if (view.shadowOverlay) {
            ImGui::SameLine (0.0f, 2.0f * font);
            LegendSwatch (ShadowOnWhite (view.shadowRgba), "shadow at the viewer's sun");
        }
        if (view.blueThreshold > 0.0f) {
            ImGui::SameLine (0.0f, 2.0f * font);
            LegendSwatch (view.lowSunRgba >> 8,
                          ("low sun: below " + SunStudyClock (view.blueThreshold, true)).c_str ());
        }
        // The gradient across the strip, and its hour steps under it: every
        // band edge when stepped, every hour when smooth.
        const ImVec2 at = ImGui::GetCursorScreenPos ();
        const ImVec2 size (ImGui::GetContentRegionAvail ().x, font);
        // The ticks below carry the hours, so the cells stay unlabelled here.
        DrawSunStudyRampBar (draw, at, size, view.bandCount, view.bandEdges, view.blueThreshold, view.lowSunRgba >> 8,
                             false, { view.ramp, size_t (view.rampCount) });
        ImGui::Dummy (size);
        std::vector<float> ticks;
        if (view.bandCount > 0)
            ticks.assign (view.bandEdges, view.bandEdges + view.bandCount - 1);
        else
            for (int hour = 1; hour <= 9; ++hour)
                ticks.push_back (float (hour));
        ticks.insert (ticks.begin (), 0.0f);
        const ImU32 ink = ImGui::GetColorU32 (ImGuiCol_Text);
        const float labelY = at.y + size.y + 0.25f * font;
        for (size_t k = 0; k < ticks.size (); ++k) {
            const float x = at.x + size.x * ticks[k] / kSunRampTop;
            const std::string label = SunStudyValueText (ticks[k], true) + (k + 1 == ticks.size () ? "+" : "");
            const float wide = ImGui::CalcTextSize (label.c_str ()).x;
            // Centred on its tick, held inside the bar's span at the two ends.
            const float left = std::clamp (x - 0.5f * wide, at.x, at.x + size.x - wide);
            draw->AddLine (ImVec2 (x, at.y + size.y), ImVec2 (x, labelY), ink, (std::max) (1.0f, 0.08f * font));
            draw->AddText (ImVec2 (left, labelY), ink, label.c_str ());
        }
    }
    else {
        const auto row = [&] (const char* title, std::initializer_list<std::pair<unsigned, const char*>> swatches) {
            ImGui::TextUnformatted (title);
            for (const auto& swatch : swatches) {
                ImGui::SameLine (0.0f, 2.0f * font);
                LegendSwatch (swatch.first, swatch.second);
            }
        };
        char clock[16] = "--:--";
        if (state.sunStep >= 0 && size_t (state.sunStep) < study.stepMinutes.size ())
            Clock (study.stepMinutes[size_t (state.sunStep)], clock, sizeof (clock));
        const std::string at = std::string ("Shadow at ") + clock;
        // The person's colours (#116), the same ones the tint draws.
        if (mode == 5)
            row (at.c_str (),
                 { { state.sunSingleLitRgba >> 8, "sunlit" }, { state.sunSingleShadowedRgba >> 8, "shadowed" } });
        else if (mode == 6)
            row ("Shadow duration",
                 { { state.sunAmPmSunlitRgba >> 8, "sunlit" },
                   { state.sunAmPmMorningRgba >> 8, "morning" },
                   { state.sunAmPmEveningRgba >> 8, "evening" },
                   { SunStudyAmPmOverlapRgba (state.sunAmPmMorningRgba, state.sunAmPmEveningRgba) >> 8, "overlap" } });
        else if (mode == 7)
            row ("Shadow time fan, colour of the last shadow",
                 { { 0x796ab1, "earliest" }, { 0xdf9792, "latest" }, { 0xd4d6d8, "never shadowed" } });
        else if (mode == 4)
            row ("Sun study roles", { { 0xf59e24, "analysis" }, { 0x8599b3, "context" }, { 0xd65cb8, "ignored" } });
        else
            ImGui::TextUnformatted ("Sun study diagnostic view");
    }
    ImGui::End ();
    ImGui::PopStyleColor (3);
    ImGui::PopStyleVar (2);
}

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
    view.lowSunRgba = state.sunLowSunRgba;
    view.shadowRgba = state.sunShadowRgba;
    view.singleLitRgba = state.sunSingleLitRgba;
    view.singleShadowedRgba = state.sunSingleShadowedRgba;
    view.amPmSunlitRgba = state.sunAmPmSunlitRgba;
    view.amPmMorningRgba = state.sunAmPmMorningRgba;
    view.amPmEveningRgba = state.sunAmPmEveningRgba;
    view.shadowOverlay =
        state.sunShadowOverlay && state.shadowsEnabled && state.shadowMode == int (DiligentShadowMode::Pcf);
    const auto ramp = SunStudyRampStops (state.sunGradientPreset, state.sunGradientCustom, state.sunGradientFlipped);
    view.rampCount = int (ramp.size ());
    std::copy (ramp.begin (), ramp.end (), view.ramp);
    if (state.sunGradientStepped) {
        view.bandCount = std::clamp (state.sunBandCount, kSunBandsMin, kSunBandsMax);
        std::copy (state.sunBandEdges, state.sunBandEdges + 11, view.bandEdges);
        SunStudyClampBandEdges (view.bandCount, view.bandEdges);
    }
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

void DrawSunStudyInspectorTooltip (const HudState& state, const InputSnapshot& input, uint32_t width, uint32_t height)
{
    (void) width;
    (void) height;
    if (state.sunInspect != 1 || !input.inside)
        return;
    if (state.sunView == kObstructionView && state.sunObstructionMode == 3 && state.sunCutReadingMinute >= 0) {
        hudshell::TipAt (ImGui::GetIO ().MousePos, hudshell::TipSide::Right,
                         SunStudyCutSurfaceTooltip (state.sunCutReadingMinute), kSolarCutWonRgba);
        return;
    }
    if (state.sunReadingState != 1)
        return;
    const int chosen = ChosenMode (state);
    const int mode = chosen >= 0 ? chosen : CommandedMode (state);
    if (mode != 5 && mode != 6 && mode != 7) {
        // The hours view keeps its exact-hour hover (#102).
        // A period says what the number is: a mean a day, a share.
        char first[96] = {}, second[96] = {};
        if (state.sunShowsPeriod && ReadingLines (state, first, second, sizeof (first)))
            hudshell::TipAt (ImGui::GetIO ().MousePos, hudshell::TipSide::Right, first);
        else
            hudshell::TipAt (ImGui::GetIO ().MousePos, hudshell::TipSide::Right,
                             SunStudyClock (state.sunReadingHours, true));
        return;
    }
    // The shadow views say WHEN, never the hours of sun (#116).
    char first[96] = {}, second[96] = {};
    if (!ReadingLines (state, first, second, sizeof (first)))
        return;
    uint32_t swatch = 0;
    const size_t step = size_t ((std::max) (state.sunStep, 0));
    if (mode == 5 && step < state.sunReadingLit.size ())
        swatch = state.sunReadingLit[step] != 0 ? state.sunSingleLitRgba : state.sunSingleShadowedRgba;
    hudshell::TipAt (ImGui::GetIO ().MousePos, hudshell::TipSide::Right,
                     second[0] != 0 ? std::string (first) + "\n" + second : std::string (first), swatch);
}

void DrawSunStudyHudSection (HudState& state, const DiligentSceneStats& scene)
{
    // ⚠️ A VISIBILITY STUDY ON THE SHARED CHANNEL IS NOT THIS TAB'S. It is hidden
    // here (AnalysisTabs.hpp), so the tab reads as having no study and can run
    // one; its own last one, if any, is put back by ApplyAnalysisView.
    SunStudyOverlayStatus study = scene.sunStudy;
    study.drawing = study.drawing && study.analysisKind == 0;
    // The study is asked for here, not only in the palette: open until the
    // first one is on screen, then folded under what it shows.
    if (DrawSunStudyRunAction (state))
        ImGui::SetNextItemOpen (true, ImGuiCond_Always);
    if (ImGui::CollapsingHeader ("Run sun study", study.drawing ? 0 : ImGuiTreeNodeFlags_DefaultOpen))
        DrawSunStudyRunControls (state, study);
    // âš ï¸ ONLY WHILE A STUDY IS ON SCREEN. A range slider with nothing to filter
    // reads as a control that does nothing -- but the machine's LIMITS are worth
    // reading before the first study, so they alone stay reachable.
    if (!study.drawing) {
        if (ImGui::CollapsingHeader ("Machine limits"))
            DrawMachineLimits (study);
        return;
    }
    // Whether the shown study is the runner's finished period, by id.
    state.sunShowsPeriod = !state.sunPeriodStudyId.empty () && state.sunPeriodStudyId == study.studyId;
    ImGui::TextDisabled ("%s, %u element(s)", study.studyId.c_str (), unsigned (study.elementsAttached));
    if (state.sunShowsPeriod) {
        char line[160];
        std::snprintf (line, sizeof (line), "Period %s%s; shadows of %s", state.sunPeriodLabel.c_str (),
                       state.sunPeriodEstimate ? " (estimate)" : "", state.sunPeriodCarrierDate.c_str ());
        SunStudyNote (line, "The map shows the period's totals. The Shadows view scrubs one traced day -- the last "
                            "one -- not the period.");
    }
    DrawSunStudyPeriodView (state);
    if (!state.sunShowsPeriod)
        DrawSunStudyPointDay (state, study.studyId, study.version);
    if (study.preview)
        ImGui::TextUnformatted ("Coarse preview: complete day, not final grid");
    DrawSunStudyScreenshotControls (state);

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
    state.sunNoonStep = study.noonStep;
    state.sunQuantumHours = study.quantumHours;
    const int current = ChosenMode (state) >= 0 ? ChosenMode (state) : int (study.debugMode);
    const bool obstruction = state.sunView == kObstructionView;
    const bool shadows = !obstruction && (current == 5 || current == 6 || current == 7);
    const bool roles = current == 4;
    const float buttonWidth = (ImGui::GetContentRegionAvail ().x - 3.0f * ImGui::GetStyle ().ItemSpacing.x) / 4.0f;
    ImGui::PushStyleColor (
        ImGuiCol_Button,
        ImGui::GetStyleColorVec4 (shadows || obstruction || roles ? ImGuiCol_FrameBg : ImGuiCol_ButtonActive));
    if (ImGui::Button ("Sunstudy##sunview", ImVec2 (buttonWidth, 0)))
        state.sunView = 1;
    ImGui::PopStyleColor ();
    ImGui::SameLine ();
    ImGui::PushStyleColor (ImGuiCol_Button,
                           ImGui::GetStyleColorVec4 (shadows ? ImGuiCol_ButtonActive : ImGuiCol_FrameBg));
    if (ImGui::Button ("Shadows##sunview", ImVec2 (buttonWidth, 0))) {
        state.sunView = kMultipleView;
        state.sunFanInterval = kFanAmPm;
    }
    ImGui::PopStyleColor ();
    ImGui::SameLine ();
    ImGui::PushStyleColor (ImGuiCol_Button,
                           ImGui::GetStyleColorVec4 (obstruction ? ImGuiCol_ButtonActive : ImGuiCol_FrameBg));
    if (ImGui::Button ("Obstruction##sunview", ImVec2 (buttonWidth, 0))) {
        state.sunView = kObstructionView;
        if (state.sunObstructionMode == 0)
            state.sunObstructionMode = 1;
    }
    ImGui::PopStyleColor ();
    ImGui::SameLine ();
    ImGui::PushStyleColor (ImGuiCol_Button,
                           ImGui::GetStyleColorVec4 (roles ? ImGuiCol_ButtonActive : ImGuiCol_FrameBg));
    if (ImGui::Button ("Roles##sunview", ImVec2 (buttonWidth, 0)))
        state.sunView = 4;
    ImGui::PopStyleColor ();

    char first[96] = {}, second[96] = {};
    ReadingLines (state, first, second, sizeof (first));
    DrawSunStudyInspectControl (state.sunInspect, first, second);
    ImGui::Checkbox ("Sun path##sunpath", &state.sunPathShown);
    ImGui::SetItemTooltip ("Draws the sun's path over the model from Archicad's location and north: the winter "
                           "(blue) and summer (orange) solstices, the equinox (grey), the run form's date (yellow), "
                           "the hours across the year, and the sun at the shadow view's time. Centred on the study "
                           "point when one is placed.");
    ImGui::SameLine ();
    ImGui::BeginDisabled (!state.sunPathShown);
    ImGui::SetNextItemWidth (140.0f);
    ImGui::SliderFloat ("scale##sunpath", &state.sunPathScale, 0.25f, 4.0f, "%.2fx");
    ImGui::EndDisabled ();
    ImGui::SetItemTooltip ("Scales only the sun-path preview and ground compass, not the analysis or measured hours.");
    DrawSunStudySunPathStatus (state, study.studyId);
    if (ChosenMode (state) == 4 || (ChosenMode (state) < 0 && study.debugMode == 4)) {
        Swatch ("##roleA", Rgb (0xf59e24), "analysis: measured");
        ImGui::SameLine ();
        ImGui::TextUnformatted ("Analysis");
        Swatch ("##roleC", Rgb (0x8599b3), "context: casts shadow, not measured");
        ImGui::SameLine ();
        ImGui::TextUnformatted ("Context");
        Swatch ("##roleI", Rgb (0xd65cb8), "ignored: absent from the study");
        ImGui::SameLine ();
        ImGui::TextUnformatted ("Ignored");
        return;
    }
    if (obstruction) {
        if (state.sunShowsPeriod)
            SunStudyNote ("Obstruction attribution is available for a single-day study.");
        else
            DrawSunStudyObstructionControls (state, study.studyId);
        return;
    }
    if (shadows && state.sunShowsPeriod) {
        char line[120];
        std::snprintf (line, sizeof (line), "Shadows of one traced day, %s - not the period",
                       state.sunPeriodCarrierDate.c_str ());
        SunStudyNote (line, "Pick another day of the period below: a kept day plays from the archive, any other "
                            "is traced as a single day with the period's settings.");
        DrawSunStudyShadowDate (state);
    }
    if (shadows) {
        const char* styles[] = { "Single", "Morning / evening", "Time fan (diagnostic)" };
        int style = current == 5 ? 0 : current == 6 ? 1 : 2;
        if (viewerhud::Choice ("view##shadowstyle", style, styles, 3)) {
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
        const bool noon = ImGui::SmallButton ("noon");
        if (noon)
            state.sunStep = std::clamp (int (study.noonStep), first, last);
        // Play the day: the shared timeline -- one step every 0.4 s at 1x.
        HudTimeline play { state.sunPlaying, state.sunPlaySpeed, state.sunPlayedAt, state.sunPlayLoop };
        if (noon)
            play.playedAt = ImGui::GetTime (); // do not immediately advance past the requested noon
        const auto clock = [&] {
            char text[16] = "--:--";
            if (size_t (state.sunStep) < study.stepMinutes.size ())
                Clock (study.stepMinutes[size_t (state.sunStep)], text, sizeof (text));
            return std::string (text);
        };
        DrawSunStudyTimeline ("sunstep", state.sunStep, first, last, play, 0.4, clock);
        state.sunPlaying = play.playing;
        state.sunPlaySpeed = play.speed;
        state.sunPlayedAt = play.playedAt;
        state.sunPlayLoop = play.loop;
        DrawShadowPalette (
            "singleshadow",
            { { &state.sunSingleLitRgba, kSingleShadowLitRgba, "sunlit", "Sunlit at this time" },
              { &state.sunSingleShadowedRgba, kSingleShadowShadowedRgba, "shadowed", "Shadowed at this time" } });
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
        DrawShadowPalette ("ampm",
                           { { &state.sunAmPmSunlitRgba, kAmPmSunlitRgba, "sunlit", "Never shadowed" },
                             { &state.sunAmPmMorningRgba, kAmPmMorningRgba, "morning", "Shadowed before solar noon" },
                             { &state.sunAmPmEveningRgba, kAmPmEveningRgba, "evening", "Shadowed after solar noon" } });
        // Not a colour of its own: the tint mixes morning and evening by share.
        ImGui::PushID ("ampm");
        Swatch ("##overlap", Rgb (SunStudyAmPmOverlapRgba (state.sunAmPmMorningRgba, state.sunAmPmEveningRgba) >> 8),
                "Shadowed morning and evening: their mix, by each one's share");
        ImGui::PopID ();
        ImGui::SameLine ();
        ImGui::TextUnformatted ("overlap");
    }

    if (!shadowView) {
        ImGui::TextUnformatted (SunLegendUnit () == SunValueUnit::Hours ? "Direct sunlight range" : "Range shown");
        // Full daylight range, rather than a 9+ endpoint pretending to be a clock.
        DrawSunStudyRange ("##sun-hours", state.sunFilterLo, state.sunFilterHi, 0.0f, kSunHoursFilterOpenTop, true);
        if (ImGui::SmallButton ("All hours")) {
            state.sunFilterLo = 0.0f;
            state.sunFilterHi = kSunHoursFilterOpenTop;
        }
        ImGui::Checkbox ("Low-sun blue", &state.sunLowBlue);
        if (state.sunLowBlue) {
            ImGui::SameLine ();
            InlineColour ("##sunlowsuncolour", state.sunLowSunRgba, false,
                          "Low-sun colour: every surface below the cutoff");
            ImGui::SameLine ();
            ImGui::TextDisabled ("below %s", SunStudyClock (state.sunBlueThreshold, true).c_str ());
        }
        DrawSunStudyHoursDisplay (state);
    }

    if (ImGui::TreeNode ("machine limits")) {
        DrawMachineLimits (study);
        ImGui::TreePop ();
    }
}

} // namespace archviz
} // namespace geomsrv
