#ifndef EVP_ARCHVIZ_DILIGENTHUDNAMES_HPP
#define EVP_ARCHVIZ_DILIGENTHUDNAMES_HPP

// ArchViz/DiligentHudNames -- what the viewer HUD's combos call the debug views, the render
// modes and the render qualities: one copy, for the read-only summary and the tabs alike.

#include "ArchViz/DiligentScene.hpp"   // SceneRenderMode, RenderQuality
#include "ArchViz/DiligentShaders.hpp" // DiligentDebugView

namespace geomsrv {
namespace archviz {

// The debug views, by the names the user already knows from the command's
// `debug_view` parameter. ⚠️ THE ORDER IS THE DiligentDebugView ENUM'S ORDER --
// the combo's index IS the value, so an insertion here silently renumbers them.
inline constexpr const char* kDebugViewNames[] = {
    "final",
    "normals",
    "lit",
    "base color",
    "sun vector",
    "shadow",
    "roughness",
    "G-buffer normals",
    "G-buffer depth",
    "ambient occlusion",
    "G-buffer albedo",
    "G-buffer roughness",
    "G-buffer material",
    "motion vectors",
};
inline constexpr int kDebugViewCount = int (sizeof (kDebugViewNames) / sizeof (kDebugViewNames[0]));

static_assert (kDebugViewCount == int (DiligentDebugView::MotionVectors) + 1,
               "the HUD's combo and DiligentDebugView have drifted apart");

// The render modes, in SceneRenderMode's order -- the combo's index IS the
// value, exactly as for the debug views above.
inline constexpr const char* kRenderModeNames[] = { "shaded", "wireframe", "shaded + wireframe" };
inline constexpr int kRenderModeCount = int (sizeof (kRenderModeNames) / sizeof (kRenderModeNames[0]));

static_assert (kRenderModeCount == int (SceneRenderMode::ShadedWireframe) + 1,
               "the HUD's combo and SceneRenderMode have drifted apart");

// The render qualities, in RenderQuality's order -- same rule again: the combo's
// index IS the enum value.
inline constexpr const char* kRenderQualityNames[] = { "fast", "realistic" };
inline constexpr int kRenderQualityCount = int (sizeof (kRenderQualityNames) / sizeof (kRenderQualityNames[0]));

static_assert (kRenderQualityCount == int (RenderQuality::Realistic) + 1,
               "the HUD's combo and RenderQuality have drifted apart");

} // namespace archviz
} // namespace geomsrv

#endif
