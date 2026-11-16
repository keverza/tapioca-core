#ifndef TAPIOCA_HUD_VIRTUAL_EDITOR_BAKE_HPP
#define TAPIOCA_HUD_VIRTUAL_EDITOR_BAKE_HPP

// tapioca-dev #177: the built terrain made into a native Archicad Mesh.
//
// The plan (Edit/EditTerrainBake.hpp) is computed from the built surface -- never from the
// field preview -- for the options in the panel, and drawn over the terrain while the preview
// is on: the outline, every level line and every point the Mesh would get. Nothing is written
// until Create is pressed. The create is requested from layout and done on the message loop
// by FollowTerrainBake (ArchViz/VirtualEditorTerrainBake.cpp) through the command dispatcher,
// which owns the undo step. The editor's document is not changed by it.
#include <string>

namespace geomsrv::edit {
struct BakePlan;
}
namespace geomsrv::archviz::overlaylayers {
struct Layer;
}
namespace geomsrv::archviz::edithud {
struct Session;

// The plan for the built surface and the panel's options, cached until either changes. Null,
// with `why`, when there is no built surface that follows the document.
const edit::BakePlan* TerrainBakePlan (Session& session, std::string* why = nullptr);
// The panel section: the two ways of choosing lines, their controls, the counts, and Create.
void TerrainBakePanel (Session& session);
// The cached plan over the terrain, while the preview is on and the plan is the built surface's.
void DrawTerrainBake (const Session& session, overlaylayers::Layer& layer);
// The File menu's entry: with nothing selected the Inspector shows the terrain, and the section is
// opened and scrolled to. False, with the reason in `message`, when there is no terrain design.
bool RevealTerrainBake (Session& session);
// Asks for the create; false, with the reason in `message`, when there is nothing sound to make.
bool RequestTerrainBake (Session& session);
// The native create's parameters for the cached plan, as JSON text with every number a real
// (a whole number written without a fraction does not reach the command as a double).
std::string TerrainBakeRequestJson (const Session& session);
// The create's answer: clears the request and says what happened.
void FinishTerrainBake (Session& session, bool created, const std::string& message);
} // namespace geomsrv::archviz::edithud
#endif
