// ArchViz/OverlayGuestText -- see the header.

#include "ArchViz/OverlayGuestText.hpp"

#include "ArchViz/OverlayInput.hpp" // ViewName

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/OverlayFonts.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayText.hpp"
#include "ArchViz/SceneTextFont.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace guesttext {

namespace {

std::unique_ptr<overlaytext::Engine> g_engine; // MAIN THREAD
bool g_failed = false;
std::unique_ptr<overlayhud::Engine> g_hud[2]; // by view
bool g_hudFailed[2] = { false, false };
// What the user did to the panels, the views' engines' both (OverlayHud.hpp `State`).
std::shared_ptr<overlayhud::State> g_hudState;
// A caller's fonts by path; null for one that failed, so it is said once.
std::map<std::string, std::unique_ptr<overlaytext::Engine>> g_fonts;

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

overlaytext::Engine* EngineFor (const std::string& path)
{
    if (path.empty ())
        return Engine ();
    const auto found = g_fonts.find (path);
    if (found != g_fonts.end ())
        return found->second.get ();
    std::unique_ptr<overlaytext::Engine> engine;
    std::string error;
    std::vector<uint8_t> font;
    if (g_fonts.size () >= kMaxFonts)
        error = "the overlays already hold " + std::to_string (kMaxFonts) + " fonts";
    else if (overlayfonts::Read (path, font, error)) {
        engine = std::make_unique<overlaytext::Engine> ();
        if (!engine->Init (std::move (font), error, overlaytext::Engine::SmallSeedText ()))
            engine.reset ();
    }
    if (engine != nullptr)
        ArchVizLog ("OVERLAY TEXT  font \"" + path + "\" ready in " +
                    std::to_string (engine->GetStats ().seedMilliseconds) + " ms");
    else
        ArchVizLog ("OVERLAY TEXT  font \"" + path + "\" NOT AVAILABLE: " + error +
                    " -- its texts use the bundled font");
    overlaytext::Engine* const made = engine.get ();
    g_fonts[path] = std::move (engine);
    return made;
}

overlayhud::Engine* Hud (overlayinput::View view)
{
    const size_t at = view == overlayinput::View::ThreeD ? 0 : 1;
    if (g_hud[at] != nullptr)
        return g_hud[at].get ();
    if (g_hudFailed[at])
        return nullptr;
    std::vector<uint8_t> font;
    std::string error;
    auto hud = std::make_unique<overlayhud::Engine> ();
    if (!LoadBundledSceneTextFont (font, error) || !hud->Init (std::move (font), error)) {
        g_hudFailed[at] = true;
        ArchVizLog ("OVERLAY HUD  NOT AVAILABLE: " + error + " -- HUD panels are not drawn; everything else is");
        return nullptr;
    }
    ArchVizLog (std::string ("OVERLAY HUD  the ") + overlayinput::ViewName (view) +
                " HUD ready: Dear ImGui over the bundled font, laid out on the main thread");
    hud->SetFontLoader (&overlayfonts::Read);
    if (g_hudState == nullptr)
        g_hudState = overlayhud::NewState ();
    hud->UseState (g_hudState);
    g_hud[at] = std::move (hud);
    return g_hud[at].get ();
}

void ForgetHudState ()
{
    if (g_hudState != nullptr)
        overlayhud::ClearState (*g_hudState);
}

} // namespace guesttext
} // namespace archviz
} // namespace geomsrv
