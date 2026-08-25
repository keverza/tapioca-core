#include "Preview/PreviewRuntimeState.hpp"

namespace evp::preview {

PreviewRuntimeState& PreviewRuntimeState::Get ()
{
    static PreviewRuntimeState state;
    return state;
}

} // namespace evp::preview
