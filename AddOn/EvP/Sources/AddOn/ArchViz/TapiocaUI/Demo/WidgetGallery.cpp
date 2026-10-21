#include "ArchViz/TapiocaUI/Demo/WidgetGallery.hpp"

#include "ArchViz/HudShell.hpp"
#include "ArchViz/OverlayLayers.hpp"
#include "ArchViz/TapiocaUI/Foundation/Icons.hpp"
#include "ArchViz/TapiocaUI/Foundation/ScopedStyle.hpp"
#include "ArchViz/TapiocaUI/Foundation/Units.hpp"

#include <imgui.h>
#include <imgui_internal.h> // ImGuiItemFlags_MixedValue: the indeterminate checkbox

#include <algorithm>
#include <cmath>
#include <vector>

namespace geomsrv::archviz::ui {

namespace {

namespace layers = overlaylayers;
namespace shell = hudshell;

// The light card is the HUD's plain look; the dark one is a caller panel's default, the
// overlays' own dark card, at the plain look's size. Neither carries a colour of its own here.
layers::Panel LookOf (const GalleryState& state)
{
    const layers::Panel& plain = shell::PlainLook ();
    if (!state.dark)
        return plain;
    layers::Panel dark;
    dark.title = plain.title;
    dark.sizePixels = plain.sizePixels;
    dark.widthPixels = plain.widthPixels;
    return dark;
}

float Density (const GalleryState& state)
{
    return tokens::kDensitySteps[(std::min) (state.density, tokens::kDensityStepCount - 1)];
}

// The scale hudshell's helpers take: the host's, read back from the font it pushed against the
// look's own font size, times the gallery's density.
float ScaleOf (const layers::Panel& look, float density)
{
    const float host = look.sizePixels > 0.0f ? ImGui::GetStyle ().FontSizeBase / look.sizePixels : 1.0f;
    return host * density;
}

const char* Flag (bool on)
{
    return on ? "yes" : "-";
}

void Header (GalleryState& state)
{
    ImGui::TextUnformatted ("Density");
    for (uint32_t i = 0; i < tokens::kDensityStepCount; ++i) {
        ImGui::SameLine ();
        const std::string label = shell::Percent (tokens::kDensitySteps[i]);
        if (ImGui::RadioButton (label.c_str (), state.density == i))
            state.density = i;
    }
    ImGui::TextUnformatted ("Theme");
    ImGui::SameLine ();
    if (ImGui::RadioButton ("Light", !state.dark))
        state.dark = false;
    ImGui::SameLine ();
    if (ImGui::RadioButton ("Dark", state.dark))
        state.dark = true;
}

void TextSamples ()
{
    ImGui::TextUnformatted ("Body text in the HUD's font");
    ImGui::TextDisabled ("Muted text: a disabled label or a hint");
    ImGui::TextUnformatted ("Grundfläche Ø 12,5 m² · Höhe ±0,00 · 45° · Façade");
    ImGui::TextWrapped ("A long label wraps inside the panel's padding instead of running off its edge, as every page "
                        "of the floating panel does when the text size is raised.");
}

// One number as the features edit it today -- ImGui's own InputDouble -- enabled, then disabled.
void NumberRow (GalleryState& state, const char* control, double& value, const char* format)
{
    ImGui::PushID (control);
    ImGui::TextUnformatted (control);
    ImGui::InputDouble ("##enabled", &value, 0.0, 0.0, format);
    ObserveLastItem (state, control);
    ImGui::BeginDisabled ();
    double shown = value;
    ImGui::InputDouble ("disabled##disabled", &shown, 0.0, 0.0, format);
    ImGui::EndDisabled ();
    ImGui::PopID ();
}

void NumberSamples (GalleryState& state)
{
    NumberRow (state, "huge", state.huge, "%.0f");
    NumberRow (state, "tiny", state.tiny, "%.6g");
    NumberRow (state, "negative", state.negative, "%.2f");

    ImGui::TextUnformatted ("length, dragged, with a unit");
    ImGui::DragScalar ("##length", ImGuiDataType_Double, &state.length, 0.05f, nullptr, nullptr, "%.2f m");
    ObserveLastItem (state, "length");

    // Clamped at both ends; the buttons put it on each.
    ImGui::TextUnformatted ("fraction, clamped 0 to 1");
    ImGui::SliderFloat ("##fraction", &state.fraction, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ObserveLastItem (state, "fraction");
    if (ImGui::SmallButton ("min"))
        state.fraction = 0.0f;
    ImGui::SameLine ();
    if (ImGui::SmallButton ("max"))
        state.fraction = 1.0f;

    // The library's own parsing, on what is typed: what NumericField will accept in P2.
    ImGui::TextUnformatted ("typed number, ',' or '.'");
    ImGui::InputText ("##typed", state.typed, sizeof state.typed);
    ObserveLastItem (state, "typed");
    double parsed = 0.0;
    if (ParseNumber (state.typed, parsed))
        ImGui::TextDisabled ("reads %s", FormatNumber (parsed, 3).c_str ());
    else
        ImGui::TextColored (shell::Colour (shell::kErrorRgba), "refused: not one finite number");
}

void ChoiceSamples (GalleryState& state)
{
    ImGui::Checkbox ("flag", &state.flag);
    ObserveLastItem (state, "flag");
    ImGui::PushItemFlag (ImGuiItemFlags_MixedValue, true);
    ImGui::Checkbox ("mixed: elements differ", &state.mixed);
    ImGui::PopItemFlag ();
    ObserveLastItem (state, "mixed");
    ImGui::BeginDisabled ();
    bool shown = state.flag;
    ImGui::Checkbox ("flag, disabled", &shown);
    ImGui::EndDisabled ();

    const char* const options[] = { "First option", "A much longer option that is cut by the combo", "Third" };
    ImGui::Combo ("##choice", &state.choice, options, IM_ARRAYSIZE (options));
    ObserveLastItem (state, "choice");
    for (int i = 0; i < 3; ++i) {
        if (i > 0)
            ImGui::SameLine ();
        ImGui::PushID (i);
        if (ImGui::RadioButton (i == 0 ? "One" : i == 1 ? "Two" : "Three", state.radio == i)) {
            state.radio = i;
            EditResult picked;
            picked.changed = picked.committed = true;
            Record (state, "radio", picked);
        }
        ImGui::PopID ();
    }

    ImGui::TextUnformatted ("colour");
    EditResult chosen;
    chosen.changed = chosen.committed = shell::ColourChoice ("colour", state.colour);
    Record (state, "colour", chosen);
    ImGui::BeginDisabled ();
    uint32_t colour = state.colour;
    shell::ColourChoice ("colour, disabled", colour);
    ImGui::EndDisabled ();
}

void CardSamples (const layers::Panel& look, float scale)
{
    shell::Card card;
    card.title = "Figures";
    card.alignDecimals = true;
    card.figures.push_back ({ "Gross area", "12 345.67 m²" });
    card.figures.push_back ({ "Storeys", "12" });
    card.figures.push_back ({ "Tiny", "0.000001" });
    card.figures.push_back ({ "Below datum", "-3.25 m", shell::kErrorRgba });
    card.figures.push_back ({ "A label long enough to need the whole column", "1.5" });
    card.progress = 0.42;
    card.progressText = "42 % read";
    card.note = "A note in a line of its own under the figures.";
    shell::Cards ({ card }, look, scale);
}

void TipSamples ()
{
    const std::pair<const char*, shell::TipSide> sides[] = { { "Left", shell::TipSide::Left },
                                                             { "Right", shell::TipSide::Right },
                                                             { "Above", shell::TipSide::Above },
                                                             { "Below", shell::TipSide::Below } };
    for (size_t i = 0; i < IM_ARRAYSIZE (sides); ++i) {
        if (i > 0)
            ImGui::SameLine ();
        ImGui::SmallButton (sides[i].first);
        shell::Tip (std::string ("Said ") + sides[i].first + ", its arrow at the button", sides[i].second);
    }
    ImGui::BeginDisabled ();
    ImGui::SmallButton ("Disabled");
    ImGui::EndDisabled ();
    shell::Tip ("A disabled control still says its tip", shell::TipSide::Right);
}

// ⚠️ THE ICON SHEET IS THE GLYPHS' VISUAL CONTRACT (HANDOFF-TapiocaUI-Library.md section 9): every
// id at 16, 24 and 32 px, disabled and as a button that toggles, with its name and the upstream
// file it was generated from -- a missing, malformed or mismatched glyph shows here before it
// reaches a toolbar. Light and dark are the gallery's own switch.
void IconSamples (GalleryState& state, float scale)
{
    const float sizes[] = { 16.0f, 24.0f, 32.0f };
    const ImU32 ink = ImGui::GetColorU32 (ImGuiCol_Text);
    for (uint32_t i = 0; i < IconCount (); ++i) {
        const IconId icon = IconId (i);
        ImGui::PushID (int (i));
        for (const float pixels : sizes) {
            const float side = std::round (pixels * scale);
            Icon (icon, ImGui::GetCursorScreenPos (), side, ink);
            ImGui::Dummy (ImVec2 (side, side));
            ImGui::SameLine ();
        }
        ImGui::BeginDisabled ();
        IconButton ("disabled", icon);
        ImGui::EndDisabled ();
        ImGui::SameLine ();
        if (IconButton ("toggle", icon, state.activeIcon == int (i))) {
            state.activeIcon = state.activeIcon == int (i) ? -1 : int (i);
            EditResult toggled;
            toggled.changed = toggled.committed = true;
            Record (state, IconName (icon), toggled);
        }
        ImGui::SameLine ();
        ImGui::TextUnformatted (IconName (icon));
        ImGui::SameLine ();
        ImGui::TextDisabled ("%s", IconFile (icon)[0] != 0 ? IconFile (icon) : "placeholder");
        ImGui::PopID ();
    }
}

void LogTable (GalleryState& state)
{
    if (state.log.empty ()) {
        ImGui::TextDisabled ("Press or edit a control: what it said appears here.");
        return;
    }
    if (ImGui::SmallButton ("Clear"))
        state.log.clear ();
    if (!ImGui::BeginTable ("log", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
        return;
    ImGui::TableSetupColumn ("control", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn ("changed");
    ImGui::TableSetupColumn ("committed");
    ImGui::TableSetupColumn ("canceled");
    ImGui::TableSetupColumn ("frame");
    ImGui::TableHeadersRow ();
    for (const GalleryEntry& entry : state.log) {
        ImGui::TableNextRow ();
        ImGui::TableNextColumn ();
        ImGui::TextUnformatted (entry.control.c_str ());
        ImGui::TableNextColumn ();
        ImGui::TextUnformatted (Flag (entry.result.changed));
        ImGui::TableNextColumn ();
        ImGui::TextUnformatted (Flag (entry.result.committed));
        ImGui::TableNextColumn ();
        ImGui::TextUnformatted (Flag (entry.result.canceled));
        ImGui::TableNextColumn ();
        ImGui::Text ("%d", entry.frame);
    }
    ImGui::EndTable ();
}

} // namespace

void Record (GalleryState& state, const char* control, const EditResult& result)
{
    if (!result.Any ())
        return;
    const auto held = std::find_if (state.log.begin (), state.log.end (),
                                    [control] (const GalleryEntry& entry) { return entry.control == control; });
    if (held != state.log.end ())
        state.log.erase (held);
    GalleryEntry entry;
    entry.control = control;
    entry.result = result;
    entry.frame = ImGui::GetFrameCount ();
    state.log.insert (state.log.begin (), std::move (entry));
}

void ObserveLastItem (GalleryState& state, const char* control)
{
    EditResult result;
    result.changed = ImGui::IsItemEdited ();
    result.committed = ImGui::IsItemDeactivatedAfterEdit ();
    // Not exclusive of `committed`: whether today's controls say both on an Escape is exactly
    // what the log is here to show.
    result.canceled = ImGui::IsItemDeactivated () && ImGui::IsKeyPressed (ImGuiKey_Escape, false);
    Record (state, control, result);
}

void DrawGallery (GalleryState& state)
{
    const layers::Panel look = LookOf (state);
    const float density = Density (state);
    const float scale = ScaleOf (look, density);
    const ImGuiStyle& base = ImGui::GetStyle ();

    ScopedStyle style;
    style.Adopt (shell::PushLook (look, scale), shell::kLookVars);
    style.Var (ImGuiStyleVar_FramePadding, ImVec2 (base.FramePadding.x * density, base.FramePadding.y * density))
        .Var (ImGuiStyleVar_ItemSpacing, ImVec2 (base.ItemSpacing.x * density, base.ItemSpacing.y * density))
        .Var (ImGuiStyleVar_ItemInnerSpacing,
              ImVec2 (base.ItemInnerSpacing.x * density, base.ItemInnerSpacing.y * density));
    // ⚠️ THE BASE SIZE, NOT GetFontSize (): ImGui 1.92 applies its global scale factors to what
    // PushFont is given, so a scaled size passed back in would be scaled twice.
    style.Font (nullptr, base.FontSizeBase * density);

    ImGui::PushID ("tapioca.ui.gallery");
    Header (state);
    ImGui::Separator ();
    if (shell::Section ("Text", true))
        TextSamples ();
    if (shell::Section ("Numbers", true))
        NumberSamples (state);
    if (shell::Section ("Choices", true))
        ChoiceSamples (state);
    if (shell::Section ("Cards"))
        CardSamples (look, scale);
    if (shell::Section ("Tips"))
        TipSamples ();
    if (shell::Section ("Icons", true))
        IconSamples (state, scale);
    if (shell::Section ("Interaction log", true))
        LogTable (state);
    ImGui::PopID ();
}

void GalleryWindow (GalleryState& state, bool& open)
{
    const layers::Panel look = LookOf (state);
    // The window's own ground, padding and rounding are its look's, pushed before Begin.
    ScopedStyle frame;
    frame.Adopt (shell::PushLook (look, ScaleOf (look, 1.0f)), shell::kLookVars);
    const float em = ImGui::GetFontSize ();
    ImGui::SetNextWindowSize (ImVec2 (28.0f * em, 40.0f * em), ImGuiCond_FirstUseEver);
    if (ImGui::Begin ("Widget gallery###tapioca.ui.gallery", &open))
        DrawGallery (state);
    ImGui::End ();
}

} // namespace geomsrv::archviz::ui
