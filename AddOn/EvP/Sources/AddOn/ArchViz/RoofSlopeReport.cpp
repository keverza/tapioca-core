#include "ArchViz/RoofSlope.hpp"
#include <algorithm>
#include <set>

namespace geomsrv::archviz::roofslope {
Measures Measure (const Data& data)
{
    Measures out;
    for (const auto& c : data.cells) {
        const double w = c.box[2] - c.box[0], h = c.box[3] - c.box[1];
        out.maxAspect = (std::max) (out.maxAspect, (std::max) (w, h) / (std::min) (w, h));
        out.maxSpan = (std::max) (out.maxSpan, (std::max) (w, h));
        for (int n : c.rim)
            out.maxBorderRise = (std::max) (out.maxBorderRise, data.nodes[n].z - data.nodes[c.drain].z);
    }
    std::set<std::array<int, 3>> faces;
    for (const auto& t : data.triangles)
        faces.insert (t.nodes);
    out.faces = int (faces.size ());
    return out;
}
} // namespace geomsrv::archviz::roofslope
