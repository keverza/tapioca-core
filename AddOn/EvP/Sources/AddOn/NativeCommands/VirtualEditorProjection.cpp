#include "NativeCommands/VirtualEditorProjection.hpp"

namespace geomsrv::nativecommands {

bool VirtualEditorCrossfallPercent (const edit::PathCrossfall& crossfall, double& percent, std::string& error)
{
    if (!edit::PathCrossfallPercent (crossfall, percent, error)) {
        error = "VirtualEditorCrossfall: " + error;
        return false;
    }
    return true;
}

} // namespace geomsrv::nativecommands
