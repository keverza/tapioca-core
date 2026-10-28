#ifndef TAPIOCA_HUD_VIRTUAL_EDITOR_DEBUG_HPP
#define TAPIOCA_HUD_VIRTUAL_EDITOR_DEBUG_HPP

#include "Edit/EditTerrainDesign.hpp"
#include <filesystem>
#include <memory>

namespace geomsrv {
struct Mesh;
}
namespace geomsrv::archviz::edithud {
struct Session;
struct TerrainDebugSource {
    std::shared_ptr<const Mesh> mesh;
    tapioca::geometry::TerrainDesignOptions options;
    bool nativeSolid = false, fresh = true, active = false;
    bool outcomeMatchesOptions = false;
};
// Retained input only: never recapture the native terrain or wait for a worker.
TerrainDebugSource GetTerrainDebugSource (const Session& session);
bool RequestDebugExport (Session& session);
// Consumed by the message-loop controller, never by layout or Present. The directory
// overload keeps filesystem tests away from the user's real dump bucket.
bool SaveDebugExport (Session& session, const std::filesystem::path& directory);
bool SaveDebugExport (Session& session);
bool EncodeDebugScene (const Session& session, std::string& text, std::string& error);
} // namespace geomsrv::archviz::edithud
#endif
