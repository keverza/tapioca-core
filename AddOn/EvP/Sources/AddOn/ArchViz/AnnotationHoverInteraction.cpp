#include "ArchViz/AnnotationHoverInteraction.hpp"

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/InputRingBuffer.hpp"

#include <chrono>
#include <optional>

namespace geomsrv::archviz {

AnnotationPrimitiveFilter UpdateAnnotationHover (const annotation::Frame& frame,
                                                 const std::shared_ptr<const annotation::DrawList>& source,
                                                 std::size_t nodeIndex, std::size_t frameIndex,
                                                 const float viewProj[16], uint32_t width, uint32_t height,
                                                 float dpiScale, bool fitSelectedFrame, const HudState& hudState,
                                                 const InputSnapshot& input, DimensionHoverState& state)
{
    if (!hudState.annotationDimensionsOnHover) {
        UpdateDimensionHover (state, source, nodeIndex, frameIndex, std::nullopt, false,
                              std::chrono::steady_clock::now ());
        return [] (std::size_t, const annotation::Primitive& primitive) { return !primitive.hoverOnly; };
    }
    const bool eligible = input.inside && !hudState.wantsMouse && input.buttons == kMouseNone && !input.navButton &&
                          input.wheelDelta == 0;
    const std::optional<std::size_t> hit =
        eligible ? HitTestTraceDimension (frame, viewProj, width, height, dpiScale, fitSelectedFrame,
                                          { float (input.x), float (input.y) }, hudState.annotationTextHeightMetres,
                                          hudState.annotationHideBelowPixels, hudState.annotationCapAbovePixels)
                 : std::nullopt;
    const std::optional<std::size_t> visible =
        UpdateDimensionHover (state, source, nodeIndex, frameIndex, hit, eligible, std::chrono::steady_clock::now ());
    return [visible] (std::size_t primitiveIndex, const annotation::Primitive& primitive) {
        return primitive.kind != annotation::PrimitiveKind::Dimension || primitive.alwaysVisible ||
               (visible.has_value () && primitiveIndex == *visible);
    };
}

} // namespace geomsrv::archviz
