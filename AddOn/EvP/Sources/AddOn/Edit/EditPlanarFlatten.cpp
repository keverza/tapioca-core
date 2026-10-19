#include "Edit/EditPlanarInternal.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace tapioca::geometry::detail {

namespace {

constexpr double kPi = 3.14159265358979323846;

bool SameXY (Vec3 a, Vec3 b)
{
    return a.x == b.x && a.y == b.y;
}

bool FinitePoint (Vec3 p)
{
    return std::isfinite (p.x) && std::isfinite (p.y) && std::isfinite (p.z);
}

bool NonzeroHandle (Vec3 handle)
{
    return handle.x != 0.0 || handle.y != 0.0;
}

double Length2 (Vec3 a)
{
    return std::hypot (a.x, a.y);
}

Vec3 Midpoint (Vec3 a, Vec3 b)
{
    // 0.5*a + 0.5*b avoids overflow for opposite finite extrema.
    return { a.x * 0.5 + b.x * 0.5, a.y * 0.5 + b.y * 0.5, 0.0 };
}

double PointSegmentDistance (Vec3 point, Vec3 a, Vec3 b)
{
    const double scale = std::max (
        { std::abs (point.x), std::abs (point.y), std::abs (a.x), std::abs (a.y), std::abs (b.x), std::abs (b.y) });
    if (!(scale > 0.0))
        return 0.0;
    const double ax = a.x / scale, ay = a.y / scale;
    const double bx = b.x / scale, by = b.y / scale;
    const double px = point.x / scale, py = point.y / scale;
    const double dx = bx - ax, dy = by - ay;
    const double length2 = dx * dx + dy * dy;
    double t = 0.0;
    if (length2 > 0.0)
        t = std::clamp (((px - ax) * dx + (py - ay) * dy) / length2, 0.0, 1.0);
    const double qx = ax + t * dx;
    const double qy = ay + t * dy;
    return std::hypot (point.x - qx * scale, point.y - qy * scale);
}

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

bool SubdivisionLimit (PlanarMap& out, const PlanarOptions& options, std::string& error, Id source, Vec3 at)
{
    AddDiagnostic (out, PlanarDiagnosticCode::WorkLimit, source, kNoId, at,
                   static_cast<double> (options.maxSubdivisionDepth),
                   "cubic subdivision depth limit exceeded before chordTolerance");
    error = "cubic subdivision depth limit exceeded before chordTolerance";
    return false;
}

bool ArcPrecisionLimit (PlanarMap& out, std::string& error, Id source, Vec3 at, std::string reason)
{
    error = "arc chord tolerance cannot be met: " + reason;
    AddDiagnostic (out, PlanarDiagnosticCode::WorkLimit, source, kNoId, at, 0.0, error);
    return false;
}

bool ArcTooManySegments (PlanarMap& out, const PlanarOptions& options, std::string& error, Id source, Vec3 at)
{
    error = "arc flattening would exceed maxSegments; the finite angle is below a full turn";
    AddDiagnostic (out, PlanarDiagnosticCode::TooManySegments, source, kNoId, at,
                   static_cast<double> (options.maxSegments), error);
    return false;
}

bool AddFlattened (std::vector<PieceSegment>& segments, const PlanarOptions& options, PlanarMap& out,
                   std::string& error, Vec3 a, Vec3 b, Id source, uint32_t piece, double u0, double u1, bool openStart,
                   bool openEnd)
{
    if (segments.size () >= options.maxSegments)
        return TooManySegments (out, options, error);
    const uint32_t ordinal = static_cast<uint32_t> (segments.size ());
    segments.push_back ({ a, b, source, piece, u0, u1, openStart, openEnd, ordinal });
    return true;
}

bool FlattenCubic (std::vector<PieceSegment>& segments, const PlanarOptions& options, PlanarMap& out,
                   std::string& error, Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, Id source, uint32_t piece, bool openStart,
                   bool openEnd)
{
    struct Node {
        Vec3 p0, p1, p2, p3;
        double t0 = 0.0, t1 = 1.0;
        std::size_t depth = 0;
    };
    std::vector<Node> stack;
    stack.push_back ({ p0, p1, p2, p3, 0.0, 1.0, 0 });
    while (!stack.empty ()) {
        Node node = stack.back ();
        stack.pop_back ();
        if (!FinitePoint (node.p0) || !FinitePoint (node.p1) || !FinitePoint (node.p2) || !FinitePoint (node.p3)) {
            error = "cubic control point is non-finite";
            AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, source, kNoId, node.p0, 0.0, error);
            return false;
        }
        if (SameXY (node.p0, node.p3)) {
            error = "zero-length cubic chord with nontrivial control points is ambiguous";
            AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, source, kNoId, node.p0, 0.0, error);
            return false;
        }
        const double deviation = std::max (PointSegmentDistance (node.p1, node.p0, node.p3),
                                           PointSegmentDistance (node.p2, node.p0, node.p3));
        if (deviation <= options.chordTolerance) {
            if (!AddFlattened (segments, options, out, error, node.p0, node.p3, source, piece, node.t0, node.t1,
                               openStart && node.t0 == 0.0, openEnd && node.t1 == 1.0))
                return false;
            continue;
        }
        if (node.depth >= options.maxSubdivisionDepth)
            return SubdivisionLimit (out, options, error, source, node.p0);
        if (segments.size () + stack.size () + 2 > options.maxSegments)
            return TooManySegments (out, options, error);

        const Vec3 p01 = Midpoint (node.p0, node.p1);
        const Vec3 p12 = Midpoint (node.p1, node.p2);
        const Vec3 p23 = Midpoint (node.p2, node.p3);
        const Vec3 p012 = Midpoint (p01, p12);
        const Vec3 p123 = Midpoint (p12, p23);
        const Vec3 p0123 = Midpoint (p012, p123);
        const double tm = node.t0 * 0.5 + node.t1 * 0.5;
        stack.push_back ({ p0123, p123, p23, node.p3, tm, node.t1, node.depth + 1 });
        stack.push_back ({ node.p0, p01, p012, p0123, node.t0, tm, node.depth + 1 });
    }
    return true;
}

bool FlattenLineOrArc (std::vector<PieceSegment>& segments, const PlanarOptions& options, PlanarMap& out,
                       std::string& error, Vec3 a, Vec3 b, Id source, uint32_t piece, double angle, bool openStart,
                       bool openEnd)
{
    const double sweep = angle; // Positive Archicad arcAngle bulges right.
    std::size_t count = 1;
    if (std::abs (sweep) >= 1.0e-12) {
        const Vec3 chordVector { b.x - a.x, b.y - a.y, 0.0 };
        const double chord = Length2 (chordVector);
        if (!(chord > 0.0)) {
            error = "source " + std::to_string (source) + " has a zero-length arc chord";
            AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, source, kNoId, a, 0.0, error);
            return false;
        }
        const double half = sweep * 0.5;
        const double sinHalf = std::sin (half);
        if (std::abs (sinHalf) < 1.0e-15)
            return ArcPrecisionLimit (out, error, source, a,
                                      "the angle is numerically indistinguishable from a full turn");
        const double radius = chord / (2.0 * std::abs (sinHalf));
        if (!(radius > 0.0) || !std::isfinite (radius))
            return ArcPrecisionLimit (out, error, source, a, "the arc radius is non-finite");
        const double quarter = std::sin (std::abs (sweep) * 0.25);
        const double sagitta = 2.0 * radius * quarter * quarter;

        // A loose sagitta allowance must not collapse a closed arc into one
        // coincident chord. Keep at least two chords for any nonzero arc, and
        // at least one chord per pi/2 of sweep; this is a topology floor, not
        // a second tolerance.
        const double topologyAngle = kPi * 0.5;
        const double minimumWanted = std::max (2.0, std::ceil (std::abs (sweep) / topologyAngle));
        if (!std::isfinite (minimumWanted) || minimumWanted > static_cast<double> (options.maxSegments))
            return ArcTooManySegments (out, options, error, source, a);
        count = std::max<std::size_t> (1, static_cast<std::size_t> (minimumWanted));

        if (sagitta > options.chordTolerance) {
            // sagitta = 2R sin^2(delta/4); this inversion does not subtract
            // tiny values from one.
            const double ratio = options.chordTolerance / (2.0 * radius);
            if (!(ratio >= 0.0) || !std::isfinite (ratio))
                return ArcPrecisionLimit (out, error, source, a, "the sagitta ratio is non-finite");
            const double maxAngle = ratio >= 1.0 ? 2.0 * kPi : 4.0 * std::asin (std::sqrt (std::min (ratio, 1.0)));
            if (!(maxAngle > 0.0) || !std::isfinite (maxAngle))
                return ArcPrecisionLimit (out, error, source, a, "the chord-angle inversion is non-finite");
            const double wanted = std::ceil (std::abs (sweep) / maxAngle);
            if (!std::isfinite (wanted))
                return ArcPrecisionLimit (out, error, source, a, "the required chord count is non-finite");
            if (wanted > static_cast<double> (options.maxSegments))
                return ArcTooManySegments (out, options, error, source, a);
            count = std::max (count, std::max<std::size_t> (1, static_cast<std::size_t> (wanted)));
        }
        if (count > options.maxSegments || segments.size () > options.maxSegments - count)
            return ArcTooManySegments (out, options, error, source, a);
    }
    else if (segments.size () >= options.maxSegments)
        return TooManySegments (out, options, error);

    if (count == 1) {
        if (!AddFlattened (segments, options, out, error, a, b, source, piece, 0.0, 1.0, openStart, openEnd))
            return false;
        return true;
    }

    const Vec3 chordVector { b.x - a.x, b.y - a.y, 0.0 };
    const double chord = Length2 (chordVector);
    const Vec3 left { -chordVector.y / chord, chordVector.x / chord, 0.0 };
    const double half = sweep * 0.5;
    const double tanHalf = std::tan (half);
    const double offset = std::abs (tanHalf) < 1.0e-15 ? 0.0 : (chord * 0.5) / tanHalf;
    const Vec3 centre { (a.x + b.x) * 0.5 + left.x * offset, (a.y + b.y) * 0.5 + left.y * offset, 0.0 };
    const double radius = chord / (2.0 * std::abs (std::sin (half)));
    const double start = std::atan2 (a.y - centre.y, a.x - centre.x);
    const auto at = [&] (double t) {
        const double theta = start + sweep * t;
        return Vec3 { centre.x + radius * std::cos (theta), centre.y + radius * std::sin (theta), 0.0 };
    };

    for (std::size_t i = 0; i < count; ++i) {
        const double t0 = static_cast<double> (i) / static_cast<double> (count);
        const double t1 = static_cast<double> (i + 1) / static_cast<double> (count);
        const Vec3 p0 = i == 0 ? a : at (t0);
        const Vec3 p1 = i + 1 == count ? b : at (t1);
        if (!AddFlattened (segments, options, out, error, p0, p1, source, piece, t0, t1, openStart && i == 0,
                           openEnd && i + 1 == count))
            return false;
    }
    return true;
}

bool FlattenSegment (std::vector<PieceSegment>& segments, const PlanarOptions& options, PlanarMap& out,
                     std::string& error, Vec3 a, Vec3 b, Id source, uint32_t piece, double angle, Vec3 outHandle,
                     Vec3 inHandle, bool openStart, bool openEnd)
{
    const bool cubic = NonzeroHandle (outHandle) || NonzeroHandle (inHandle);
    if (cubic) {
        if (std::abs (angle) > 1.0e-12) {
            error = "source " + std::to_string (source) + " has both an arc angle and cubic handles on one segment";
            AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, source, kNoId, a, angle, error);
            return false;
        }
        const Vec3 c1 { a.x + outHandle.x, a.y + outHandle.y, 0.0 };
        const Vec3 c2 { b.x + inHandle.x, b.y + inHandle.y, 0.0 };
        if (!FinitePoint (c1) || !FinitePoint (c2)) {
            error = "source " + std::to_string (source) + " has a non-finite cubic control point";
            AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, source, kNoId, a, 0.0, error);
            return false;
        }
        return FlattenCubic (segments, options, out, error, a, c1, c2, b, source, piece, openStart, openEnd);
    }
    return FlattenLineOrArc (segments, options, out, error, a, b, source, piece, angle, openStart, openEnd);
}

} // namespace

bool NormalizeCurves (const std::vector<PlanarCurve>& curves, std::vector<NormalizedCurve>& normalized, PlanarMap& out,
                      std::string& error)
{
    std::set<Id> sources;
    for (const auto& curve : curves) {
        if (curve.source == kNoId) {
            error = "a planar curve has source ID 0 (absent)";
            AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, kNoId, kNoId, {}, 0.0, error);
            return false;
        }
        if (!sources.insert (curve.source).second) {
            error = "duplicate source ID " + std::to_string (curve.source);
            AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, curve.source, curve.source, {}, 0.0, error);
            return false;
        }
        if (curve.points.size () < 2) {
            error = "source " + std::to_string (curve.source) + " needs at least two points";
            AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, curve.source, kNoId, {}, 0.0, error);
            return false;
        }
        if (curve.points.size () > std::numeric_limits<uint32_t>::max ()) {
            error = "source " + std::to_string (curve.source) + " has too many points for 32-bit piece handles";
            AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, curve.source, kNoId, {}, 0.0, error);
            return false;
        }
        for (const auto& point : curve.points) {
            if (!FinitePoint (point)) {
                error = "source " + std::to_string (curve.source) + " has a non-finite point";
                AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, curve.source, kNoId, point, 0.0, error);
                return false;
            }
        }
        if (!curve.arcAngles.empty () && curve.arcAngles.size () != curve.points.size ()) {
            error = "source " + std::to_string (curve.source) + " has one arc angle per vertex required";
            AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, curve.source, kNoId, {}, 0.0, error);
            return false;
        }
        if ((!curve.inHandles.empty () && curve.inHandles.size () != curve.points.size ()) ||
            (!curve.outHandles.empty () && curve.outHandles.size () != curve.points.size ())) {
            error = "source " + std::to_string (curve.source) + " has one handle per vertex required";
            AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, curve.source, kNoId, {}, 0.0, error);
            return false;
        }
        for (const auto& handle : curve.inHandles) {
            if (!FinitePoint (handle)) {
                error = "source " + std::to_string (curve.source) + " has a non-finite incoming handle";
                AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, curve.source, kNoId, handle, 0.0, error);
                return false;
            }
        }
        for (const auto& handle : curve.outHandles) {
            if (!FinitePoint (handle)) {
                error = "source " + std::to_string (curve.source) + " has a non-finite outgoing handle";
                AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, curve.source, kNoId, handle, 0.0, error);
                return false;
            }
        }
        constexpr double fullTurn = 2.0 * kPi;
        for (double angle : curve.arcAngles) {
            if (!std::isfinite (angle)) {
                error = "source " + std::to_string (curve.source) + " has a non-finite arc angle";
                AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, curve.source, kNoId, {}, angle, error);
                return false;
            }
            if (std::abs (angle) >= fullTurn) {
                error = "source " + std::to_string (curve.source) +
                        " has an arc angle at or beyond a full turn; use a finite angle below 2*pi";
                AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, curve.source, kNoId, {}, angle, error);
                return false;
            }
        }

        const std::size_t pointCount = curve.points.size ();
        const std::size_t segmentCount = curve.closed ? pointCount : pointCount - 1;
        if (!curve.arcAngles.empty ()) {
            for (std::size_t i = 0; i < segmentCount; ++i) {
                const std::size_t next = (i + 1) % pointCount;
                if (SameXY (curve.points[i], curve.points[next]) && std::abs (curve.arcAngles[i]) > 1.0e-12) {
                    error = "source " + std::to_string (curve.source) +
                            " has a nonzero arc angle on a zero-length segment (ambiguous)";
                    AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, curve.source, kNoId, curve.points[i],
                                   curve.arcAngles[i], error);
                    return false;
                }
            }
        }

        NormalizedCurve line;
        line.source = curve.source;
        line.closed = curve.closed;
        line.angles = curve.arcAngles;
        line.inHandles = curve.inHandles;
        line.outHandles = curve.outHandles;
        std::size_t start = 0;
        if (curve.closed) {
            bool found = false;
            for (std::size_t i = 0; i < pointCount; ++i) {
                if (!SameXY (curve.points[i], curve.points[(i + pointCount - 1) % pointCount])) {
                    start = i;
                    found = true;
                    break;
                }
            }
            if (!found) {
                error = "source " + std::to_string (curve.source) + " needs at least two distinct points";
                AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, curve.source, kNoId, curve.points.front (), 0.0,
                               error);
                return false;
            }
        }
        for (std::size_t k = 0; k < pointCount; ++k) {
            const std::size_t index = (start + k) % pointCount;
            if (!line.points.empty () && SameXY (line.points.back ().p, curve.points[index])) {
                line.points.back ().lastOriginal = static_cast<uint32_t> (index);
                line.points.back ().originals.push_back (static_cast<uint32_t> (index));
            }
            else {
                NormalizedPoint point;
                point.p = curve.points[index];
                point.firstOriginal = static_cast<uint32_t> (index);
                point.lastOriginal = static_cast<uint32_t> (index);
                point.originals.push_back (static_cast<uint32_t> (index));
                line.points.push_back (std::move (point));
            }
        }
        if (line.points.size () < 2) {
            error = "source " + std::to_string (curve.source) + " needs at least two distinct points";
            AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, curve.source, kNoId, curve.points.front (), 0.0,
                           error);
            return false;
        }

        // A duplicate run's incoming handle belongs to its first input vertex,
        // its outgoing handle and leaving segment to the last. A handle on any
        // zero-length duplicate in between would be silently lost, so refuse.
        for (const auto& run : line.points) {
            for (uint32_t original : run.originals) {
                if (!line.inHandles.empty () && original != run.firstOriginal &&
                    NonzeroHandle (line.inHandles[original])) {
                    error = "source " + std::to_string (curve.source) +
                            " has an unused incoming handle on a zero-length duplicate vertex (ambiguous)";
                    AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, curve.source, kNoId, curve.points[original],
                                   0.0, error);
                    return false;
                }
                if (!line.outHandles.empty () && original != run.lastOriginal &&
                    NonzeroHandle (line.outHandles[original])) {
                    error = "source " + std::to_string (curve.source) +
                            " has an unused outgoing handle on a zero-length duplicate vertex (ambiguous)";
                    AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, curve.source, kNoId, curve.points[original],
                                   0.0, error);
                    return false;
                }
            }
        }
        if (!line.closed) {
            if (!line.inHandles.empty () && NonzeroHandle (line.inHandles[line.points.front ().firstOriginal])) {
                error = "source " + std::to_string (curve.source) +
                        " has an incoming handle at the open start with no incoming segment";
                AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, curve.source, kNoId,
                               curve.points[line.points.front ().firstOriginal], 0.0, error);
                return false;
            }
            if (!line.outHandles.empty () && NonzeroHandle (line.outHandles[line.points.back ().lastOriginal])) {
                error = "source " + std::to_string (curve.source) +
                        " has an outgoing handle at the open end with no leaving segment";
                AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, curve.source, kNoId,
                               curve.points[line.points.back ().lastOriginal], 0.0, error);
                return false;
            }
        }

        if (!line.angles.empty ()) {
            std::set<uint32_t> used;
            const std::size_t usedCount = line.closed ? line.points.size () : line.points.size () - 1;
            for (std::size_t i = 0; i < usedCount; ++i)
                used.insert (line.points[i].lastOriginal);
            for (std::size_t i = 0; i < line.angles.size (); ++i) {
                if (used.count (static_cast<uint32_t> (i)) == 0 && std::abs (line.angles[i]) > 1.0e-12) {
                    error = "source " + std::to_string (curve.source) +
                            " has an arc angle at a vertex with no leaving segment";
                    AddDiagnostic (out, PlanarDiagnosticCode::Degenerate, curve.source, kNoId, curve.points[i], 0.0,
                                   error);
                    return false;
                }
            }
        }
        normalized.push_back (std::move (line));
    }
    return true;
}

bool FlattenCurves (const std::vector<NormalizedCurve>& normalized, const PlanarOptions& options,
                    std::vector<PieceSegment>& segments, PlanarMap& out, std::string& error)
{
    for (const auto& curve : normalized) {
        const std::size_t segmentCount = curve.closed ? curve.points.size () : curve.points.size () - 1;
        for (std::size_t i = 0; i < segmentCount; ++i) {
            const NormalizedPoint& from = curve.points[i];
            const NormalizedPoint& to = curve.points[(i + 1) % curve.points.size ()];
            const double angle = curve.angles.empty () ? 0.0 : curve.angles[from.lastOriginal];
            const Vec3 outHandle = curve.outHandles.empty () ? Vec3 {} : curve.outHandles[from.lastOriginal];
            const Vec3 inHandle = curve.inHandles.empty () ? Vec3 {} : curve.inHandles[to.firstOriginal];
            const bool openStart = !curve.closed && i == 0;
            const bool openEnd = !curve.closed && i + 1 == segmentCount;
            if (!FlattenSegment (segments, options, out, error, from.p, to.p, curve.source, from.lastOriginal, angle,
                                 outHandle, inHandle, openStart, openEnd))
                return false;
        }
    }
    return true;
}

} // namespace tapioca::geometry::detail
