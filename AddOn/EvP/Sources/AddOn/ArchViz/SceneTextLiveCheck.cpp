#include "ArchViz/SceneTextLiveCheck.hpp"

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/SceneTextLayer.hpp"

#include <imgui.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace geomsrv::archviz {

void DrawSceneTextLiveCheckControls (HudState& state)
{
    if (!ImGui::CollapsingHeader ("scene text"))
        return;

    ImGui::Checkbox ("show production text sample", &state.showSceneTextLiveCheck);
    ImGui::TextDisabled ("HarfBuzz shaping + linear MTSDF, not ImGui text");
    if (state.showSceneTextLiveCheck) {
        ImGui::SetNextItemWidth (-1.0f);
        ImGui::SliderFloat ("##scenetextsize", &state.sceneTextCheckSizePixels, 12.0f, 72.0f, "%.0f px");
        ImGui::TextDisabled ("sample size");
        const float size = std::clamp (state.sceneTextCheckSizePixels, 12.0f, 72.0f);
        const float whiteHalo = size <= 24.0f ? size / 48.0f : 0.5f + (size - 24.0f) / 32.0f;
        ImGui::TextDisabled ("white halo %.2f px; coloured halo 0 px", whiteHalo);
    }
    ImGui::TextDisabled ("atlas %ux%u, %llu bytes", state.sceneTextAtlasWidth, state.sceneTextAtlasHeight,
                         (unsigned long long) state.sceneTextAtlasBytes);
    ImGui::TextDisabled ("pages %u | pending %u | staging %llu bytes", state.sceneTextAtlasPages,
                         state.sceneTextPendingGlyphs, (unsigned long long) state.sceneTextStagingBytes);
    ImGui::TextDisabled ("misses %llu | uploads %llu | evictions %llu",
                         (unsigned long long) state.sceneTextAtlasMisses,
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

void DrawSceneTextLiveCheck (SceneTextLayer& layer, Diligent::IRenderDevice* device,
                             Diligent::IDeviceContext* context, HudState& state, uint32_t width,
                             uint32_t height, float dpiScale)
{
    if (state.showSceneTextLiveCheck && !state.readOnly && layer.IsReady ()) {
        const float x = width >= 720 ? float (width) * 0.48f : 16.0f;
        const float size = std::clamp (state.sceneTextCheckSizePixels, 12.0f, 72.0f);
        const float whiteHalo = size <= 24.0f ? size / 48.0f : 0.5f + (size - 24.0f) / 32.0f;
        std::vector<ScreenLabel> labels;
        labels.reserve (5);
        const auto add = [&] (float y, const char* text, uint32_t rgba, float halo) {
            ScreenLabel label;
            label.anchor = { x, y };
            label.text = text;
            label.rgba = rgba;
            label.fontSize = size;
            label.haloRgba = halo > 0.0f ? 0x000000D8u : 0u;
            label.haloWidthPixels = halo;
            label.backgroundPanel = false;
            labels.push_back (std::move (label));
        };
        add (64.0f, "Tapioca HarfBuzz + MTSDF", 0xFFFFFFFFu, whiteHalo);
        add (64.0f + size * 1.35f, "Ąžuolų plotas 42 m² | 18° | Ø250", 0xFFE08AFFu, 0.0f);
        add (64.0f + size * 2.70f, "office affine AV To Wa", 0x9FE8FFFFu, 0.0f);
        add (64.0f + size * 4.05f, "Café = Café", 0xFFFFFFFFu, whiteHalo);
        add (64.0f + size * 5.40f, "Łódź | ősz | fațadă", 0xB7F7A8FFu, 0.0f);
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
