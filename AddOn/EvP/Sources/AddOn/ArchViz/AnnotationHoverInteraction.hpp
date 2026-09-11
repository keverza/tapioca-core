#ifndef EVP_ARCHVIZ_ANNOTATIONHOVERINTERACTION_HPP
#define EVP_ARCHVIZ_ANNOTATIONHOVERINTERACTION_HPP

#include "ArchViz/TraceAnnotationLayer.hpp"

namespace geomsrv::archviz {

struct HudState;
struct InputSnapshot;

AnnotationPrimitiveFilter UpdateAnnotationHover (const annotation::Frame& frame,
                                                 const std::shared_ptr<const annotation::DrawList>& source,
                                                 std::size_t nodeIndex, std::size_t frameIndex,
                                                 const float viewProj[16], uint32_t width, uint32_t height,
                                                 float dpiScale, bool fitSelectedFrame, const HudState& hudState,
                                                 const InputSnapshot& input, DimensionHoverState& state);

} // namespace geomsrv::archviz

#endif
