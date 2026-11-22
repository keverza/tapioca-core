#ifndef EVP_ARCHVIZ_EXTRACTIONDEFERREDUV_HPP
#define EVP_ARCHVIZ_EXTRACTIONDEFERREDUV_HPP

// ArchViz/ExtractionDeferredUv -- texture coordinates after the geometry (#14).
//
// A full pass spent about half its main-thread capture reading texture coordinates: one
// Polygon::GetTextureCoordinate call per corner, about 37 us each, 33.8 s of a 68.6 s capture on
// the live check's project (six identical 16,900-triangle meshes at 1.9 s each). The model appeared
// only when the last of them was read.
//
// So a full pass reads none: it still asks each polygon whether its material is textured (the
// cheap half), draws those elements without their coordinates, and notes them here. As soon as
// the full pass completes, ExtractionWorker::RunDeferredUv re-extracts exactly those elements
// with their coordinates, as a partial pass -- the same path an edit takes -- so textures land a
// little later and the model a lot sooner. A partial pass from an edit always reads them.
//
// Thread-safe: the main-thread slices note, the extraction worker takes.

#include <set>
#include <string>

namespace geomsrv {
namespace archviz {
namespace extractiondeferreduv {

// An element captured without the texture coordinates it has.
void Note (const std::string& guid);

// Every element noted since the last call, which forgets them.
std::set<std::string> Take ();

// The same, without forgetting: the scene cache stores these as not yet reusable (#14).
std::set<std::string> Peek ();

// True while the deferred pass runs. ⚠️ A PARTIAL PASS CAN TURN INTO A FULL ONE (an incremental
// baseline that no longer fits); that full pass must read the coordinates itself, or it would defer
// them again and ask for another deferred pass, without end.
bool Suspended ();

// A partial pass whose base no longer fits (a pool renumber, a missing or older baseline). An edit's
// pass becomes a full pass over the same acquired model: `filter` cleared, `partial` false, `full`
// true, and true returned. ⚠️ THE TEXTURE PASS (`refining`) DOES NOT: its base no longer fitting
// means Archicad changed the model under it, and a full pass there is what reloaded the whole scene
// after every edit (2026-11-21: 84-91 s each). It puts its elements back (Note), returns false, and
// the caller gives the pass up; the edit's own pass runs at its stamp, and the texture pass follows
// the next complete pass.
bool FallBackToFull (bool refining, std::set<std::string>& filter, bool& partial, bool& full);

} // namespace extractiondeferreduv
} // namespace archviz
} // namespace geomsrv

#endif
