#include "Geometry/BvhBuild.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace geomsrv {

namespace {

constexpr size_t kLeafTriangles = 4;     // never split below this
constexpr size_t kMaxLeafTriangles = 16; // a cheaper-as-leaf node above this still splits
constexpr size_t kBins = 32;
// Relative SAH costs: one box test against one triangle test.
constexpr double kTraversalCost = 1.0;
constexpr double kTriangleCost = 1.0;

struct Box {
    double lo[3] = { std::numeric_limits<double>::max (), std::numeric_limits<double>::max (),
                     std::numeric_limits<double>::max () };
    double hi[3] = { -std::numeric_limits<double>::max (), -std::numeric_limits<double>::max (),
                     -std::numeric_limits<double>::max () };

    // std::min / std::max in nanort's argument order, so a NaN coordinate
    // is ignored the way nanort's own box computation ignores it.
    void Grow (const double* lower, const double* upper)
    {
        for (int k = 0; k < 3; ++k) {
            lo[k] = (std::min) (lo[k], lower[k]);
            hi[k] = (std::max) (hi[k], upper[k]);
        }
    }
    void Grow (const Box& other)
    {
        Grow (other.lo, other.hi);
    }
    double HalfArea () const
    {
        if (hi[0] < lo[0] || hi[1] < lo[1] || hi[2] < lo[2])
            return 0.0;
        const double x = hi[0] - lo[0], y = hi[1] - lo[1], z = hi[2] - lo[2];
        return x * y + y * z + z * x;
    }
};

class Builder {
  public:
    Builder (const double* vertices, const uint32_t* faces, size_t triangles)
        : boxes_ (triangles), centroids_ (triangles * 3)
    {
        result.indices.resize (triangles);
        for (size_t t = 0; t < triangles; ++t) {
            result.indices[t] = static_cast<uint32_t> (t);
            const double* a = &vertices[3 * size_t (faces[3 * t])];
            const double* b = &vertices[3 * size_t (faces[3 * t + 1])];
            const double* c = &vertices[3 * size_t (faces[3 * t + 2])];
            Box& box = boxes_[t];
            box.Grow (a, a);
            box.Grow (b, b);
            box.Grow (c, c);
            for (int k = 0; k < 3; ++k) {
                const double centre = 0.5 * (box.lo[k] + box.hi[k]);
                // A non-finite triangle still goes in a leaf; it just bins at 0.
                centroids_[3 * t + k] = std::isfinite (centre) ? centre : 0.0;
            }
        }
        result.nodes.reserve (triangles > 0 ? 2 * triangles / kLeafTriangles + 1 : 0);
    }

    // Pre-order: returns the node's index, then its left subtree follows it.
    uint32_t Build (size_t first, size_t last, size_t depth)
    {
        result.depth = (std::max) (result.depth, depth);
        const uint32_t index = static_cast<uint32_t> (result.nodes.size ());
        result.nodes.push_back ({});
        Box bounds, centres;
        for (size_t i = first; i < last; ++i) {
            const uint32_t t = result.indices[i];
            bounds.Grow (boxes_[t]);
            centres.Grow (&centroids_[3 * t], &centroids_[3 * t]);
        }
        const size_t count = last - first;
        size_t middle = first;
        int axis = 0;
        if (count <= kLeafTriangles || depth >= kBvhMaxDepth || !Split (first, last, bounds, centres, middle, axis)) {
            Leaf (index, bounds, first, count);
            return index;
        }
        const uint32_t left = Build (first, middle, depth + 1);
        const uint32_t right = Build (middle, last, depth + 1);
        BvhBuildNode& node = result.nodes[index];
        SetBox (node, bounds);
        node.flag = 0;
        node.axis = axis;
        node.data[0] = left;
        node.data[1] = right;
        return index;
    }

    BvhBuildResult result;

  private:
    struct Bin {
        Box box;
        size_t count = 0;
    };

    static void SetBox (BvhBuildNode& node, const Box& box)
    {
        for (int k = 0; k < 3; ++k) {
            node.bmin[k] = box.lo[k];
            node.bmax[k] = box.hi[k];
        }
    }

    void Leaf (uint32_t index, const Box& bounds, size_t first, size_t count)
    {
        BvhBuildNode& node = result.nodes[index];
        SetBox (node, bounds);
        node.flag = 1;
        node.axis = 0;
        node.data[0] = static_cast<uint32_t> (count);
        node.data[1] = static_cast<uint32_t> (first);
    }

    size_t BinOf (uint32_t t, int axis, const Box& centres, double scale) const
    {
        const double offset = (centroids_[3 * t + size_t (axis)] - centres.lo[axis]) * scale;
        return offset <= 0.0 ? 0 : (std::min) (kBins - 1, size_t (offset));
    }

    // False when the node is cheaper, and small enough, as a leaf.
    bool Split (size_t first, size_t last, const Box& bounds, const Box& centres, size_t& middle, int& axis)
    {
        const size_t count = last - first;
        double bestCost = std::numeric_limits<double>::max ();
        size_t bestBin = 0;
        int bestAxis = -1;
        for (int k = 0; k < 3; ++k) {
            const double extent = centres.hi[k] - centres.lo[k];
            if (!(extent > 0.0) || !std::isfinite (extent))
                continue;
            const double scale = double (kBins) / extent;
            std::array<Bin, kBins> bins {};
            for (size_t i = first; i < last; ++i) {
                const uint32_t t = result.indices[i];
                Bin& bin = bins[BinOf (t, k, centres, scale)];
                bin.box.Grow (boxes_[t]);
                ++bin.count;
            }
            // Right-hand sweep, then the left-hand one evaluates each plane.
            std::array<double, kBins> rightArea {};
            std::array<size_t, kBins> rightCount {};
            Box right;
            size_t inRight = 0;
            for (size_t b = kBins - 1; b > 0; --b) {
                right.Grow (bins[b].box);
                inRight += bins[b].count;
                rightArea[b] = right.HalfArea ();
                rightCount[b] = inRight;
            }
            Box left;
            size_t inLeft = 0;
            for (size_t b = 1; b < kBins; ++b) {
                left.Grow (bins[b - 1].box);
                inLeft += bins[b - 1].count;
                if (inLeft == 0 || rightCount[b] == 0)
                    continue;
                const double cost = double (inLeft) * left.HalfArea () + double (rightCount[b]) * rightArea[b];
                if (cost < bestCost) {
                    bestCost = cost;
                    bestBin = b;
                    bestAxis = k;
                }
            }
        }
        const double area = bounds.HalfArea ();
        if (bestAxis >= 0) {
            const double splitCost = kTraversalCost + kTriangleCost * (area > 0.0 ? bestCost / area : 0.0);
            if (splitCost >= kTriangleCost * double (count) && count <= kMaxLeafTriangles)
                return false;
            const double scale = double (kBins) / (centres.hi[bestAxis] - centres.lo[bestAxis]);
            uint32_t* begin = &result.indices[first];
            uint32_t* split = std::partition (
                begin, begin + count, [&] (uint32_t t) { return BinOf (t, bestAxis, centres, scale) < bestBin; });
            middle = first + size_t (split - begin);
            axis = bestAxis;
            if (middle != first && middle != last)
                return true;
        }
        // Coincident centroids (stacked duplicates) give SAH nothing to cut;
        // halve by index along the longest axis so no leaf grows unbounded.
        if (count <= kMaxLeafTriangles)
            return false;
        axis = 0;
        for (int k = 1; k < 3; ++k)
            if (centres.hi[k] - centres.lo[k] > centres.hi[axis] - centres.lo[axis])
                axis = k;
        middle = first + count / 2;
        uint32_t* begin = &result.indices[first];
        std::nth_element (begin, &result.indices[middle], begin + count, [&] (uint32_t a, uint32_t b) {
            return centroids_[3 * a + size_t (axis)] < centroids_[3 * b + size_t (axis)];
        });
        return true;
    }

    std::vector<Box> boxes_;
    std::vector<double> centroids_;
};

} // namespace

BvhBuildResult BuildBinnedSahBvh (const double* vertices, const uint32_t* faces, size_t triangles)
{
    if (triangles == 0 || vertices == nullptr || faces == nullptr)
        return {};
    Builder builder (vertices, faces, triangles);
    builder.Build (0, triangles, 1);
    return std::move (builder.result);
}

} // namespace geomsrv
