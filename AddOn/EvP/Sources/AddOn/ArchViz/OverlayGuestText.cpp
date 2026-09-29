// ArchViz/OverlayGuestText -- see the header.

#include "ArchViz/OverlayGuestText.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/OverlayHud.hpp"
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
std::unique_ptr<overlayhud::Engine> g_hud;
bool g_hudFailed = false;

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

overlayhud::Engine* Hud ()
{
    if (g_hud != nullptr)
        return g_hud.get ();
    if (g_hudFailed)
        return nullptr;
    std::vector<uint8_t> font;
    std::string error;
    auto hud = std::make_unique<overlayhud::Engine> ();
    if (!LoadBundledSceneTextFont (font, error) || !hud->Init (std::move (font), error)) {
        g_hudFailed = true;
        ArchVizLog ("OVERLAY HUD  NOT AVAILABLE: " + error + " -- HUD panels are not drawn; everything else is");
        return nullptr;
    }
    ArchVizLog ("OVERLAY HUD  ready: Dear ImGui over the bundled font, laid out on the main thread");
    g_hud = std::move (hud);
    return g_hud.get ();
}

} // namespace guesttext
} // namespace archviz
} // namespace geomsrv
