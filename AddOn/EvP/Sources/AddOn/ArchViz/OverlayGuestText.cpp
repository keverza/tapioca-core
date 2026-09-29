// ArchViz/OverlayGuestText -- see the header.

#include "ArchViz/OverlayGuestText.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/OverlayText.hpp"
#include "ArchViz/SceneTextFont.hpp"

#include <memory>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace guesttext {

namespace {

std::unique_ptr<overlaytext::Engine> g_engine; // MAIN THREAD
bool g_failed = false;

} // namespace

overlaytext::Engine* Engine ()
{
    if (g_engine != nullptr)
        return g_engine.get ();
    if (g_failed)
        return nullptr;
    std::vector<uint8_t> font;
    std::string error;
    auto engine = std::make_unique<overlaytext::Engine> ();
    if (!LoadBundledSceneTextFont (font, error) || !engine->Init (std::move (font), error)) {
        g_failed = true;
        ArchVizLog ("OVERLAY TEXT  NOT AVAILABLE: " + error +
                    " -- labels, dimensions' text and legends' values "
                    "are not drawn; everything else is");
        return nullptr;
    }
    ArchVizLog ("OVERLAY TEXT  ready: the bundled font's seed atlas in " +
                std::to_string (engine->GetStats ().seedMilliseconds) + " ms");
    g_engine = std::move (engine);
    return g_engine.get ();
}

} // namespace guesttext
} // namespace archviz
} // namespace geomsrv
