#ifndef EVP_ARCHVIZ_DILIGENTHUDDIAGRAM_HPP
#define EVP_ARCHVIZ_DILIGENTHUDDIAGRAM_HPP
// The viewer's Massing tab (#201): the exploded diagram's controls and status, drawn by the
// renderer-owned HUD. RENDER THREAD: it reads DiagramModel's values and writes its controls;
// it never calls ACAPI and never reads massingslicesmodel's main-thread state.
#include <string>

namespace geomsrv::archviz::viewerhud {
constexpr char kMassingKey[] = "@viewer.massing";
// Draws the page. Returns true when the user asked to open the full Massing workflow
// (the main-thread HUD in this viewer); the caller routes that request.
bool DiagramPage (float ui);
} // namespace geomsrv::archviz::viewerhud
#endif
