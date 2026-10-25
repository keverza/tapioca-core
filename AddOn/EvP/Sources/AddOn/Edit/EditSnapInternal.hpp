#ifndef EVP_EDIT_EDITSNAPINTERNAL_HPP
#define EVP_EDIT_EDITSNAPINTERNAL_HPP

// What EditSnap.cpp and EditSnapTargets.cpp share: the gathered snap targets, the working
// plane's frame and the index built over them. Not part of the kernel's interface.
#include "Edit/EditSnap.hpp"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace geomsrv::edit {
namespace snapgather {

struct P2 {
    double x = 0.0, y = 0.0;
};

struct Frame {
    Vec3 origin, u, v;
    P2 To (Vec3 p) const
    {
        const Vec3 d = p - origin;
        return { Dot (d, u), Dot (d, v) };
    }
};

struct Seg {
    Vec3 a, b;
    Ref ref;
    std::string context;
    int run = -1; // a piece of a drawn curve: the curve's number (unique in the gather), -1 straight
};

struct Pt {
    Vec3 p;
    SnapKind kind;
    Ref ref;
    std::string context;
};

// Every snap target of the document and the host context the query does not exclude.
void Gather (const Document& doc, const SnapContext& context, const SnapQuery& query, std::vector<Pt>& points,
             std::vector<Seg>& segments);

} // namespace snapgather

// The gathered targets, and a uniform grid over them in the working plane's frame: each cell
// lists the points in it and the segments whose bounds touch it, in gather order.
struct SnapTargets::Index {
    uint64_t contextKey = 0;
    std::vector<Entity> entities; // what it was gathered from, compared on every use
    std::vector<Path> paths;
    std::vector<Ref> exclude;
    double pathStep = 0.0;
    Plane plane;
    snapgather::Frame frame;
    std::vector<snapgather::Pt> points;
    std::vector<snapgather::Seg> segments;
    double x0 = 0.0, y0 = 0.0, cell = 1.0;
    int nx = 0, ny = 0;
    std::vector<std::vector<uint32_t>> pointCells, segmentCells;

    bool For (const Document& doc, const SnapQuery& q) const
    {
        return exclude == q.exclude && pathStep == q.pathStep && entities == doc.entities && paths == doc.paths &&
               plane.origin == q.plane.origin && plane.normal == q.plane.normal && plane.u == q.plane.u;
    }
    int Column (double x) const
    {
        return std::clamp (int (std::floor ((x - x0) / cell)), 0, nx - 1);
    }
    int Row (double y) const
    {
        return std::clamp (int (std::floor ((y - y0) / cell)), 0, ny - 1);
    }
    // Indices whose cells meet the square of half side `radius` about `c`, ascending: the
    // gather order, so every tie and crossing is met in the order a full scan meets it.
    void Near (const std::vector<std::vector<uint32_t>>& cells, snapgather::P2 c, double radius,
               std::vector<uint32_t>& out) const
    {
        out.clear ();
        if (nx == 0 || !(radius >= 0.0) || !std::isfinite (c.x + c.y + radius))
            return;
        const int cx0 = Column (c.x - radius), cx1 = Column (c.x + radius);
        const int cy0 = Row (c.y - radius), cy1 = Row (c.y + radius);
        for (int y = cy0; y <= cy1; ++y)
            for (int x = cx0; x <= cx1; ++x) {
                const auto& list = cells[size_t (y) * size_t (nx) + size_t (x)];
                out.insert (out.end (), list.begin (), list.end ());
            }
        std::sort (out.begin (), out.end ());
        out.erase (std::unique (out.begin (), out.end ()), out.end ());
    }
};

} // namespace geomsrv::edit
#endif
