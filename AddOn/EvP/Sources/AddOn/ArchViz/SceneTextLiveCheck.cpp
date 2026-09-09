#include "ArchViz/SceneTextLiveCheck.hpp"

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/SceneTextLayer.hpp"

#include <imgui.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace geomsrv::archviz {

namespace {

float LiveCheckHalo (float size)
{
    if (size <= 16.0f)
        return 0.0f;
    if (size <= 24.0f)
        return (size - 16.0f) / 16.0f;
    return 0.5f + (size - 24.0f) / 32.0f;
}

} // namespace

void DrawSceneTextLiveCheckControls (HudState& state)
{
    if (!ImGui::CollapsingHeader ("scene text"))
        return;

    ImGui::Checkbox ("show production text sample", &state.showSceneTextLiveCheck);
    ImGui::Checkbox ("show edge / overlap placement", &state.showSceneTextPlacementCheck);
    ImGui::Checkbox ("show always / hide / fade lines", &state.showSceneTextOcclusionCheck);
    ImGui::TextDisabled ("HarfBuzz shaping + linear MTSDF, not ImGui text");
    if (state.showSceneTextLiveCheck || state.showSceneTextPlacementCheck || state.showSceneTextOcclusionCheck) {
        ImGui::SetNextItemWidth (-1.0f);
        ImGui::SliderFloat ("##scenetextsize", &state.sceneTextCheckSizePixels, 12.0f, 72.0f, "%.0f px");
        ImGui::TextDisabled ("sample size");
        const float size = std::clamp (state.sceneTextCheckSizePixels, 12.0f, 72.0f);
        const float whiteHalo = LiveCheckHalo (size);
        ImGui::TextDisabled ("white halo %.2f px; coloured halo 0 px", whiteHalo);
    }
    if (state.showSceneTextOcclusionCheck) {
        ImGui::DragFloat3 ("anchor XYZ", state.sceneTextOcclusionAnchor, 0.1f, -100000.0f, 100000.0f, "%.2f m");
        ImGui::SliderFloat ("line spacing", &state.sceneTextOcclusionSpacingMetres, 0.05f, 5.0f, "%.2f m");
        ImGui::TextDisabled ("move the three world anchors behind solid geometry");
    }
    if (state.showSceneTextPlacementCheck)
        ImGui::TextDisabled ("edge labels stay inset; the red overlap label stays hidden");
    ImGui::TextDisabled ("atlas %ux%u, %llu bytes", state.sceneTextAtlasWidth, state.sceneTextAtlasHeight,
                         (unsigned long long) state.sceneTextAtlasBytes);
    ImGui::TextDisabled ("pages %u | pending %u | staging %llu bytes", state.sceneTextAtlasPages,
                         state.sceneTextPendingGlyphs, (unsigned long long) state.sceneTextStagingBytes);
    ImGui::TextDisabled ("misses %llu | uploads %llu | evictions %llu", (unsigned long long) state.sceneTextAtlasMisses,
                         (unsigned long long) state.sceneTextAtlasUploads,
                         (unsigned long long) state.sceneTextAtlasEvictions);
    ImGui::TextDisabled ("rejected %llu | generation/upload failures %llu/%llu",
                         (unsigned long long) state.sceneTextAtlasRejected,
                         (unsigned long long) state.sceneTextAtlasGenerationFailures,
                         (unsigned long long) state.sceneTextAtlasUploadFailures);
    ImGui::TextDisabled ("generation %.2f ms | upload %.2f ms", state.sceneTextGenerationMicroseconds / 1000.0,
                         state.sceneTextUploadMicroseconds / 1000.0);
    ImGui::TextDisabled ("last pass: %llu label(s), %llu glyph(s), %llu draw(s)",
                         (unsigned long long) state.sceneTextLabels, (unsigned long long) state.sceneTextGlyphs,
                         (unsigned long long) state.sceneTextDrawCalls);
}

void DrawSceneTextOcclusionLiveCheck (SceneTextLayer& layer, Diligent::IRenderDevice* device,
                                      Diligent::IDeviceContext* context, Diligent::ITextureView* depthView,
                                      HudState& state, const float placementViewProj[16], const float depthViewProj[16],
                                      uint32_t width, uint32_t height, float dpiScale, float nearClip, float farClip,
                                      bool perspective)
{
    if (!state.showSceneTextOcclusionCheck || state.readOnly || !layer.IsReady ())
        return;
    const float size = std::clamp (state.sceneTextCheckSizePixels, 12.0f, 72.0f);
    const float spacing = std::clamp (state.sceneTextOcclusionSpacingMetres, 0.05f, 5.0f);
    const float halo = LiveCheckHalo (size);
    std::vector<SceneTextLabel> labels;
    labels.reserve (3);
    const auto add = [&] (float zOffset, const char* text, uint32_t rgba, SceneTextOcclusion occlusion) {
        SceneTextLabel label;
        label.anchor[0] = state.sceneTextOcclusionAnchor[0];
        label.anchor[1] = state.sceneTextOcclusionAnchor[1];
        label.anchor[2] = state.sceneTextOcclusionAnchor[2] + zOffset;
        label.text = text;
        label.sizePixels = size;
        label.rgba = rgba;
        label.alignment = SceneTextAlignment::Center;
        label.haloRgba = halo > 0.0f ? 0x000000D8u : 0u;
        label.haloWidthPixels = halo;
        label.occlusion = occlusion;
        label.allowOverlap = true;
        labels.push_back (std::move (label));
    };
    add (spacing, "ALWAYS | fully visible", 0xFFE08AFFu, SceneTextOcclusion::Always);
    add (0.0f, "HIDE | disappears behind depth", 0x8FE8FFFFu, SceneTextOcclusion::Hide);
    add (-spacing, "FADE | 25% behind depth", 0xB7F7A8FFu, SceneTextOcclusion::Fade);
    layer.Draw (device, context, depthView, labels, placementViewProj, depthViewProj, width, height, dpiScale, nearClip,
                farClip, perspective);
}

void DrawSceneTextLiveCheck (SceneTextLayer& layer, Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                             HudState& state, uint32_t width, uint32_t height, float dpiScale)
{
    if ((state.showSceneTextLiveCheck || state.showSceneTextPlacementCheck) && !state.readOnly && layer.IsReady ()) {
        const float x = width >= 720 ? float (width) * 0.48f : 16.0f;
        const float size = std::clamp (state.sceneTextCheckSizePixels, 12.0f, 72.0f);
        const float whiteHalo = LiveCheckHalo (size);
        std::vector<ScreenLabel> labels;
        labels.reserve (9);
        const auto add = [&] (float labelX, float y, const char* text, uint32_t rgba, float halo,
                              bool centered = false) {
            ScreenLabel label;
            label.anchor = { labelX, y };
            label.text = text;
            label.rgba = rgba;
            label.fontSize = size;
            label.haloRgba = halo > 0.0f ? 0x000000D8u : 0u;
            label.haloWidthPixels = halo;
            label.backgroundPanel = false;
            label.centered = centered;
            labels.push_back (std::move (label));
        };
        if (state.showSceneTextLiveCheck) {
            add (x, 64.0f, "Tapioca HarfBuzz + MTSDF", 0xFFFFFFFFu, whiteHalo);
            add (x, 64.0f + size * 1.35f, "Ąžuolų plotas 42 m² | 18° | Ø250", 0xFFE08AFFu, 0.0f);
            add (x, 64.0f + size * 2.70f, "office affine AV To Wa", 0x9FE8FFFFu, 0.0f);
            add (x, 64.0f + size * 4.05f, "Café = Café", 0xFFFFFFFFu, whiteHalo);
            add (x, 64.0f + size * 5.40f, "Łódź | ősz | fațadă", 0xB7F7A8FFu, 0.0f);
        }
        if (state.showSceneTextPlacementCheck) {
            const float edgeY = float (height) * 0.55f;
            const float overlapY = float (height) * 0.72f;
            add (0.0f, edgeY, "LEFT EDGE", 0xFFE08AFFu, 0.0f, true);
            add (float (width), edgeY, "RIGHT EDGE", 0x9FE8FFFFu, 0.0f, true);
            add (float (width) * 0.5f, overlapY, "FIRST LABEL WINS", 0xB7F7A8FFu, 0.0f, true);
            add (float (width) * 0.5f, overlapY, "ERROR: OVERLAP VISIBLE", 0xFF6767FFu, 0.0f, true);
        }
        layer.DrawProjected (device, context, labels, width, height, dpiScale);
    }
    const SceneTextLayerStats stats = layer.Stats ();
    state.sceneTextLabels = stats.labels;
    state.sceneTextGlyphs = stats.glyphs;
    state.sceneTextAtlasBytes = stats.atlasBytes;
    state.sceneTextAtlasWidth = stats.atlasWidth;
    state.sceneTextAtlasHeight = stats.atlasHeight;
    state.sceneTextAtlasPages = stats.atlasPages;
    state.sceneTextPendingGlyphs = stats.pendingGlyphs;
    state.sceneTextStagingBytes = stats.stagingBytes;
    state.sceneTextAtlasMisses = stats.atlasMisses;
    state.sceneTextAtlasUploads = stats.atlasUploads;
    state.sceneTextAtlasUploadFailures = stats.atlasUploadFailures;
    state.sceneTextAtlasEvictions = stats.atlasEvictions;
    state.sceneTextAtlasRejected = stats.atlasRejected;
    state.sceneTextAtlasGenerationFailures = stats.atlasGenerationFailures;
    state.sceneTextGenerationMicroseconds = stats.atlasGenerationMicroseconds;
    state.sceneTextUploadMicroseconds = stats.atlasUploadMicroseconds;
    state.sceneTextDrawCalls = stats.drawCalls;
}

} // namespace geomsrv::archviz
