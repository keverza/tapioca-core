#ifndef EVP_ARCHVIZ_ANNOTATIONSCREENLAYOUT_HPP
#define EVP_ARCHVIZ_ANNOTATIONSCREENLAYOUT_HPP

#include "ArchViz/TraceAnnotationLayer.hpp"

namespace geomsrv::archviz {

float AnnotationCandidateOccupancyPenalty (const ProjectedDrawList& drawList, const ScreenPoint& center,
                                           const ScreenTextExtent& extent, float rotationRadians, float gap,
                                           const ScreenTextMeasure& measureText);

void ResolveAnnotationLabelOverlaps (ProjectedDrawList& drawList, uint32_t width, uint32_t height, float dpiScale,
                                     const ScreenTextMeasure& measureText,
                                     AnnotationPlacementHistory* placementHistory);

} // namespace geomsrv::archviz

#endif
