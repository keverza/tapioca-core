#ifndef EVP_EDIT_EDITPATHCROSSING_HPP
#define EVP_EDIT_EDITPATHCROSSING_HPP

// Where paths cross or meet, they share a height. A crossing of two centrelines, or a
// path's end lying on another path (a T, or an L where two ends meet), is a junction:
// each path keeps a grade break there (GradeBreak::crossing names the other path), and
// the two breaks' elevations agree -- equal at a centreline crossing, the end on the other
// path's surface (its crossfall included) at a T. The breaks are the paths' own: they
// follow the junction when either path moves, go when the paths part, and an elevation
// typed or dragged on either is carried to the other. Pure geometry on the document; the
// editing session settles every accepted edit through it (EditSession.cpp).
#include "Edit/EditDocument.hpp"
#include <vector>

namespace geomsrv::edit {

struct Junction {
    size_t a = 0, b = 0;                   // indices into the paths
    double stationA = 0.0, stationB = 0.0; // where it is along each
    // Path a's surface there above path b's centre elevation at stationB: 0 where the
    // centrelines cross, the crossfall's drop where a's end lies off b's centreline.
    double rise = 0.0;
    Vec3 at; // plan
};
// Every junction among `paths`; paths that do not validate have none.
std::vector<Junction> FindJunctions (const std::vector<Path>& paths);

// Puts each junction's breaks in place, retires those whose junction has gone, and makes
// each pair's elevations agree: the one the edit changed (against `previous`) wins, a fixed
// one always does, and otherwise the older path's (lower id) -- so a new path takes the
// heights of the paths it meets. A path its links would leave invalid is left as it was.
void LinkCrossings (Document& doc, const Document& previous);

} // namespace geomsrv::edit
#endif
