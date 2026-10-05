// ArchViz/OverlayHudDisplays -- Settings' "Displays": the add-on's own displays on the overlays,
// switched and styled (OverlayHud.hpp `Displays`). The engine's (OverlayHudEngine.hpp): it shows
// what the owner says they are and says what the user makes them; the owner applies it.

#include "ArchViz/OverlayHudEngine.hpp"

#include <cfloat>
#include <string>

namespace geomsrv {
namespace archviz {
namespace overlayhud {

namespace {

// The hidden part of a slice's outline, as Settings names it, in StorySliceOverlayContent's
// terms: dashed is the drafting convention and the default.
constexpr const char* kBehindNames[] = { "dashed", "hidden", "faint", "shown" };
constexpr layers::Behind kBehinds[] = { layers::Behind::Dash, layers::Behind::Hide, layers::Behind::Fade,
                                        layers::Behind::Show };

int BehindIndex (layers::Behind behind)
{
    for (int k = 0; k < 4; ++k)
        if (kBehinds[k] == behind)
            return k;
    return 0;
}

// A row of the two-column table: its label, then the control's cell, filled by it.
void Row (const char* label)
{
    ImGui::TableNextRow ();
    ImGui::TableSetColumnIndex (0);
    ImGui::AlignTextToFramePadding ();
    ImGui::TextUnformatted (label);
    ImGui::TableSetColumnIndex (1);
    ImGui::SetNextItemWidth (-FLT_MIN);
}

constexpr uint32_t kFillAlpha = 0x4Du; // a fill switched on: light and translucent, as by default

} // namespace

bool TakeDisplays (State& state, Displays& displays)
{
    if (!state.displaysPending)
        return false;
    displays = state.displays;
    state.displaysPending = false;
    return true;
}

// ⚠️ THE USER, 2026-10-03: Settings should have options for the additional information displays
// there are -- the storey slices on and off, and their style. Shown only where the overlay runs
// (`OwnPages::standalone`): its owner is the one that can apply them.
void Engine::Impl::DisplaySettings ()
{
    if (!own.standalone)
        return;
    ImGui::SeparatorText ("Displays");
    // What the user just set, until the owner has taken it; what the owner says otherwise.
    const Displays& shown = store->displaysPending ? store->displays : own.displays;
    Displays wanted = shown;
    std::string what;

    if (ImGui::Checkbox ("Storey slices##tapioca.display.slices", &wanted.slicesOn))
        what = wanted.slicesOn ? "slices on" : "slices off";
    hudshell::Tip ("Each storey's floor outlined on the model, with its area: the selected massing slabs' floors, "
                   "or the whole model cut at every storey");
    if (ImGui::Checkbox ("Massing story slices##tapioca.display.massing", &wanted.massingSlicesOn))
        what = wanted.massingSlicesOn ? "massing slices on" : "massing slices off";
    hudshell::Tip (
        "Display settings below apply to standalone and massing slices; massing colours follow floor function.");
    if ((wanted.slicesOn || wanted.massingSlicesOn) &&
        ImGui::BeginTable ("##tapioca.display.slices", 2,
                           ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn ("##label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn ("##value", ImGuiTableColumnFlags_WidthStretch);
        storysliceoverlay::Controls& look = wanted.slices;

        if (wanted.slicesOn) {
            Row ("From");
            int source = wanted.slicesFromModel ? 1 : 0;
            const char* const sources[] = { "selected slabs", "whole model" };
            if (ImGui::Combo ("##from", &source, sources, 2)) {
                wanted.slicesFromModel = source == 1;
                what = "slices source";
            }
        }
        Row ("Width");
        if (ImGui::SliderFloat ("##width", &look.outlineWidthPixels, 1.0f, 6.0f, "%.1f px"))
            what = "slices width";
        Row ("Hidden");
        int behind = BehindIndex (look.outlineBehind);
        if (ImGui::Combo ("##behind", &behind, kBehindNames, 4)) {
            look.outlineBehind = kBehinds[behind];
            what = "slices hidden part";
        }
        hudshell::Tip ("The outline where the building is in front of it");
        Row ("Fill");
        bool fill = (look.fillRgba & 0xFFu) != 0;
        if (ImGui::Checkbox ("##fill", &fill)) {
            look.fillRgba = (look.fillRgba & 0xFFFFFF00u) | (fill ? kFillAlpha : 0u);
            what = fill ? "slices fill on" : "slices fill off";
        }
        if (fill) {
            Row ("Opacity");
            if (ImGui::SliderFloat ("##opacity", &look.fillOpacity, 0.1f, 1.0f, "%.2f"))
                what = "slices fill opacity";
        }
        Row ("Labels");
        if (ImGui::Checkbox ("##labels", &look.label))
            what = look.label ? "slices labels on" : "slices labels off";
        hudshell::Tip ("Each slice's area, at a corner of it");
        if (look.label) {
            Row ("Name");
            if (ImGui::Checkbox ("##name", &look.labelName))
                what = "slices label name";
            hudshell::Tip ("The storey's name before the area");
            Row ("On slice");
            if (ImGui::Checkbox ("##onslice", &look.labelOnSlice))
                what = "slices label placement";
            hudshell::Tip ("Lying on the slice, sized to it; off, facing the screen");
        }
        ImGui::EndTable ();
    }
    if (wanted.slicesOn && !shown.slicesSaid.empty ())
        ImGui::TextDisabled ("%s", shown.slicesSaid.c_str ());

    if (ImGui::Checkbox ("Watch annotations##tapioca.display.annotations", &wanted.annotationsOn))
        what = wanted.annotationsOn ? "annotations on" : "annotations off";
    hudshell::Tip ("The Watch trace's annotations of the selected frame, on the model");

    if (what.empty ())
        return;
    store->displays = wanted;
    store->displaysPending = true;
    changes.push_back ({ "display", std::string (), "Settings", what, -1, 1.0, what, true });
}

} // namespace overlayhud
} // namespace archviz
} // namespace geomsrv
