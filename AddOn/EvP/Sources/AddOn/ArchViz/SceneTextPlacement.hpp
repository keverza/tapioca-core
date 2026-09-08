#ifndef EVP_ARCHVIZ_SCENETEXTPLACEMENT_HPP
#define EVP_ARCHVIZ_SCENETEXTPLACEMENT_HPP

#include <vector>

namespace geomsrv::archviz {

struct SceneTextBounds {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

struct SceneTextPlacement {
    bool accepted = false;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    SceneTextBounds bounds;
};

SceneTextPlacement ResolveSceneTextPlacement (const SceneTextBounds& bounds, float surfaceWidth, float surfaceHeight,
                                              float edgeInset, float overlapGap,
                                              const std::vector<SceneTextBounds>& occupiedBounds);

} // namespace geomsrv::archviz

#endif
