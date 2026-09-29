// ArchViz/ImGuiContextLock -- see the header.

#include "ArchViz/ImGuiContextLock.hpp"

namespace geomsrv {
namespace archviz {

std::mutex& ImGuiContextMutex ()
{
    static std::mutex mutex;
    return mutex;
}

} // namespace archviz
} // namespace geomsrv
