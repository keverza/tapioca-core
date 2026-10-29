#ifndef EVP_ARCHVIZ_ANALYSISPATH_HPP
#define EVP_ARCHVIZ_ANALYSISPATH_HPP

// ArchViz/AnalysisPath -- a polyline taken from Archicad as the path the shared
// analysis point travels along (Point visibility and Viewpoint), and the curve
// evaluated at a parameter: a point on it and the direction it runs there.
//
// Pure: no ACAPI, no ImGui. AnalysisPathSource reads the element.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::archviz::analysispath {

using Point3 = std::array<double, 3>;

struct Path {
    std::string guid;           // the Archicad element it was read from
    uint64_t stamp = 0;         // the element's modification stamp when it was read
    std::vector<Point3> points; // arcs tessellated; a closed path ends on its first point
    std::vector<double> along;  // cumulative length at each point, metres; along[0] == 0
    bool closed = false;

    double Length () const
    {
        return along.empty () ? 0.0 : along.back ();
    }
};

// From the outline's vertices -- x, y pairs, with one signed arc angle per vertex
// for the edge leaving it (radians, counter-clockwise positive, 0 straight) -- at
// height z. A closed outline gains the edge back to its first vertex. Fewer than
// two vertices, or no length, give an empty path.
Path Build (std::string guid, const std::vector<double>& xy, const std::vector<double>& arcs, bool closed, double z);

// The point at `t` of the way along the path's length (clamped to 0..1) and the
// unit tangent there, in the direction the path runs. False for an empty path.
bool Evaluate (const Path& path, double t, Point3& at, Point3& tangent);

} // namespace geomsrv::archviz::analysispath

#endif
