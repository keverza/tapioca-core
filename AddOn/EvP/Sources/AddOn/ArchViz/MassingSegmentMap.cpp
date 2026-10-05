#include "ArchViz/MassingRules.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace geomsrv::archviz::massingrules {
std::vector<int> SegmentMap (const std::vector<Edge>& before, const std::vector<Edge>& after)
{
    std::vector<int> mapping (after.size (), -1);
    if (!ValidEdges (before) || !ValidEdges (after))
        return mapping;
    std::map<std::string, std::vector<int>> old, current;
    for (size_t i = 0; i < before.size (); ++i)
        old[Fingerprint (before[i])].push_back (int (i));
    for (size_t i = 0; i < after.size (); ++i)
        current[Fingerprint (after[i])].push_back (int (i));
    for (size_t i = 0; i < after.size (); ++i) {
        const auto key = Fingerprint (after[i]);
        if (old[key].size () == 1 && current[key].size () == 1)
            mapping[i] = old[key][0];
    }
    if (before.size () == after.size ()) {
        double best = 1e100;
        std::vector<int> correspondence;
        const int count = int (before.size ());
        for (int reverse : { 0, 1 })
            for (int shift = 0; shift < count; ++shift) {
                double score = 0;
                std::vector<int> candidate;
                for (int i = 0; i < count; ++i) {
                    const int index = (shift + (reverse ? count - i : i)) % count;
                    if (mapping[size_t (i)] >= 0 && mapping[size_t (i)] != index) {
                        score = 1e100;
                        break;
                    }
                    const auto& a = after[size_t (i)];
                    const auto& b = before[size_t (index)];
                    score += std::hypot (a.ax - (reverse ? b.bx : b.ax), a.ay - (reverse ? b.by : b.ay));
                    score += std::hypot (a.bx - (reverse ? b.ax : b.bx), a.by - (reverse ? b.ay : b.by));
                    candidate.push_back (index);
                }
                if (score < best) {
                    best = score;
                    correspondence = std::move (candidate);
                }
            }
        if (correspondence.size () == after.size ())
            return correspondence;
    }
    // Inserting a point on an old straight segment gives both children its offset.
    for (size_t i = 0; i < after.size (); ++i) {
        if (mapping[i] >= 0 || std::abs (after[i].arcAngle) > 1e-8)
            continue;
        int match = -1;
        for (size_t j = 0; j < before.size (); ++j) {
            const auto& edge = before[j];
            if (std::abs (edge.arcAngle) > 1e-8)
                continue;
            const double dx = edge.bx - edge.ax, dy = edge.by - edge.ay, length = std::hypot (dx, dy);
            const auto on = [&] (double x, double y) {
                const double along = ((x - edge.ax) * dx + (y - edge.ay) * dy) / length;
                return std::abs ((x - edge.ax) * dy - (y - edge.ay) * dx) / length < 1e-6 && along >= -1e-6 &&
                       along <= length + 1e-6;
            };
            if (on (after[i].ax, after[i].ay) && on (after[i].bx, after[i].by)) {
                if (match >= 0) {
                    match = -1;
                    break;
                }
                match = int (j);
            }
        }
        mapping[i] = match;
    }
    // A newly drawn/merged edge inherits the closest previous segment rather
    // than resetting every offset. Exact and split matches always take priority.
    for (size_t i = 0; i < after.size (); ++i) {
        if (mapping[i] >= 0)
            continue;
        double best = 1e100;
        for (size_t j = 0; j < before.size (); ++j) {
            const auto& a = after[i];
            const auto& b = before[j];
            const double direct = std::hypot (a.ax - b.ax, a.ay - b.ay) + std::hypot (a.bx - b.bx, a.by - b.by);
            const double reverse = std::hypot (a.ax - b.bx, a.ay - b.by) + std::hypot (a.bx - b.ax, a.by - b.ay);
            const double score = (std::min) (direct, reverse);
            if (score < best) {
                best = score;
                mapping[i] = int (j);
            }
        }
    }
    return mapping;
}
} // namespace geomsrv::archviz::massingrules
