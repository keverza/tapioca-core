#include "ArchViz/DiligentViewportSupport.hpp"

#include "Annotation/RetainedTraceSelection.hpp"
#include "ArchViz/AnnotationHoverInteraction.hpp"
#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/SceneTextLayer.hpp"
#include "ArchViz/SceneTextLiveCheck.hpp"

#include <windows.h>

#include <DeviceContext.h>

namespace geomsrv::archviz {

ProjectedDrawList UpdateAndDrawTraceAnnotations (
    SceneTextLayer& layer, Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, bool blanked,
    bool offscreen, bool annotationsOnly, void* nativeWindow, Diligent::ITextureView* colorTarget,
    Diligent::ITextureView* depthTarget, Diligent::ITextureView* depthView, const float viewProj[16], uint32_t width,
    uint32_t height, float nearClip, float farClip, bool perspective, HudState& hudState, const InputSnapshot& input,
    AnnotationPlacementHistory& placementHistory, DimensionHoverState& dimensionHoverState)
{
    ProjectedDrawList annotations;
    if (offscreen) {
        dimensionHoverState = {};
        return annotations;
    }

    context->SetRenderTargets (1, &colorTarget, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    float dpiScale = 1.0f;
    if (nativeWindow != nullptr) {
        const UINT dpi = GetDpiForWindow (static_cast<HWND> (nativeWindow));
        if (dpi != 0)
            dpiScale = float (dpi) / 96.0f;
    }
    if (!blanked) {
        const auto selected = annotation::SelectedRetainedFrameSnapshotCopy ();
        if (selected.has_value ()) {
            if (placementHistory.source != selected->drawList || placementHistory.nodeIndex != selected->nodeIndex ||
                placementHistory.frameIndex != selected->frameIndex) {
                placementHistory.source = selected->drawList;
                placementHistory.nodeIndex = selected->nodeIndex;
                placementHistory.frameIndex = selected->frameIndex;
                placementHistory.candidateByPrimitive.clear ();
            }
            const ScreenTextMeasure measureText = [&layer] (std::string_view text, float fontSize,
                                                            ScreenTextExtent& extent) {
                return layer.MeasureProjectedText (text, fontSize, extent);
            };
            const AnnotationPrimitiveFilter primitiveFilter = UpdateAnnotationHover (
                selected->SelectedFrame (), selected->drawList, selected->nodeIndex, selected->frameIndex, viewProj,
                width, height, dpiScale, annotationsOnly, hudState, input, dimensionHoverState);
            annotations = BuildTraceAnnotations (
                selected->SelectedFrame (), viewProj, width, height, dpiScale, annotationsOnly, measureText,
                &placementHistory, hudState.annotationTextHeightMetres, hudState.annotationHideBelowPixels,
                hudState.annotationCapAbovePixels, primitiveFilter);
            if (!annotations.labels.empty () && layer.IsReady () &&
                layer.DrawProjected (device, context, depthView, annotations.labels, width, height, dpiScale, nearClip,
                                     farClip, perspective)) {
                annotations.labels.clear ();
            }
        }
        else {
            dimensionHoverState = {};
        }
    }
    else {
        dimensionHoverState = {};
    }
    if (!annotationsOnly)
        DrawSceneTextLiveCheck (layer, device, context, hudState, width, height, dpiScale);
    context->SetRenderTargets (1, &colorTarget, depthTarget, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    return annotations;
}

} // namespace geomsrv::archviz
