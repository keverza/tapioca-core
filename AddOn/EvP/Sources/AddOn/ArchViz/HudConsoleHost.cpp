#include "APIEnvir.h"
#include "ACAPinc.h"
#include "ArchViz/HudConsole.hpp"
#include "ArchViz/SelectionMetadata.hpp"
#include "Python/PathUtils.hpp"
#include <shellapi.h>

namespace geomsrv::archviz::hudconsole {
void OpenLogs ()
{
    // Both HUDs can request this; viewer layout runs on the render thread.
    // Defer shell/file work to the main message loop, outside ImGui's lock.
    if (!selectionmetadata::Later ([] () {
            const auto root = evp::EvpDataDir ();
            if (root.IsEmpty ()) {
                Warning ("Logs", "The local Tapioca data folder could not be resolved.");
                return;
            }
            const GS::UniString directory (root + GS::UniString ("\\logs"));
            evp::CreateDirectoryChain (directory);
            const auto launched =
                ShellExecuteW (nullptr, L"open", reinterpret_cast<LPCWSTR> (directory.ToUStr ().Get ()), nullptr,
                               nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR> (launched) <= 32)
                Warning ("Logs", "The Tapioca logs folder could not be opened in Explorer.");
        }))
        Warning ("Logs", "Opening the logs folder was not queued: the project message loop is unavailable.");
}
} // namespace geomsrv::archviz::hudconsole
