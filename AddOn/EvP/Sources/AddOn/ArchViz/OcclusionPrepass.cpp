// The rule from OcclusionPrepass.hpp. See the header for why each clause is
// there; this file is the clause order and nothing else.

#include "ArchViz/OcclusionPrepass.hpp"

namespace geomsrv {
namespace archviz {

bool OcclusionPrepassWanted (const OcclusionPrepassInputs& inputs)
{
    if (!inputs.enabled)
        return false;
    if (inputs.elementCount == 0)
        return false;
    if (!inputs.modelIsDrawn)
        return false;
    // ⚠️ THE ONLY POSITIVE CASE, AND IT IS WRITTEN AS ONE. Testing
    // `!= Shaded && != ShadedWireframe` would silently opt a fourth mode in on
    // the day one is added, and the honest default for a new mode is "the
    // author says", not "inherits a prepass".
    return inputs.renderMode == SceneRenderMode::Wireframe;
}

} // namespace archviz
} // namespace geomsrv
