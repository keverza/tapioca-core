#ifndef EVP_EDIT_EDITGRADETOCONTROLS_HPP
#define EVP_EDIT_EDITGRADETOCONTROLS_HPP

// #109: grade paths to control points. Each enabled fixed-height control point (an authored vertex,
// its Z the height: LandscapeTerrainControl) within `radius` of a path's centreline in plan fixes the
// path's elevation at the station it projects to. A grade break already within `radius` of that
// station takes the height (and, between the ends, the station); otherwise a break is added there.
// The grade runs straight between breaks as it always does, a path end with no control in reach
// keeps its height, and a path with none in reach is left as it is. Two controls within `radius` of
// each other along one path: the one nearer the centreline wins and the other is reported.
// One document edit; nothing changes when nothing is in reach.
#include "Edit/EditDocument.hpp"
#include <string>
#include <vector>

namespace tapioca::geometry {

struct GradeToControlsReport {
    struct Graded {
        geomsrv::edit::Id path = geomsrv::edit::kNoId;
        size_t points = 0; // control points it now follows
    };
    std::vector<Graded> graded;               // paths that changed
    std::vector<geomsrv::edit::Id> untouched; // selected paths with no control in reach
    size_t controls = 0;                      // enabled control points considered
    size_t skipped = 0;                       // controls dropped for a nearer one at the same place
    std::string Summary () const;             // the sentence the editor shows
};

bool GradePathsToControls (geomsrv::edit::Document& doc, const std::vector<geomsrv::edit::Id>& paths, double radius,
                           GradeToControlsReport& report, std::string& error);

} // namespace tapioca::geometry
#endif
