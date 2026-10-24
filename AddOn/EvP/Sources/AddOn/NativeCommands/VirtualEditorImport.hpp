#ifndef TAPIOCA_NATIVE_VIRTUAL_EDITOR_IMPORT_HPP
#define TAPIOCA_NATIVE_VIRTUAL_EDITOR_IMPORT_HPP

#include <string>
namespace geomsrv::archviz::edithud {
struct Session;
}
namespace geomsrv {
// What the selection becomes. Lines: editor lines, region boundaries in a landscape. Property:
// the one closed shape becomes the property line. Building: each closed shape a building
// outline. Each of those keeps its Archicad element's GUID on its landscape source. Entrances:
// the selected doors become entrances on the building outline they stand on.
enum class DraftingImport { Lines, Property, Building, Entrances };
// Main thread. The current selection's Lines, PolyLines, circular Arcs and Circles, and Fills'
// contours become editor lines at the working plane's height, in one undo step of the editor's
// own history; in a landscape each is a region-boundary source. Reads the selection and the
// elements only: no model or selection writes, no undo scope. False with a message when nothing
// could be read; otherwise `message` says what was added and what was skipped, and why.
bool ImportSelectedDrafting (archviz::edithud::Session& session, DraftingImport as, std::string& message);
} // namespace geomsrv
#endif
