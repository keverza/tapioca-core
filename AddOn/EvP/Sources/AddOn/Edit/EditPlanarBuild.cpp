#include "Edit/EditPlanar.hpp"
#include "Edit/EditPlanarInternal.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace tapioca::geometry::detail {

namespace {

constexpr double kParallelRelative = 1.0e-12;
constexpr double kTangentAngle = 0.12; // Radians, about seven degrees.
constexpr std::size_t kMaxGridCellsPerSegment = 256;

struct PointRef {
    Vec3 p;
    Id source = kNoId;
    uint32_t piece = 0;
};

struct Bounds {
    double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;
};

double Dot2 (Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y;
}

double Cross2 (Vec3 a, Vec3 b)
{
    return a.x * b.y - a.y * b.x;
}

double Length2 (Vec3 a)
{
    return std::hypot (a.x, a.y);
}

// Topology is welded only at numerical roundoff scale. Descriptor lattice
// spacing (snapTolerance) must not decide connectivity: a coarse key grid can
// then collapse two distinct faces into one descriptor and report the required
// ambiguity instead of merging them.
double TopologyWeld (const std::vector<PieceSegment>& segments)
{
    double scale = 0.0;
    for (const auto& segment : segments) {
        scale = std::max (
            { scale, std::abs (segment.a.x), std::abs (segment.a.y), std::abs (segment.b.x), std::abs (segment.b.y) });
    }
    return std::max (1.0e-9, 64.0 * std::numeric_limits<double>::epsilon () * scale);
}

bool CellCoord (double value, double origin, double cell, int64_t& cellValue)
{
    const long double quotient = std::floor ((static_cast<long double> (value) - static_cast<long double> (origin)) /
                                             static_cast<long double> (cell));
    if (!std::isfinite (quotient))
        return false;
    // 2^63 is exactly representable even where long double is double; using
    // INT64_MAX as a long double would round up to 2^63 and admit one bad value.
    constexpr long double kInt64Limit = 9223372036854775808.0L;
    if (quotient < -kInt64Limit || quotient >= kInt64Limit)
        return false;
    cellValue = static_cast<int64_t> (quotient);
    return true;
}

class UnionFind {
  public:
    explicit UnionFind (std::size_t count) : mParent (count)
    {
        std::iota (mParent.begin (), mParent.end (), uint32_t { 0 });
    }

    uint32_t Find (uint32_t index)
    {
        uint32_t root = index;
        while (mParent[root] != root)
            root = mParent[root];
        while (mParent[index] != index) {
            const uint32_t next = mParent[index];
            mParent[index] = root;
            index = next;
        }
        return root;
    }

    // The lower index is the representative, so a cluster's canonical point is
    // independent of the order the input curves arrived in.
    uint32_t Unite (uint32_t a, uint32_t b)
    {
        a = Find (a);
        b = Find (b);
        if (a == b)
            return a;
        if (a > b)
            std::swap (a, b);
        mParent[b] = a;
        return a;
    }

  private:
    std::vector<uint32_t> mParent;
};

void AddDiagnostic (PlanarMap& out, PlanarDiagnosticCode code, Id sourceA, Id sourceB, Vec3 at, double amount,
                    std::string message)
{
    out.diagnostics.push_back ({ code, sourceA, sourceB, at, amount, std::move (message) });
}

bool TooManySegments (PlanarMap& out, const PlanarOptions& options, std::string& error)
{
    AddDiagnostic (out, PlanarDiagnosticCode::TooManySegments, kNoId, kNoId, {},
                   static_cast<double> (options.maxSegments), "the arrangement exceeds maxSegments");
    error = "the arrangement exceeds maxSegments (" + std::to_string (options.maxSegments) + ")";
    return false;
}

Bounds SegmentBounds (const PieceSegment& segment)
{
    return { std::min (segment.a.x, segment.b.x), std::min (segment.a.y, segment.b.y),
             std::max (segment.a.x, segment.b.x), std::max (segment.a.y, segment.b.y) };
}

bool BuildIntersectionSplits (const std::vector<PieceSegment>& segments, const PlanarOptions& options, double weld,
                              PlanarMap& out, std::string& error, std::vector<std::vector<double>>& splits)
{
    splits.assign (segments.size (), {});
    for (auto& list : splits)
        list = { 0.0, 1.0 };
    std::size_t splitEvents = 0;
    const auto splitTolerance = [&] (std::size_t segment) {
        const auto& s = segments[segment];
        return std::min (0.5, weld / std::hypot (s.b.x - s.a.x, s.b.y - s.a.y));
    };
    const auto addSplit = [&] (std::size_t segment, double t) {
        if (splitEvents >= options.maxSplitEvents) {
            error = "split-event budget exceeded (" + std::to_string (options.maxSplitEvents) + ")";
            AddDiagnostic (out, PlanarDiagnosticCode::WorkLimit, kNoId, kNoId, segments[segment].a,
                           static_cast<double> (options.maxSplitEvents), error);
            return false;
        }
        ++splitEvents;
        const double tolerance = splitTolerance (segment);
        if (t <= tolerance)
            splits[segment].push_back (0.0);
        else if (t >= 1.0 - tolerance)
            splits[segment].push_back (1.0);
        else
            splits[segment].push_back (t);
        return true;
    };

    Bounds all { std::numeric_limits<double>::infinity (), std::numeric_limits<double>::infinity (),
                 -std::numeric_limits<double>::infinity (), -std::numeric_limits<double>::infinity () };
    std::vector<Bounds> bounds (segments.size ());
    const double coordinateLimit = std::numeric_limits<double>::max ();
    for (std::size_t i = 0; i < segments.size (); ++i) {
        bounds[i] = SegmentBounds (segments[i]);
        // The grid must see every pair that the narrow phase can weld. Expand
        // insertion/query bounds by the same numerical tolerance, saturating at
        // the finite double range so a cell coordinate never becomes inf/NaN.
        bounds[i].minX = std::max (-coordinateLimit, bounds[i].minX - weld);
        bounds[i].minY = std::max (-coordinateLimit, bounds[i].minY - weld);
        bounds[i].maxX = std::min (coordinateLimit, bounds[i].maxX + weld);
        bounds[i].maxY = std::min (coordinateLimit, bounds[i].maxY + weld);
        all.minX = std::min (all.minX, bounds[i].minX);
        all.minY = std::min (all.minY, bounds[i].minY);
        all.maxX = std::max (all.maxX, bounds[i].maxX);
        all.maxY = std::max (all.maxY, bounds[i].maxY);
    }
    // The unscaled predicates below multiply coordinate differences. Refuse an
    // unsupported range before any inf/NaN can silently drop an intersection.
    // The factor four leaves headroom for sums of two products and weld bounds.
    const double predicateSpan = std::sqrt (std::numeric_limits<double>::max ()) * 0.25;
    if (all.maxX - all.minX > predicateSpan || all.maxY - all.minY > predicateSpan) {
        error = "coordinate range exceeds finite planar intersection arithmetic";
        AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, kNoId, kNoId, {}, 0.0, error);
        return false;
    }
    const double diagonal = std::hypot (all.maxX - all.minX, all.maxY - all.minY);
    const double cell =
        std::max ({ weld * 4.0, diagonal / std::sqrt (static_cast<double> (segments.size ())), 1.0e-9 });

    using Cell = std::pair<int64_t, int64_t>;
    std::map<Cell, std::vector<uint32_t>> grid;
    std::vector<uint32_t> large;
    for (std::size_t i = 0; i < segments.size (); ++i) {
        int64_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
        if (!CellCoord (bounds[i].minX, all.minX, cell, x0) || !CellCoord (bounds[i].maxX, all.minX, cell, x1) ||
            !CellCoord (bounds[i].minY, all.minY, cell, y0) || !CellCoord (bounds[i].maxY, all.minY, cell, y1)) {
            error = "coordinate range exceeds the planar broadphase grid";
            AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, segments[i].source, kNoId, segments[i].a, 0.0, error);
            return false;
        }
        const long double countX = static_cast<long double> (x1) - static_cast<long double> (x0) + 1.0L;
        const long double countY = static_cast<long double> (y1) - static_cast<long double> (y0) + 1.0L;
        if (countX * countY > static_cast<long double> (kMaxGridCellsPerSegment)) {
            large.push_back (static_cast<uint32_t> (i));
            continue;
        }
        for (int64_t x = x0;; ++x) {
            for (int64_t y = y0;; ++y) {
                grid[{ x, y }].push_back (static_cast<uint32_t> (i));
                if (y == y1)
                    break;
            }
            if (x == x1)
                break;
        }
    }

    std::set<std::pair<uint32_t, uint32_t>> pairs;
    const auto addPair = [&] (uint32_t a, uint32_t b) {
        const std::pair<uint32_t, uint32_t> key { std::min (a, b), std::max (a, b) };
        if (pairs.find (key) != pairs.end ())
            return true;
        if (pairs.size () >= options.maxCandidatePairs) {
            error = "candidate-pair budget exceeded (" + std::to_string (options.maxCandidatePairs) + ")";
            AddDiagnostic (out, PlanarDiagnosticCode::WorkLimit, kNoId, kNoId, segments[a].a,
                           static_cast<double> (options.maxCandidatePairs), error);
            return false;
        }
        pairs.insert (key);
        return true;
    };
    for (const auto& [key, list] : grid) {
        (void) key;
        for (std::size_t i = 0; i < list.size (); ++i)
            for (std::size_t j = i + 1; j < list.size (); ++j)
                if (!addPair (list[i], list[j]))
                    return false;
    }
    const uint32_t segmentCount = static_cast<uint32_t> (segments.size ());
    for (uint32_t index : large)
        for (uint32_t other = 0; other < segmentCount; ++other)
            if (index != other && !addPair (index, other))
                return false;

    const double collinearTolerance = weld;
    const double nearCoincidentTolerance = weld * 256.0;
    for (const auto& [ia, ib] : pairs) {
        const PieceSegment& a = segments[ia];
        const PieceSegment& b = segments[ib];
        const Vec3 da { a.b.x - a.a.x, a.b.y - a.a.y, 0.0 };
        const Vec3 db { b.b.x - b.a.x, b.b.y - b.a.y, 0.0 };
        const double lengthA2 = Dot2 (da, da);
        const double lengthB2 = Dot2 (db, db);
        if (!(lengthA2 > 0.0) || !(lengthB2 > 0.0))
            continue;
        const double lengthA = std::sqrt (lengthA2);
        const double lengthB = std::sqrt (lengthB2);
        const double toleranceA = splitTolerance (ia);
        const double toleranceB = splitTolerance (ib);
        const double cross = Cross2 (da, db);
        const bool parallel = std::abs (cross) <= kParallelRelative * lengthA * lengthB;

        if (parallel) {
            const double distanceA = std::abs (Cross2 (da, Vec3 { b.a.x - a.a.x, b.a.y - a.a.y, 0.0 })) / lengthA;
            const double distanceB = std::abs (Cross2 (da, Vec3 { b.b.x - a.a.x, b.b.y - a.a.y, 0.0 })) / lengthA;
            const double distance = std::max (distanceA, distanceB);
            if (distance > collinearTolerance && distance <= nearCoincidentTolerance && a.source != b.source) {
                const double ta0 = Dot2 (Vec3 { b.a.x - a.a.x, b.a.y - a.a.y, 0.0 }, da) / lengthA2;
                const double ta1 = Dot2 (Vec3 { b.b.x - a.a.x, b.b.y - a.a.y, 0.0 }, da) / lengthA2;
                const double low = std::max (0.0, std::min (ta0, ta1));
                const double high = std::min (1.0, std::max (ta0, ta1));
                if (high >= low - toleranceA) {
                    const Vec3 at { a.a.x + da.x * (low + high) * 0.5, a.a.y + da.y * (low + high) * 0.5, 0.0 };
                    AddDiagnostic (out, PlanarDiagnosticCode::NearCoincident, a.source, b.source, at, distance,
                                   "near-coincident parallel edges");
                }
            }
            if (distance <= collinearTolerance) {
                const double ta0 = Dot2 (Vec3 { b.a.x - a.a.x, b.a.y - a.a.y, 0.0 }, da) / lengthA2;
                const double ta1 = Dot2 (Vec3 { b.b.x - a.a.x, b.b.y - a.a.y, 0.0 }, da) / lengthA2;
                const double low = std::max (0.0, std::min (ta0, ta1));
                const double high = std::min (1.0, std::max (ta0, ta1));
                if (high >= low - toleranceA) {
                    if (!addSplit (ia, std::clamp (low, 0.0, 1.0)) || !addSplit (ia, std::clamp (high, 0.0, 1.0)))
                        return false;
                    for (double t : { 0.0, 1.0 }) {
                        const Vec3 p { a.a.x + da.x * t, a.a.y + da.y * t, 0.0 };
                        const double tb = Dot2 (Vec3 { p.x - b.a.x, p.y - b.a.y, 0.0 }, db) / lengthB2;
                        if (tb >= -toleranceB && tb <= 1.0 + toleranceB && !addSplit (ib, std::clamp (tb, 0.0, 1.0)))
                            return false;
                    }
                }
            }
            // An endpoint that lies exactly on an almost-parallel edge is still
            // a T-junction. Only a collinear projection is used here so two
            // merely near-coincident parallel edges stay distinct.
            for (Vec3 q : { b.a, b.b }) {
                const double side = std::abs (Cross2 (da, Vec3 { q.x - a.a.x, q.y - a.a.y, 0.0 })) / lengthA;
                if (side > weld)
                    continue;
                const double t = Dot2 (Vec3 { q.x - a.a.x, q.y - a.a.y, 0.0 }, da) / lengthA2;
                if (t >= -toleranceA && t <= 1.0 + toleranceA && !addSplit (ia, std::clamp (t, 0.0, 1.0)))
                    return false;
            }
            for (Vec3 q : { a.a, a.b }) {
                const double side = std::abs (Cross2 (db, Vec3 { q.x - b.a.x, q.y - b.a.y, 0.0 })) / lengthB;
                if (side > weld)
                    continue;
                const double t = Dot2 (Vec3 { q.x - b.a.x, q.y - b.a.y, 0.0 }, db) / lengthB2;
                if (t >= -toleranceB && t <= 1.0 + toleranceB && !addSplit (ib, std::clamp (t, 0.0, 1.0)))
                    return false;
            }
            continue;
        }

        const Vec3 offset { b.a.x - a.a.x, b.a.y - a.a.y, 0.0 };
        const double ta = Cross2 (offset, db) / cross;
        const double tb = Cross2 (offset, da) / cross;
        if (ta >= -toleranceA && ta <= 1.0 + toleranceA && tb >= -toleranceB && tb <= 1.0 + toleranceB) {
            const double ca = std::clamp (ta, 0.0, 1.0);
            const double cb = std::clamp (tb, 0.0, 1.0);
            if (!addSplit (ia, ca) || !addSplit (ib, cb))
                return false;
            if (a.source == b.source && ta > 1.0e-9 && ta < 1.0 - 1.0e-9 && tb > 1.0e-9 && tb < 1.0 - 1.0e-9) {
                const Vec3 at { a.a.x + da.x * ta, a.a.y + da.y * ta, 0.0 };
                AddDiagnostic (out, PlanarDiagnosticCode::SelfIntersection, a.source, a.source, at, 0.0,
                               "a source curve crosses itself");
            }
        }

        // A non-parallel endpoint within numerical weld distance of the other
        // edge makes a T-junction. The descriptor lattice is deliberately not
        // used here: a coarse key grid must not create topology.
        const auto projectEndpoint = [&] (std::size_t sourceIndex, std::size_t targetIndex) {
            const PieceSegment& source = segments[sourceIndex];
            const PieceSegment& target = segments[targetIndex];
            const Vec3 ds { source.b.x - source.a.x, source.b.y - source.a.y, 0.0 };
            const double ls2 = Dot2 (ds, ds);
            if (!(ls2 > 0.0))
                return true;
            for (Vec3 q : { target.a, target.b }) {
                const double t = Dot2 (Vec3 { q.x - source.a.x, q.y - source.a.y, 0.0 }, ds) / ls2;
                const double tolerance = splitTolerance (sourceIndex);
                if (t < -tolerance || t > 1.0 + tolerance)
                    continue;
                const double distance =
                    std::abs (Cross2 (ds, Vec3 { q.x - source.a.x, q.y - source.a.y, 0.0 })) / std::sqrt (ls2);
                if (distance <= weld && !addSplit (sourceIndex, std::clamp (t, 0.0, 1.0)))
                    return false;
            }
            return true;
        };
        if (!projectEndpoint (ia, ib) || !projectEndpoint (ib, ia))
            return false;
    }

    for (std::size_t i = 0; i < splits.size (); ++i) {
        auto& list = splits[i];
        const double tolerance = splitTolerance (i);
        std::sort (list.begin (), list.end ());
        std::vector<double> unique;
        for (double t : list) {
            if (unique.empty () || t - unique.back () > tolerance)
                unique.push_back (std::clamp (t, 0.0, 1.0));
        }
        if (unique.size () < 2 || unique.front () != 0.0 || unique.back () != 1.0) {
            error = "internal planar split normalization failed";
            AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, kNoId, kNoId, {}, 0.0, error);
            return false;
        }
        list = std::move (unique);
    }
    return true;
}

bool BuildSegmentsAfterSplit (const std::vector<PieceSegment>& segments, const std::vector<std::vector<double>>& splits,
                              const PlanarOptions& options, PlanarMap& out, std::string& error,
                              std::vector<PieceSegment>& pieces)
{
    std::size_t total = 0;
    for (const auto& list : splits) {
        const std::size_t count = list.size () - 1;
        if (total > options.maxSegments || count > options.maxSegments - total)
            return TooManySegments (out, options, error);
        total += count;
    }
    pieces.reserve (total);
    for (std::size_t i = 0; i < segments.size (); ++i) {
        const PieceSegment& segment = segments[i];
        const Vec3 direction { segment.b.x - segment.a.x, segment.b.y - segment.a.y, 0.0 };
        for (std::size_t k = 0; k + 1 < splits[i].size (); ++k) {
            const double t0 = splits[i][k];
            const double t1 = splits[i][k + 1];
            const Vec3 a { segment.a.x + direction.x * t0, segment.a.y + direction.y * t0, 0.0 };
            const Vec3 b { segment.a.x + direction.x * t1, segment.a.y + direction.y * t1, 0.0 };
            const double u0 = segment.u0 + (segment.u1 - segment.u0) * t0;
            const double u1 = segment.u0 + (segment.u1 - segment.u0) * t1;
            pieces.push_back ({ a, b, segment.source, segment.piece, u0, u1, segment.openStart && k == 0,
                                segment.openEnd && k + 1 == splits[i].size () - 1, segment.ordinal });
        }
    }
    return true;
}

struct CandidateEdge {
    uint32_t a = 0, b = 0;
    Id source = kNoId;
    uint32_t piece = 0;
    double u0 = 0.0, u1 = 1.0;
    bool reversed = false;
};

bool BuildVerticesAndEdges (const std::vector<PieceSegment>& pieces, std::size_t originalSegmentCount,
                            const PlanarOptions& options, double weld, PlanarMap& out, std::string& error)
{
    std::vector<bool> hadPiece (originalSegmentCount, false);
    std::vector<bool> surviving (originalSegmentCount, false);
    std::vector<Id> sourceOf (originalSegmentCount, kNoId);
    std::vector<Vec3> atOf (originalSegmentCount);
    std::vector<PointRef> refs;
    refs.reserve (pieces.size () * 2);
    std::vector<uint32_t> openEnds;
    for (const auto& piece : pieces) {
        if (piece.ordinal >= originalSegmentCount) {
            error = "internal planar segment ordinal is out of range";
            AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, piece.source, kNoId, piece.a, 0.0, error);
            return false;
        }
        hadPiece[piece.ordinal] = true;
        sourceOf[piece.ordinal] = piece.source;
        atOf[piece.ordinal] = piece.a;
        refs.push_back ({ piece.a, piece.source, piece.piece });
        if (piece.openStart)
            openEnds.push_back (static_cast<uint32_t> (refs.size () - 1));
        refs.push_back ({ piece.b, piece.source, piece.piece });
        if (piece.openEnd)
            openEnds.push_back (static_cast<uint32_t> (refs.size () - 1));
    }
    if (refs.empty ())
        return true;

    double originX = refs.front ().p.x;
    double originY = refs.front ().p.y;
    for (const auto& ref : refs) {
        originX = std::min (originX, ref.p.x);
        originY = std::min (originY, ref.p.y);
    }

    UnionFind unionFind (refs.size ());
    std::vector<uint32_t> order (refs.size ());
    std::iota (order.begin (), order.end (), uint32_t { 0 });
    std::sort (order.begin (), order.end (), [&] (uint32_t a, uint32_t b) {
        const Vec3 &pa = refs[a].p, &pb = refs[b].p;
        return std::tie (pa.x, pa.y, pa.z, a) < std::tie (pb.x, pb.y, pb.z, b);
    });

    using Cell = std::pair<int64_t, int64_t>;
    std::map<Cell, std::vector<uint32_t>> snapCells;
    for (uint32_t index : order) {
        const PointRef& point = refs[index];
        int64_t cx = 0, cy = 0;
        if (!CellCoord (point.p.x, originX, weld, cx) || !CellCoord (point.p.y, originY, weld, cy)) {
            error = "coordinate range exceeds the planar numerical weld grid";
            AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, point.source, kNoId, point.p, 0.0, error);
            return false;
        }
        for (int64_t dx = -1; dx <= 1; ++dx) {
            for (int64_t dy = -1; dy <= 1; ++dy) {
                const auto found = snapCells.find ({ cx + dx, cy + dy });
                if (found == snapCells.end ())
                    continue;
                for (uint32_t other : found->second) {
                    const double distance = std::hypot (point.p.x - refs[other].p.x, point.p.y - refs[other].p.y);
                    if (distance <= weld)
                        unionFind.Unite (index, other);
                }
            }
        }
        snapCells[{ cx, cy }].push_back (index);
    }

    if (options.gapTolerance > 0.0) {
        std::map<Cell, std::vector<uint32_t>> gapCells;
        for (std::size_t i = 0; i < refs.size (); ++i) {
            int64_t cx = 0, cy = 0;
            if (!CellCoord (refs[i].p.x, originX, options.gapTolerance, cx) ||
                !CellCoord (refs[i].p.y, originY, options.gapTolerance, cy)) {
                error = "coordinate range exceeds the planar gap grid";
                AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, refs[i].source, kNoId, refs[i].p, 0.0, error);
                return false;
            }
            gapCells[{ cx, cy }].push_back (static_cast<uint32_t> (i));
        }
        std::sort (openEnds.begin (), openEnds.end ());
        openEnds.erase (std::unique (openEnds.begin (), openEnds.end ()), openEnds.end ());
        for (uint32_t endpoint : openEnds) {
            int64_t cx = 0, cy = 0;
            if (!CellCoord (refs[endpoint].p.x, originX, options.gapTolerance, cx) ||
                !CellCoord (refs[endpoint].p.y, originY, options.gapTolerance, cy))
                continue;
            for (int64_t dx = -1; dx <= 1; ++dx) {
                for (int64_t dy = -1; dy <= 1; ++dy) {
                    const auto found = gapCells.find ({ cx + dx, cy + dy });
                    if (found == gapCells.end ())
                        continue;
                    for (uint32_t other : found->second) {
                        if (other == endpoint || unionFind.Find (endpoint) == unionFind.Find (other))
                            continue;
                        const double distance =
                            std::hypot (refs[endpoint].p.x - refs[other].p.x, refs[endpoint].p.y - refs[other].p.y);
                        if (distance > options.gapTolerance)
                            continue;
                        unionFind.Unite (endpoint, other);
                        const Vec3 at { (refs[endpoint].p.x + refs[other].p.x) * 0.5,
                                        (refs[endpoint].p.y + refs[other].p.y) * 0.5, 0.0 };
                        AddDiagnostic (out, PlanarDiagnosticCode::OpenGap, refs[endpoint].source, refs[other].source,
                                       at, distance, "open endpoint gap healed within gapTolerance");
                    }
                }
            }
        }
    }

    // A cluster's canonical position is its lexicographically smallest point,
    // not its lowest point-array index: gap healing unions in an order that can
    // depend on the supplied curve order, while the canonical map must not.
    std::map<uint32_t, uint32_t> representative;
    for (std::size_t i = 0; i < refs.size (); ++i) {
        const uint32_t root = unionFind.Find (static_cast<uint32_t> (i));
        const auto found = representative.find (root);
        if (found == representative.end ()) {
            representative[root] = static_cast<uint32_t> (i);
            continue;
        }
        const Vec3 &candidate = refs[i].p, &current = refs[found->second].p;
        if (std::tie (candidate.x, candidate.y, candidate.z) < std::tie (current.x, current.y, current.z))
            found->second = static_cast<uint32_t> (i);
    }
    std::vector<uint32_t> roots;
    roots.reserve (representative.size ());
    for (const auto& [root, point] : representative) {
        (void) point;
        roots.push_back (root);
    }
    std::sort (roots.begin (), roots.end (), [&] (uint32_t a, uint32_t b) {
        const Vec3 &pa = refs[representative[a]].p, &pb = refs[representative[b]].p;
        return std::tie (pa.x, pa.y, pa.z, a) < std::tie (pb.x, pb.y, pb.z, b);
    });
    std::map<uint32_t, uint32_t> vertexOfRoot;
    out.vertices.reserve (roots.size ());
    for (std::size_t i = 0; i < roots.size (); ++i) {
        vertexOfRoot[roots[i]] = static_cast<uint32_t> (i);
        const Vec3& point = refs[representative[roots[i]]].p;
        out.vertices.push_back ({ point.x, point.y, 0.0 });
    }
    std::vector<uint32_t> vertexOfRef (refs.size ());
    for (std::size_t i = 0; i < refs.size (); ++i)
        vertexOfRef[i] = vertexOfRoot[unionFind.Find (static_cast<uint32_t> (i))];

    std::vector<CandidateEdge> candidates;
    candidates.reserve (pieces.size ());
    for (std::size_t i = 0; i < pieces.size (); ++i) {
        const uint32_t va = vertexOfRef[i * 2];
        const uint32_t vb = vertexOfRef[i * 2 + 1];
        if (va == vb) {
            AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, pieces[i].source, kNoId, pieces[i].a, 0.0,
                           "segment collapses under the numerical weld");
            continue;
        }
        surviving[pieces[i].ordinal] = true;
        CandidateEdge candidate;
        candidate.a = std::min (va, vb);
        candidate.b = std::max (va, vb);
        candidate.source = pieces[i].source;
        candidate.piece = pieces[i].piece;
        candidate.u0 = pieces[i].u0;
        candidate.u1 = pieces[i].u1;
        candidate.reversed = va > vb;
        candidates.push_back (candidate);
    }
    for (std::size_t i = 0; i < originalSegmentCount; ++i) {
        if (!hadPiece[i] || surviving[i])
            continue;
        error = "a source segment collapses under the numerical weld";
        AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, sourceOf[i], kNoId, atOf[i], 0.0, error);
        return false;
    }
    std::sort (candidates.begin (), candidates.end (), [] (const CandidateEdge& x, const CandidateEdge& y) {
        return std::tie (x.a, x.b, x.source, x.piece, x.u0, x.u1, x.reversed) <
               std::tie (y.a, y.b, y.source, y.piece, y.u0, y.u1, y.reversed);
    });

    for (std::size_t first = 0; first < candidates.size ();) {
        std::size_t last = first + 1;
        while (last < candidates.size () && candidates[last].a == candidates[first].a &&
               candidates[last].b == candidates[first].b)
            ++last;
        PlanarEdge edge;
        edge.a = candidates[first].a;
        edge.b = candidates[first].b;
        std::set<Id> sources;
        for (std::size_t i = first; i < last; ++i) {
            edge.pieces.push_back ({ candidates[i].source, candidates[i].piece, candidates[i].u0, candidates[i].u1,
                                     candidates[i].reversed });
            sources.insert (candidates[i].source);
        }
        std::sort (edge.pieces.begin (), edge.pieces.end (), [] (const PlanarEdgePiece& x, const PlanarEdgePiece& y) {
            return std::tie (x.source, x.piece, x.sourceStart, x.sourceEnd, x.reversed) <
                   std::tie (y.source, y.piece, y.sourceStart, y.sourceEnd, y.reversed);
        });
        edge.pieces.erase (std::unique (edge.pieces.begin (), edge.pieces.end ()), edge.pieces.end ());
        edge.sources.assign (sources.begin (), sources.end ());
        edge.source = edge.pieces.front ().source;
        edge.sourceStart = edge.pieces.front ().sourceStart;
        edge.sourceEnd = edge.pieces.front ().sourceEnd;
        edge.reversed = edge.pieces.front ().reversed;
        out.edges.push_back (std::move (edge));
        first = last;
    }

    out.halfEdges.resize (out.edges.size () * 2);
    for (std::size_t i = 0; i < out.edges.size (); ++i) {
        out.halfEdges[i * 2] = { static_cast<uint32_t> (i), static_cast<uint32_t> (i * 2 + 1), 0, out.edges[i].a };
        out.halfEdges[i * 2 + 1] = { static_cast<uint32_t> (i), static_cast<uint32_t> (i * 2), 0, out.edges[i].b };
    }
    std::vector<std::vector<uint32_t>> outgoing (out.vertices.size ());
    for (std::size_t h = 0; h < out.halfEdges.size (); ++h)
        outgoing[out.halfEdges[h].origin].push_back (static_cast<uint32_t> (h));
    const auto targetOf = [&] (uint32_t halfEdge) { return out.halfEdges[out.halfEdges[halfEdge].twin].origin; };
    for (auto& list : outgoing) {
        std::sort (list.begin (), list.end (), [&] (uint32_t a, uint32_t b) {
            const uint32_t origin = out.halfEdges[a].origin;
            const uint32_t ta = targetOf (a), tb = targetOf (b);
            const double angleA =
                std::atan2 (out.vertices[ta].y - out.vertices[origin].y, out.vertices[ta].x - out.vertices[origin].x);
            const double angleB =
                std::atan2 (out.vertices[tb].y - out.vertices[origin].y, out.vertices[tb].x - out.vertices[origin].x);
            return std::tie (angleA, ta, a) < std::tie (angleB, tb, b);
        });
    }
    std::vector<uint32_t> position (out.halfEdges.size (), 0);
    for (const auto& list : outgoing)
        for (std::size_t i = 0; i < list.size (); ++i)
            position[list[i]] = static_cast<uint32_t> (i);
    for (std::size_t h = 0; h < out.halfEdges.size (); ++h) {
        const uint32_t twin = out.halfEdges[h].twin;
        const uint32_t vertex = out.halfEdges[twin].origin;
        const auto& list = outgoing[vertex];
        const uint32_t degree = static_cast<uint32_t> (list.size ());
        out.halfEdges[h].next = list[(position[twin] + degree - 1) % degree];
    }

    // Two curves that touch at a vertex with nearly the same outgoing direction
    // are tangent there. A crossing (including a shallow one) is reported at the
    // segment intersection instead.
    for (std::size_t vertex = 0; vertex < outgoing.size (); ++vertex) {
        const auto& list = outgoing[vertex];
        if (list.size () < 4)
            continue;
        for (std::size_t i = 0; i < list.size (); ++i) {
            for (std::size_t j = i + 1; j < list.size (); ++j) {
                const uint32_t ha = list[i], hb = list[j];
                const uint32_t ea = out.halfEdges[ha].edge, eb = out.halfEdges[hb].edge;
                if (ea == eb || out.edges[ea].source == out.edges[eb].source)
                    continue;
                const uint32_t ta = targetOf (ha), tb = targetOf (hb);
                const Vec3 da { out.vertices[ta].x - out.vertices[vertex].x,
                                out.vertices[ta].y - out.vertices[vertex].y, 0.0 };
                const Vec3 db { out.vertices[tb].x - out.vertices[vertex].x,
                                out.vertices[tb].y - out.vertices[vertex].y, 0.0 };
                const double la = Length2 (da), lb = Length2 (db);
                if (!(la > 0.0) || !(lb > 0.0))
                    continue;
                const double cosine = std::clamp (Dot2 (da, db) / (la * lb), -1.0, 1.0);
                const double angle = std::acos (cosine);
                if (angle < kTangentAngle) {
                    const Id a = std::min (out.edges[ea].source, out.edges[eb].source);
                    const Id b = std::max (out.edges[ea].source, out.edges[eb].source);
                    AddDiagnostic (out, PlanarDiagnosticCode::Tangent, a, b, out.vertices[vertex], angle,
                                   "curves touch tangentially at a vertex");
                }
            }
        }
    }
    return true;
}

} // namespace

bool BuildTopology (const std::vector<PlanarCurve>& curves, const PlanarOptions& options, PlanarMap& out,
                    std::string& error)
{
    std::vector<NormalizedCurve> normalized;
    if (!NormalizeCurves (curves, normalized, out, error))
        return false;

    std::vector<PieceSegment> segments;
    if (!FlattenCurves (normalized, options, segments, out, error))
        return false;
    if (segments.empty ())
        return true;

    std::vector<std::vector<double>> splits;
    const double weld = TopologyWeld (segments);
    if (!BuildIntersectionSplits (segments, options, weld, out, error, splits))
        return false;

    std::vector<PieceSegment> pieces;
    if (!BuildSegmentsAfterSplit (segments, splits, options, out, error, pieces))
        return false;
    return BuildVerticesAndEdges (pieces, segments.size (), options, weld, out, error);
}

} // namespace tapioca::geometry::detail
