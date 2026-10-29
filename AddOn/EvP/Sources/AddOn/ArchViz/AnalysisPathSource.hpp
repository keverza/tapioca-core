#ifndef EVP_ARCHVIZ_ANALYSISPATHSOURCE_HPP
#define EVP_ARCHVIZ_ANALYSISPATHSOURCE_HPP

// ArchViz/AnalysisPathSource -- reads the path for the shared analysis point
// (AnalysisPath.hpp) from the Polyline selected in Archicad. The HUD asks from the
// render thread; the read runs later on the main thread (SelectionMetadata's
// `Later`), and the HUD picks the result up by its serial.
//
// The point's height profile along the path (AnalysisProfile.hpp) is kept on the
// Polyline as Tapioca metadata: read with the path, written by RequestSave in one
// undo step.
//
// An imported Polyline is followed: RequestRefresh compares its modification
// stamp on the main thread and reads it again when Archicad has changed it.
//
// ANY THREAD: RequestImport, RequestRefresh, Latest, RequestSave, LastSave.

#include "ArchViz/AnalysisPath.hpp"
#include "ArchViz/AnalysisProfile.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace geomsrv::archviz::analysispathsource {

// Read the first Polyline in the Archicad selection, at its storey's level.
void RequestImport ();

struct Imported {
    uint64_t serial = 0;                            // grows with each finished import, read or not
    std::shared_ptr<const analysispath::Path> path; // null when it failed
    // The profile saved on the Polyline, or null for none (or an unreadable one,
    // said in `error` beside a path that did import).
    std::shared_ptr<const analysisprofile::Profile> profile;
    std::string error;
    // From RequestRefresh, not an import: the same Polyline read again after an
    // edit (`path` set, no profile -- the HUD's is kept), or `gone` when it was deleted.
    bool refreshed = false;
    bool gone = false;
};

Imported Latest ();

// Read the Polyline `guid` again if its stamp is no longer `stamp`. At most one
// check is queued at a time; a call while one is pending does nothing. Nothing is
// published while it is unchanged.
void RequestRefresh (std::string guid, uint64_t stamp);

// Store `profile` on the Polyline `guid` (an Archicad GUID string), as an undo step.
void RequestSave (std::string guid, analysisprofile::Profile profile);

struct Saved {
    uint64_t serial = 0; // grows with each finished save
    std::string guid;
    std::string error; // empty when it was stored
};

Saved LastSave ();

} // namespace geomsrv::archviz::analysispathsource

#endif
