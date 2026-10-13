#ifndef EVP_ARCHVIZ_OVERLAYHOVERINDEX_HPP
#define EVP_ARCHVIZ_OVERLAYHOVERINDEX_HPP

#include "ArchViz/OverlayHover.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace geomsrv::archviz::overlayhover::detail {

struct PickIndex {
    std::vector<float> sx, sy, iw;
    std::vector<uint8_t> seen;
    std::map<std::pair<int64_t, int64_t>, std::vector<uint32_t>> bins;
    std::vector<uint32_t> large;
    double binSize = 1;

    void Build (const overlaylayers::Mesh& mesh, const ProjectView* project)
    {
        binSize = project != nullptr ? 128 : 1;
        const size_t vertices = mesh.points.size () / 3;
        if (project != nullptr) {
            sx.resize (vertices);
            sy.resize (vertices);
            iw.resize (vertices);
            seen.resize (vertices);
            for (size_t k = 0; k < vertices; ++k)
                seen[k] = (*project) (mesh.points[k * 3], mesh.points[k * 3 + 1], mesh.points[k * 3 + 2], sx[k], sy[k],
                                      iw[k]);
        }
        for (size_t t = 0; t + 2 < mesh.indices.size (); t += 3) {
            double low[2] = { 1e300, 1e300 }, high[2] = { -1e300, -1e300 };
            bool valid = true;
            for (size_t c = 0; c < 3; ++c) {
                const uint32_t v = mesh.indices[t + c];
                if (v >= vertices || (project != nullptr && !seen[v])) {
                    valid = false;
                    break;
                }
                const double xy[2] = { project != nullptr ? sx[v] : mesh.points[v * 3],
                                       project != nullptr ? sy[v] : mesh.points[v * 3 + 1] };
                for (int axis = 0; axis < 2; ++axis) {
                    low[axis] = (std::min) (low[axis], xy[axis]);
                    high[axis] = (std::max) (high[axis], xy[axis]);
                }
            }
            if (!valid)
                continue;
            const uint32_t triangle = uint32_t (t / 3);
            if (!std::isfinite (low[0]) || !std::isfinite (low[1]) || !std::isfinite (high[0]) ||
                !std::isfinite (high[1]) || std::abs (low[0]) > 1e12 || std::abs (low[1]) > 1e12 ||
                std::abs (high[0]) > 1e12 || std::abs (high[1]) > 1e12 ||
                ((high[0] - low[0]) / binSize + 2) * ((high[1] - low[1]) / binSize + 2) > 4096) {
                large.push_back (triangle);
                continue;
            }
            for (int64_t y = int64_t (std::floor (low[1] / binSize)); y <= int64_t (std::floor (high[1] / binSize));
                 ++y)
                for (int64_t x = int64_t (std::floor (low[0] / binSize)); x <= int64_t (std::floor (high[0] / binSize));
                     ++x)
                    bins[{ x, y }].push_back (triangle);
        }
    }

    std::vector<uint32_t> At (double x, double y) const
    {
        auto candidates = large;
        if (std::isfinite (x) && std::isfinite (y) && std::abs (x) <= 1e12 && std::abs (y) <= 1e12) {
            const auto bin = bins.find ({ int64_t (std::floor (x / binSize)), int64_t (std::floor (y / binSize)) });
            if (bin != bins.end ())
                candidates.insert (candidates.end (), bin->second.begin (), bin->second.end ());
        }
        std::sort (candidates.begin (), candidates.end ());
        return candidates;
    }
};

// Immutable layer ownership and a caller-supplied camera key make cached projections
// safe across pointer moves; camera, layer replacement, and teardown invalidate them.
struct PickCache {
    uint64_t key = 0;
    std::vector<std::weak_ptr<const overlaylayers::Layer>> owners;
    std::vector<std::vector<PickIndex>> indices;
    void Prepare (const std::vector<std::shared_ptr<const overlaylayers::Layer>>& all, uint64_t next,
                  const ProjectView* project)
    {
        bool same = key == next && owners.size () == all.size ();
        for (size_t i = 0; same && i < all.size (); ++i)
            same = owners[i].lock () == all[i];
        if (same)
            return;
        key = next;
        owners.assign (all.begin (), all.end ());
        indices.clear ();
        indices.resize (all.size ());
        for (size_t li = 0; li < all.size (); ++li) {
            indices[li].resize (all[li]->meshes.size ());
            if (!overlaylayers::DrawnIn (all[li]->views, project != nullptr ? overlaylayers::Views::ThreeD
                                                                            : overlaylayers::Views::TwoD))
                continue;
            for (size_t mi = 0; mi < all[li]->meshes.size (); ++mi) {
                const auto& mesh = all[li]->meshes[mi];
                if (!mesh.values.empty () || !mesh.hoverCells.empty () || !mesh.hoverTitle.empty () ||
                    !mesh.hoverRows.empty ())
                    indices[li][mi].Build (mesh, project);
            }
        }
    }
};

} // namespace geomsrv::archviz::overlayhover::detail
#endif
