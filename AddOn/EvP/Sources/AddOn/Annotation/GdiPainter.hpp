#ifndef EVP_ANNOTATION_GDIPAINTER_HPP
#define EVP_ANNOTATION_GDIPAINTER_HPP

#include "Annotation/DrawList.hpp"

#include <windows.h>

namespace geomsrv::annotation {

struct GdiPaintOptions {
    int originX = 0;
    int originY = 0;
    int lineWidth = 2;
    // Retained for paint-call compatibility; primitive role colour always wins.
    COLORREF textColour = RGB (32, 32, 32);
    // Zero reads the target DC. Tests and offscreen callers can provide an
    // explicit scale to make fixed screen furniture deterministic.
    double dpiScale = 0.0;
};

COLORREF SemanticColour (SemanticRole role);
void PaintFrameGdi (HDC hdc, const Frame& frame, const Transform2D& transform, const GdiPaintOptions& options = {});

} // namespace geomsrv::annotation

#endif
