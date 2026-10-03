// ArchViz/DiligentHudTabs -- the viewer HUD's pages but Settings (DiligentHudShell.hpp): Stats,
// Selection, the sun study's and Debug. They hold what the viewer's old single panel held, each
// thing once (HANDOFF-HudTabs.md: every control has exactly one home).

#include "ArchViz/DiligentHudShell.hpp"

#include "ArchViz/DiligentHudNames.hpp"
#include "ArchViz/DiligentHudSections.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/HudMetadata.hpp"
#include "ArchViz/SceneTextLiveCheck.hpp"
#include "ArchViz/SectionModel.hpp"
#include "ArchViz/SelectionMetadata.hpp"

#include <imgui.h>

#include <cstdio>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace viewerhud {

namespace {

using hudshell::Card;

constexpr uint32_t kAmber = 0xD9822BFFu;
constexpr uint32_t kRed = 0xD64545FFu;

std::string Format (const char* format, double value)
{
    char text[64] = {};
    std::snprintf (text, sizeof (text), format, value);
    return text;
}

std::string Count (size_t value)
{
    return std::to_string (value);
}

} // namespace

void StatsPage (HudState& state, const DiligentSceneStats& scene, float ui)
{
    const overlaylayers::Panel& look = hudshell::PlainLook ();
    std::vector<Card> cards;
    Card model;
    model.title = "Model";
    model.figures.push_back ({ "Elements", Count (scene.elements) });
    model.figures.push_back ({ "Triangles", Count (scene.triangles) });
    model.figures.push_back (
        { "Materials",
          Count (scene.materials) +
              (scene.materialMisses > 0 ? ", " + Count (scene.materialMisses) + " missed" : std::string ()) });
    if (scene.pointLayers > 0)
        model.figures.push_back ({ "Point clouds", Count (scene.pointLayers) + ", " + Count (scene.visiblePoints) +
                                                       " of " + Count (scene.points) + " points shown" });
    if (scene.pending > 0) {
        model.note = "Reading the model: " + Count (scene.pending) + " element(s) queued";
        model.noteRgba = kAmber;
    }
    cards.push_back (std::move (model));
    if (state.showStorySlices) {
        Card slices;
        slices.title = "Storey slices";
        if (!scene.storeySliceLayerReady) {
            slices.note = "The slice layer failed to create -- see archviz.log";
            slices.noteRgba = kRed;
        }
        else if (scene.storeySlices == 0) {
            slices.note = "No storey set yet -- refresh to cut the model";
            slices.noteRgba = kAmber;
        }
        else {
            slices.figures.push_back ({ "Storeys", Count (scene.storeySlices) });
            slices.figures.push_back ({ "Enclosed", Format ("%.0f", scene.storeySliceAreaM2) + " m\xC2\xB2" });
        }
        cards.push_back (std::move (slices));
    }
    if (state.showGhPreview && (state.ghPreviewMeshIndices > 0 || state.ghPreviewLineVertices > 0)) {
        Card preview;
        preview.title = "Grasshopper preview";
        preview.figures.push_back ({ "Triangles", Count (state.ghPreviewMeshIndices / 3) });
        preview.figures.push_back ({ "Curve segments", Count (state.ghPreviewLineVertices / 6) });
        if (state.ghPreviewTruncated) {
            preview.note = "Truncated at the drawable ceiling: not the whole result";
            preview.noteRgba = kRed;
        }
        cards.push_back (std::move (preview));
    }
    hudshell::Cards (cards, look, ui);
}

void SelectionPage (Shell& shell, const HudState& state, float ui)
{
    const overlaylayers::Panel& look = hudshell::PlainLook ();
    // ⚠️ THE SECTION IS ARCHICAD'S SELECTION'S, AS ON THE OVERLAYS, read on the main thread
    // (SectionModel.hpp `Published`); a value given to its floors is written to its slabs the
    // same way, and the section read again after. The picked floors are not drawn in the
    // viewer's scene.
    std::vector<hudmeta::Edit> assigned = hudsection::Diagram (sectionmodel::Published (), shell.floors, look, ui);
    if (!assigned.empty ())
        selectionmetadata::Request (std::move (assigned), {}, [] () { sectionmodel::Publish (); });
    const DiligentScene::ElementInfo& picked = state.selected;
    if (!picked.valid) {
        ImGui::TextDisabled ("Nothing picked: click an element in the viewer");
        return;
    }
    // ⚠️ EVERY FIGURE HERE IS THE EXTRACTED MESH'S, and the card says so: Archicad's own
    // quantities need an ACAPI read the render thread cannot make (DiligentHudSections.cpp's
    // selected-element window says why).
    const float sizeX = picked.boundsMax[0] - picked.boundsMin[0];
    const float sizeY = picked.boundsMax[1] - picked.boundsMin[1];
    const float sizeZ = picked.boundsMax[2] - picked.boundsMin[2];
    Card card;
    card.title = "Picked element";
    card.figures.push_back ({ "Footprint", Format ("%.3f", sizeX) + " x " + Format ("%.3f", sizeY) + " m" });
    card.figures.push_back ({ "Footprint area", Format ("%.3f", double (sizeX) * sizeY) + " m\xC2\xB2 (box)" });
    card.figures.push_back ({ "Height", Format ("%.3f", sizeZ) + " m" });
    card.figures.push_back (
        { "Elevation", Format ("%.3f", picked.boundsMin[2]) + " .. " + Format ("%.3f", picked.boundsMax[2]) + " m" });
    card.figures.push_back ({ "Box volume", Format ("%.3f", double (sizeX) * sizeY * sizeZ) + " m\xC2\xB3" });
    card.figures.push_back (
        { "Mesh", Count (picked.triangles) + " triangles, " + Count (picked.vertices) + " vertices" });
    card.figures.push_back (
        { "Materials", Count (picked.materialRanges) + (picked.hasTransparency ? ", some transparent" : "") });
    card.note = "Axis-aligned bounding box of the mesh -- not Archicad's computed quantities. " + picked.guid;
    hudshell::Cards ({ card }, look, ui);

    // ⚠️ ITS TAPIOCA METADATA IS READ AND WRITTEN ON THE MAIN THREAD (SelectionMetadata.hpp):
    // this is the render thread, which calls neither ACAPI nor the gate. The page is the one
    // last read for the picked element; an edit goes to it alone, in one undo step.
    std::vector<hudmeta::Edit> edits = hudmeta::Editor (selectionmetadata::PageOf (picked.guid), look, ui);
    if (!edits.empty ())
        selectionmetadata::Request (std::move (edits), { picked.guid });
}

void SunStudyPage (HudState& state, const DiligentSceneStats& scene)
{
    DrawSunStudyHudSection (state, scene);
}

void DebugPage (HudState& state, const DiligentSceneStats& scene, const Frame& frame, uint32_t width, uint32_t height,
                float ui)
{
    const overlaylayers::Panel& look = hudshell::PlainLook ();
    // ⚠️ THE COMBO'S INDEX IS THE ENUM VALUE (DiligentHudNames.hpp): a reordering is a static_assert.
    ImGui::SetNextItemWidth (-FLT_MIN);
    ImGui::Combo ("##debugview", &state.debugView, kDebugViewNames, kDebugViewCount);
    ImGui::TextDisabled ("debug view");

    std::vector<Card> cards;
    Card frameCard;
    frameCard.title = "Frame";
    frameCard.figures.push_back ({ "Rate", Format ("%.0f", state.fps) + " fps" });
    frameCard.figures.push_back ({ "This frame", Format ("%.1f", frame.frameMs) + " ms" });
    frameCard.figures.push_back (
        { "Worst (3 s)", Format ("%.0f", frame.worstMs) + " ms", frame.worstMs > 33.0f ? kAmber : 0u });
    frameCard.figures.push_back ({ "Size", std::to_string (width) + " x " + std::to_string (height) + " px" });
    frameCard.figures.push_back ({ "Frame latency", state.frameLatency == 0 ? std::string ("DXGI default (3)")
                                                                            : std::to_string (state.frameLatency) });
    cards.push_back (std::move (frameCard));
    Card gpu;
    gpu.title = "GPU";
    gpu.figures.push_back ({ "Adapter", state.adapter.empty () ? std::string ("not known yet") : state.adapter });
    gpu.figures.push_back ({ "Draws", Count (scene.drawCalls) });
    gpu.figures.push_back ({ "Materials in pool", Count (scene.materials) });
    if (frame.fontNote != nullptr && frame.fontNote[0] != 0)
        gpu.note = frame.fontNote;
    cards.push_back (std::move (gpu));
    hudshell::Cards (cards, look, ui);

    ImGui::Checkbox ("graph interaction lab", &state.showGraphInteractionLab);
    DrawSceneTextLiveCheckControls (state);
    // The study's machine limits are worth reading before the first study; while one is on
    // screen its whole section is the Sun study tab.
    if (!scene.sunStudy.drawing)
        DrawSunStudyHudSection (state, scene);
}

} // namespace viewerhud
} // namespace archviz
} // namespace geomsrv
