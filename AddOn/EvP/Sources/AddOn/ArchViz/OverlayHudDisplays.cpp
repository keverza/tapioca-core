// Display visibility belongs to its feature page; shared story-slice styling lives
// only inside Graphics style lab. The owner applies queued changes after layout.

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

// Standalone model cuts remain distinct from the automatic massing story slices.
void Engine::Impl::DisplaySettings ()
{
    if (!own.standalone)
        return;
    ImGui::SeparatorText ("Displays");
    // What the user just set, until the owner has taken it; what the owner says otherwise.
    const Displays& shown = store->displaysPending ? store->displays : own.displays;
    Displays wanted = shown;
    std::string what;

    if (ImGui::Checkbox ("Existing geometry wireframe##tapioca.display.wireframe", &wanted.wireframeOn))
        what = wanted.wireframeOn ? "wireframe on" : "wireframe off";
    hudshell::Tip (
        "Show the existing-model reference wireframe in 3D and outlines in 2D; analysis overlays stay visible.");
    if (ImGui::Checkbox ("Model storey cuts##tapioca.display.slices", &wanted.slicesOn))
        what = wanted.slicesOn ? "slices on" : "slices off";
    hudshell::Tip ("Each storey's floor outlined on the model, with its area: the selected massing slabs' floors, "
                   "or the whole model cut at every storey");
    if (wanted.slicesOn) {
        int source = wanted.slicesFromModel ? 1 : 0;
        const char* const sources[] = { "selected slabs", "whole model" };
        if (ImGui::Combo ("Source##from", &source, sources, 2)) {
            wanted.slicesFromModel = source == 1;
            what = "slices source";
        }
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
    changes.push_back ({ "display", {}, "Settings", what, -1, 1.0, what, true });
}

void Engine::Impl::SliceStyles ()
{
    if (!own.standalone || !ImGui::TreeNode ("Story slice display styles"))
        return;
    const Displays& shown = store->displaysPending ? store->displays : own.displays;
    Displays wanted = shown;
    std::string what;
    ImGui::TextWrapped ("Shared by storey and massing slices. Visibility and area labels are controlled in Massing.");
    if (ImGui::BeginTable ("##tapioca.display.slices", 2,
                           ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn ("##label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn ("##value", ImGuiTableColumnFlags_WidthStretch);
        auto& look = wanted.slices;
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
    ImGui::TreePop ();
    if (what.empty ())
        return;
    store->displays = wanted;
    store->displaysPending = true;
    changes.push_back ({ "display", {}, "Graphics style lab", what, -1, 1.0, what, true });
}

} // namespace overlayhud
} // namespace archviz
} // namespace geomsrv
