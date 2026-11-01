#ifndef EVP_ARCHVIZ_ORBITANCHOR_HPP
#define EVP_ARCHVIZ_ORBITANCHOR_HPP

// ArchViz/OrbitAnchor -- which world point the viewer's orbit turns about (Camera::SetOrbitPivot).
//
// ⚠️ THE SELECTION FIRST, THEN THE SCENE (the user, issue #88/#9: the orbit's centre starts in the
// scene's bounding box; a selected object becomes the navigation anchor). In a large scene the
// target Archicad's camera arrives with is wherever it was looking, and an orbit about it throws
// the model out of the window. With no bounds at all -- the model has not arrived -- there is no
// anchor and the orbit turns about the target, as it always did.
//
// Pure, header-only, so the choice is tested offline (test_camera_orbitpivot.cpp).

#include <cstdint>

namespace geomsrv {
namespace archviz {

enum class OrbitAnchorSource : uint8_t { None = 0, Scene = 1, Selection = 2 };

struct OrbitAnchor {
    OrbitAnchorSource source = OrbitAnchorSource::None;
    float point[3] = { 0.0f, 0.0f, 0.0f };
};

// The middle of the selection's box when it has one, else of the scene's; `nullptr` for a box
// that is not known.
inline OrbitAnchor ChooseOrbitAnchor (const float* selectionMin, const float* selectionMax, const float* sceneMin,
                                      const float* sceneMax)
{
    OrbitAnchor anchor;
    const float* lo = nullptr;
    const float* hi = nullptr;
    if (selectionMin != nullptr && selectionMax != nullptr) {
        lo = selectionMin, hi = selectionMax;
        anchor.source = OrbitAnchorSource::Selection;
    }
    else if (sceneMin != nullptr && sceneMax != nullptr) {
        lo = sceneMin, hi = sceneMax;
        anchor.source = OrbitAnchorSource::Scene;
    }
    else
        return anchor;
    for (int i = 0; i < 3; ++i)
        anchor.point[i] = 0.5f * (lo[i] + hi[i]);
    return anchor;
}

inline const char* OrbitAnchorName (OrbitAnchorSource source)
{
    switch (source) {
        case OrbitAnchorSource::Selection:
            return "selection";
        case OrbitAnchorSource::Scene:
            return "scene";
        default:
            return "target";
    }
}

} // namespace archviz
} // namespace geomsrv

#endif
