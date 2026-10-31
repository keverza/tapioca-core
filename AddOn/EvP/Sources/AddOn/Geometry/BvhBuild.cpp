#include "Geometry/BvhBuild.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace geomsrv {

namespace {

constexpr size_t kLeafTriangles = 4;     // never split below this
constexpr size_t kMaxLeafTriangles = 16; // a cheaper-as-leaf node above this still splits
constexpr size_t kBins = 32;
// Relative SAH costs: one box test against one triangle test.
constexpr double kTraversalCost = 1.0;
constexpr double kTriangleCost = 1.0;
// A subtree is built on its own thread only above this size; below it a
// thread costs more than the subtree.
constexpr size_t kParallelTriangles = 16384;
// A node's own scans (bounds, bins) are split into chunks only above this.
constexpr size_t kParallelScanTriangles = 131072;

// The calling thread's priority, for the build's helpers: a below-normal study
// worker must not fan out into normal-priority threads (as RunCpuTraversal).
int CallerPriority ()
{
#ifdef _WIN32
    const int priority = ::GetThreadPriority (::GetCurrentThread ());
    return priority == THREAD_PRIORITY_ERROR_RETURN ? THREAD_PRIORITY_NORMAL : priority;
#else
    return 0;
#endif
}

void AdoptPriority (int priority)
{
#ifdef _WIN32
    ::SetThreadPriority (::GetCurrentThread (), priority);
#else
    (void) priority;
#endif
}

// Run body(chunk, begin, end) over [0, count) in up to `threads` contiguous
// chunks; `chunk` is below `threads`.
template <typename Body> void ForChunks (size_t count, size_t threads, int priority, const Body& body)
{
    threads = (std::max) (size_t (1), (std::min) (threads, count / kParallelTriangles + 1));
    std::vector<std::thread> helpers;
    const size_t chunk = (count + threads - 1) / threads;
    for (size_t c = 1; c < threads; ++c)
        helpers.emplace_back ([&body, c, chunk, count, priority] {
            AdoptPriority (priority);
            body (c, (std::min) (count, c * chunk), (std::min) (count, (c + 1) * chunk));
        });
    body (0, 0, (std::min) (count, chunk));
    for (auto& helper : helpers)
        helper.join ();
}

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
    Builder (const double* vertices, const uint32_t* faces, size_t triangles, size_t threads)
        : boxes_ (triangles), centroids_ (triangles * 3), threads_ (threads), priority_ (CallerPriority ()),
          idle_ (threads > 0 ? threads - 1 : 0)
    {
        result.indices.resize (triangles);
        ForChunks (triangles, threads_, priority_, [&] (size_t, size_t begin, size_t end) {
            for (size_t t = begin; t < end; ++t) {
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
        });
        result.nodes.reserve (triangles > 0 ? 2 * triangles / kLeafTriangles + 1 : 0);
    }

    // Pre-order into `out`: returns the node's index in `out`, and its left
    // subtree follows it. `deepest` is this thread's deepest leaf.
    //
    // ⚠️ THE PARALLEL TREE IS THE SEQUENTIAL TREE, NODE FOR NODE. A large node
    // whose right subtree a free helper takes builds its left subtree straight
    // into `out` and appends the helper's nodes after it, links offset. The
    // subtrees partition disjoint ranges of `indices`, so pre-order, and every
    // split, is what one thread would have produced
    // (BvhBuild.ParallelAndSequentialTreesAreIdentical).
    //
    // ⚠️ HELPERS ARE TAKEN WHEN FREE, NOT HANDED OUT BY LEVEL. SAH splits are
    // often lopsided (a ground slab against the rest), and a fixed per-level
    // budget left one thread with most of the tree: 2.4x on eight.
    uint32_t Build (std::vector<BvhBuildNode>& out, size_t first, size_t last, size_t depth, size_t& deepest)
    {
        deepest = (std::max) (deepest, depth);
        const uint32_t index = static_cast<uint32_t> (out.size ());
        out.push_back ({});
        Box bounds, centres;
        ScanBounds (first, last, bounds, centres);
        const size_t count = last - first;
        size_t middle = first;
        int axis = 0;
        if (count <= kLeafTriangles || depth >= kBvhMaxDepth || !Split (first, last, bounds, centres, middle, axis)) {
            Leaf (out[index], bounds, first, count);
            return index;
        }
        uint32_t left = 0, right = 0;
        if (last - middle < kParallelTriangles || !ClaimHelper ()) {
            left = Build (out, first, middle, depth + 1, deepest);
            right = Build (out, middle, last, depth + 1, deepest);
        }
        else {
            std::vector<BvhBuildNode> rightNodes;
            size_t rightDeepest = 0;
            std::thread helper ([&] {
                AdoptPriority (priority_);
                Build (rightNodes, middle, last, depth + 1, rightDeepest);
                idle_.fetch_add (1); // free for another node while this one joins
            });
            left = Build (out, first, middle, depth + 1, deepest);
            helper.join ();
            deepest = (std::max) (deepest, rightDeepest);
            right = Append (out, rightNodes);
        }
        BvhBuildNode& node = out[index];
        SetBox (node, bounds);
        node.flag = 0;
        node.axis = axis;
        node.data[0] = left;
        node.data[1] = right;
        return index;
    }

    BvhBuildResult result;

  private:
    // ⚠️ TRIVIALLY CONSTRUCTIBLE (raw bounds, not a Box with its initialisers),
    // AND ONLY THE BINS IN USE ARE RESET. Every
    // node bins three axes; zero-initialising 3 x 32 bins (~5 KB) for nodes of
    // five triangles was most of the single-thread build's time.
    struct Bin {
        double lo[3];
        double hi[3];
        size_t count;
        void Reset ()
        {
            const Box empty;
            for (int k = 0; k < 3; ++k) {
                lo[k] = empty.lo[k];
                hi[k] = empty.hi[k];
            }
            count = 0;
        }
        // Box::Grow's rule: a NaN coordinate never enters.
        void Grow (const double* lower, const double* upper)
        {
            for (int k = 0; k < 3; ++k) {
                lo[k] = (std::min) (lo[k], lower[k]);
                hi[k] = (std::max) (hi[k], upper[k]);
            }
        }
    };

    static void SetBox (BvhBuildNode& node, const Box& box)
    {
        for (int k = 0; k < 3; ++k) {
            node.bmin[k] = box.lo[k];
            node.bmax[k] = box.hi[k];
        }
    }

    // A subtree built in its own vector, appended with its child links offset.
    static uint32_t Append (std::vector<BvhBuildNode>& out, const std::vector<BvhBuildNode>& subtree)
    {
        const uint32_t base = static_cast<uint32_t> (out.size ());
        for (BvhBuildNode node : subtree) {
            if (node.flag == 0) {
                node.data[0] += base;
                node.data[1] += base;
            }
            out.push_back (node);
        }
        return base;
    }

    static void Leaf (BvhBuildNode& node, const Box& bounds, size_t first, size_t count)
    {
        SetBox (node, bounds);
        node.flag = 1;
        node.axis = 0;
        node.data[0] = static_cast<uint32_t> (count);
        node.data[1] = static_cast<uint32_t> (first);
    }

    // A node of n triangles uses min(kBins, n) bins: more planes than
    // triangles add nothing but work.
    static size_t BinsFor (size_t count)
    {
        return (std::min) (kBins, (std::max) (size_t (2), count));
    }

    size_t BinOf (uint32_t t, int axis, const Box& centres, double scale, size_t bins) const
    {
        const double offset = (centroids_[3 * t + size_t (axis)] - centres.lo[axis]) * scale;
        return offset <= 0.0 ? 0 : (std::min) (bins - 1, size_t (offset));
    }

    // ⚠️ THE TOP OF THE TREE IS ONE THREAD'S WORK UNLESS ITS SCANS ARE SPLIT: the
    // root alone reads every triangle five times. Large ranges are scanned in
    // chunks and merged. The merge is exact and order-free: counts add, and a
    // box only takes min/max of values that never hold a NaN (Box::Grow drops
    // them), so the tree is the one a single thread builds.
    bool ChunkedScan (size_t count) const
    {
        return threads_ > 1 && count >= kParallelScanTriangles;
    }

    void ScanBounds (size_t first, size_t last, Box& bounds, Box& centres) const
    {
        const auto scan = [this] (size_t begin, size_t end, Box& b, Box& c) {
            for (size_t i = begin; i < end; ++i) {
                const uint32_t t = result.indices[i];
                b.Grow (boxes_[t]);
                c.Grow (&centroids_[3 * t], &centroids_[3 * t]);
            }
        };
        if (!ChunkedScan (last - first)) {
            scan (first, last, bounds, centres);
            return;
        }
        std::vector<Box> partsB (threads_), partsC (threads_);
        ForChunks (last - first, threads_, priority_, [&] (size_t part, size_t begin, size_t end) {
            scan (first + begin, first + end, partsB[part], partsC[part]);
        });
        for (size_t p = 0; p < threads_; ++p) {
            bounds.Grow (partsB[p]);
            centres.Grow (partsC[p]);
        }
    }

    using AxisBins = std::array<std::array<Bin, kBins>, 3>;

    // All three axes' bins in ONE read of the node's boxes: the scan is
    // random access through `indices`, so reading each box once instead of
    // three times is most of the build's memory traffic. An axis whose
    // `scales` entry is 0 (no extent) is left empty.
    AxisBins FillBins (size_t first, size_t last, const Box& centres, const double scales[3], size_t used) const
    {
        const auto reset = [used] (AxisBins& bins) {
            for (auto& axisBins : bins)
                for (size_t b = 0; b < used; ++b)
                    axisBins[b].Reset ();
        };
        const auto fill = [&, this] (size_t begin, size_t end, AxisBins& bins) {
            for (size_t i = begin; i < end; ++i) {
                const uint32_t t = result.indices[i];
                const Box& box = boxes_[t];
                for (int k = 0; k < 3; ++k) {
                    if (scales[k] == 0.0)
                        continue;
                    Bin& bin = bins[size_t (k)][BinOf (t, k, centres, scales[k], used)];
                    bin.Grow (box.lo, box.hi);
                    ++bin.count;
                }
            }
        };
        AxisBins bins;
        reset (bins);
        if (!ChunkedScan (last - first)) {
            fill (first, last, bins);
            return bins;
        }
        std::vector<AxisBins> parts (threads_);
        for (auto& part : parts)
            reset (part);
        ForChunks (last - first, threads_, priority_,
                   [&] (size_t part, size_t begin, size_t end) { fill (first + begin, first + end, parts[part]); });
        for (const auto& part : parts)
            for (size_t k = 0; k < 3; ++k)
                for (size_t b = 0; b < used; ++b) {
                    bins[k][b].Grow (part[k][b].lo, part[k][b].hi);
                    bins[k][b].count += part[k][b].count;
                }
        return bins;
    }

    // False when the node is cheaper, and small enough, as a leaf.
    bool Split (size_t first, size_t last, const Box& bounds, const Box& centres, size_t& middle, int& axis)
    {
        const size_t count = last - first;
        double bestCost = std::numeric_limits<double>::max ();
        size_t bestBin = 0;
        int bestAxis = -1;
        const size_t used = BinsFor (count);
        double scales[3] = { 0.0, 0.0, 0.0 };
        for (int k = 0; k < 3; ++k) {
            const double extent = centres.hi[k] - centres.lo[k];
            if (extent > 0.0 && std::isfinite (extent))
                scales[k] = double (used) / extent;
        }
        const AxisBins all = FillBins (first, last, centres, scales, used);
        for (int k = 0; k < 3; ++k) {
            if (scales[k] == 0.0)
                continue;
            const std::array<Bin, kBins>& bins = all[size_t (k)];
            // Right-hand sweep, then the left-hand one evaluates each plane.
            std::array<double, kBins> rightArea;
            std::array<size_t, kBins> rightCount;
            Box right;
            size_t inRight = 0;
            for (size_t b = used - 1; b > 0; --b) {
                right.Grow (bins[b].lo, bins[b].hi);
                inRight += bins[b].count;
                rightArea[b] = right.HalfArea ();
                rightCount[b] = inRight;
            }
            Box left;
            size_t inLeft = 0;
            for (size_t b = 1; b < used; ++b) {
                left.Grow (bins[b - 1].lo, bins[b - 1].hi);
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
            const double scale = double (used) / (centres.hi[bestAxis] - centres.lo[bestAxis]);
            uint32_t* begin = &result.indices[first];
            uint32_t* split = std::partition (
                begin, begin + count, [&] (uint32_t t) { return BinOf (t, bestAxis, centres, scale, used) < bestBin; });
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
    bool ClaimHelper ()
    {
        size_t idle = idle_.load ();
        while (idle > 0)
            if (idle_.compare_exchange_weak (idle, idle - 1))
                return true;
        return false;
    }

    size_t threads_ = 1;
    int priority_ = 0;
    std::atomic<size_t> idle_ { 0 }; // helpers free to take a subtree
};

} // namespace

BvhBuildResult BuildBinnedSahBvh (const double* vertices, const uint32_t* faces, size_t triangles, size_t threads)
{
    if (triangles == 0 || vertices == nullptr || faces == nullptr)
        return {};
    if (threads == 0)
        threads = BvhBuildThreads ();
    Builder builder (vertices, faces, triangles, threads);
    size_t deepest = 0;
    builder.Build (builder.result.nodes, 0, triangles, 1, deepest);
    builder.result.depth = deepest;
    return std::move (builder.result);
}

size_t BvhBuildThreads ()
{
    return std::clamp<size_t> (std::thread::hardware_concurrency () / 2, 1, 8);
}

} // namespace geomsrv
