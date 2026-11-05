#include "Edit/EditPolyline.hpp"
#include "Edit/EditPolylineDetail.hpp"
#include "Edit/EditPlanar.hpp"
#include <clipper2/clipper.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace geomsrv::edit::polyline_detail {
namespace {

namespace cp = Clipper2Lib;
namespace planar = tapioca::geometry;
constexpr double kSupportTolerance = 1.0e-6;
constexpr double kParameterTolerance = 1.0e-7;
constexpr int kClipperPrecision = 7; // about 0.06 micrometres, safely below kMinSegment

struct OriginalEdge {
    Vec3 a, b;
    Segment attributes;
};

bool SameAttributes (const Segment& a, const Segment& b)
{
    return a.width == b.width && a.tag == b.tag;
}

bool RemoveCollapsed (Entity& entity, std::string& error)
{
    bool changed = true;
    while (changed) {
        changed = false;
        const size_t n = entity.vertices.size ();
        for (size_t i = 1; i < n; ++i) {
            const Vec3 a = entity.vertices[i - 1].p, b = entity.vertices[i].p;
            if (DistanceXY (a, b) >= kMinSegment)
                continue;
            if (std::abs (a.z - b.z) > kSupportTolerance) {
                error = "a collapsed offset edge has incompatible endpoint elevations";
                return false;
            }
            const bool hasNext = entity.closed || i < entity.segments.size ();
            if (hasNext && !SameAttributes (entity.segments[i - 1], entity.segments[i])) {
                error = "a collapsed offset edge has incompatible segment attributes";
                return false;
            }
            if (n <= MinVertices (entity)) {
                error = "offset cleanup would leave too few vertices";
                return false;
            }
            entity.vertices.erase (entity.vertices.begin () + static_cast<std::ptrdiff_t> (i));
            entity.segments.erase (entity.segments.begin () + static_cast<std::ptrdiff_t> (i - 1));
            changed = true;
            break;
        }
        if (changed || !entity.closed || entity.vertices.empty ())
            continue;
        const Vec3 a = entity.vertices.back ().p, b = entity.vertices.front ().p;
        if (DistanceXY (a, b) >= kMinSegment)
            continue;
        const size_t segmentCount = entity.segments.size ();
        if (std::abs (a.z - b.z) > kSupportTolerance ||
            (segmentCount > 1 &&
             !SameAttributes (entity.segments[segmentCount - 1], entity.segments[segmentCount - 2]))) {
            error = "the collapsed closing edge has incompatible elevation or attributes";
            return false;
        }
        if (entity.vertices.size () <= MinVertices (entity)) {
            error = "offset cleanup would leave too few vertices";
            return false;
        }
        entity.vertices.pop_back ();
        entity.segments.pop_back ();
        changed = true;
    }
    if (entity.vertices.size () < MinVertices (entity) || entity.segments.size () != SegmentCount (entity)) {
        error = "offset cleanup left an invalid vertex or segment count";
        return false;
    }
    return true;
}

std::vector<OriginalEdge> Edges (const Entity& entity)
{
    std::vector<OriginalEdge> edges;
    for (size_t i = 0; i < SegmentCount (entity); ++i) {
        size_t from = 0, to = 0;
        SegmentEnds (entity, i, from, to);
        edges.push_back ({ entity.vertices[from].p, entity.vertices[to].p, entity.segments[i] });
    }
    return edges;
}

double Along (Vec3 point, Vec3 a, Vec3 b)
{
    const double dx = b.x - a.x, dy = b.y - a.y, length2 = dx * dx + dy * dy;
    return length2 > 0.0 ? ((point.x - a.x) * dx + (point.y - a.y) * dy) / length2 : 0.0;
}

double DistanceToSegment (Vec3 point, Vec3 a, Vec3 b)
{
    return DistanceXY (point, Lerp (a, b, std::clamp (Along (point, a, b), 0.0, 1.0)));
}

bool RepeatedNonAdjacent (const Entity& entity)
{
    const size_t n = entity.vertices.size ();
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j) {
            if (entity.closed && (j == i + 1 || (i == 0 && j + 1 == n)))
                continue;
            if (DistanceXY (entity.vertices[i].p, entity.vertices[j].p) <= kSupportTolerance)
                return true;
        }
    return false;
}

bool EndpointContact (const Entity& entity)
{
    const auto edges = Edges (entity);
    for (size_t i = 0; i < entity.vertices.size (); ++i)
        for (size_t j = 0; j < edges.size (); ++j) {
            const size_t incoming = (i + edges.size () - 1) % edges.size ();
            if (j == i || j == incoming) // an authored vertex always touches its two incident edges
                continue;
            if (DistanceXY (entity.vertices[i].p, edges[j].a) > kSupportTolerance &&
                DistanceXY (entity.vertices[i].p, edges[j].b) > kSupportTolerance &&
                DistanceToSegment (entity.vertices[i].p, edges[j].a, edges[j].b) <= kSupportTolerance)
                return true; // interior by physical distance, never a fraction of a long edge
        }
    return false;
}

planar::PlanarCurve Curve (const Entity& entity)
{
    planar::PlanarCurve curve;
    curve.source = 1;
    curve.closed = entity.closed;
    for (const auto& vertex : entity.vertices)
        curve.points.push_back (vertex.p);
    return curve;
}

bool Has (const planar::PlanarMap& map, planar::PlanarDiagnosticCode code)
{
    return std::any_of (map.diagnostics.begin (), map.diagnostics.end (),
                        [code] (const planar::PlanarDiagnostic& diagnostic) { return diagnostic.code == code; });
}

bool AmbiguousOpenOverlap (const planar::PlanarMap& map)
{
    for (const auto& edge : map.edges)
        for (size_t i = 0; i < edge.pieces.size (); ++i)
            for (size_t j = i + 1; j < edge.pieces.size (); ++j)
                if (edge.pieces[i].source == 1 && edge.pieces[j].source == 1 &&
                    edge.pieces[i].piece != edge.pieces[j].piece)
                    return true; // coincident source intervals cannot retain both attribute owners
    return false;
}

struct Interval {
    double start = 0.0, end = 1.0;
};

bool RebuildOpen (const Entity& input, const planar::PlanarMap& map, Entity& output, std::string& error)
{
    const auto edges = Edges (input);
    std::vector<std::vector<Interval>> intervals (edges.size ());
    for (const auto& edge : map.edges)
        for (const auto& piece : edge.pieces) {
            if (piece.source != 1)
                continue;
            if (piece.piece >= intervals.size () || !(piece.sourceStart >= 0.0 && piece.sourceEnd <= 1.0) ||
                !(piece.sourceStart < piece.sourceEnd)) {
                error = "offset crossing cleanup returned an invalid source interval";
                return false;
            }
            intervals[piece.piece].push_back ({ piece.sourceStart, piece.sourceEnd });
        }

    output = input;
    output.vertices.clear ();
    output.segments.clear ();
    for (size_t i = 0; i < intervals.size (); ++i) {
        auto& pieces = intervals[i];
        std::sort (pieces.begin (), pieces.end (), [] (const Interval& a, const Interval& b) {
            return a.start < b.start || (a.start == b.start && a.end < b.end);
        });
        double cursor = 0.0;
        for (const auto& piece : pieces) {
            if (std::abs (piece.start - cursor) > kParameterTolerance) {
                error = "an open offset crossing has a gap or ambiguous overlap; splitting is required explicitly";
                return false;
            }
            cursor = piece.end;
            const Vec3 a = Lerp (edges[i].a, edges[i].b, piece.start), b = Lerp (edges[i].a, edges[i].b, piece.end);
            if (output.vertices.empty ())
                output.vertices.push_back ({ kNoId, a });
            else if (Distance (output.vertices.back ().p, a) > kSupportTolerance) {
                error = "offset crossing cleanup could not preserve open traversal continuity";
                return false;
            }
            if (Distance (a, b) < kMinSegment) {
                error = "offset crossing cleanup would create a sub-minimum edge";
                return false;
            }
            output.vertices.push_back ({ kNoId, b });
            Segment attributes = edges[i].attributes;
            attributes.id = kNoId;
            output.segments.push_back (attributes);
        }
        if (pieces.empty () || std::abs (cursor - 1.0) > kParameterTolerance) {
            error = "offset crossing cleanup did not retain every open source interval";
            return false;
        }
    }
    return true;
}

struct SupportedPiece {
    Vec3 a, b;
    Segment attributes;
};

std::vector<double> Splits (Vec3 a, Vec3 b, const std::vector<OriginalEdge>& input)
{
    std::vector<double> splits { 0.0, 1.0 };
    for (const auto& edge : input)
        for (const Vec3 point : { edge.a, edge.b })
            if (DistanceToSegment (point, a, b) <= kSupportTolerance)
                splits.push_back (std::clamp (Along (point, a, b), 0.0, 1.0));
    std::sort (splits.begin (), splits.end ());
    const double length = DistanceXY (a, b);
    std::vector<double> kept { 0.0 };
    for (const double split : splits)
        if ((split - kept.back ()) * length > kParameterTolerance * std::max (1.0, length))
            kept.push_back (split);
    if (kept.size () > 1 && (1.0 - kept.back ()) * length <= kParameterTolerance * std::max (1.0, length))
        kept.back () = 1.0; // a snapped endpoint is the same point, not a closing sliver
    else if (kept.back () != 1.0)
        kept.push_back (1.0);
    return kept;
}

bool Support (Vec3 a, Vec3 b, const std::vector<OriginalEdge>& input, Segment& attributes, Vec3& atA, Vec3& atB,
              std::string& error)
{
    struct Candidate {
        size_t index = 0;
        Vec3 a, b;
    };
    std::vector<Candidate> candidates;
    for (size_t i = 0; i < input.size (); ++i) {
        const auto& edge = input[i];
        if (DistanceToSegment (a, edge.a, edge.b) > kSupportTolerance ||
            DistanceToSegment (b, edge.a, edge.b) > kSupportTolerance)
            continue;
        const double ta = Along (a, edge.a, edge.b), tb = Along (b, edge.a, edge.b);
        const double low = std::min (ta, tb), high = std::max (ta, tb);
        const double reach = kSupportTolerance / std::max (kMinSegment, DistanceXY (edge.a, edge.b));
        if (low < -reach || high > 1.0 + reach)
            continue;
        candidates.push_back (
            { i, Lerp (edge.a, edge.b, std::clamp (ta, 0.0, 1.0)), Lerp (edge.a, edge.b, std::clamp (tb, 0.0, 1.0)) });
    }
    if (candidates.empty ()) {
        error = "a cleaned offset edge has no source support";
        return false;
    }
    attributes = input[candidates.front ().index].attributes;
    attributes.id = kNoId;
    atA = candidates.front ().a;
    atB = candidates.front ().b;
    for (const auto& candidate : candidates) {
        if (!SameAttributes (attributes, input[candidate.index].attributes) ||
            Distance (atA, candidate.a) > kSupportTolerance || Distance (atB, candidate.b) > kSupportTolerance) {
            error = "a cleaned offset edge has ambiguous attributes or elevation";
            return false;
        }
    }
    atA = { a.x, a.y, atA.z };
    atB = { b.x, b.y, atB.z };
    return true;
}

bool BuildCleanRing (const cp::PathD& ring, const Entity& input, Entity& output, std::string& error)
{
    const auto inputEdges = Edges (input);
    if (ring.size () < 3 || inputEdges.size () > 2000000 / std::max<size_t> (1, ring.size ())) {
        error = "closed offset cleanup exceeds its representable boundary budget";
        return false;
    }
    output = input;
    output.vertices.clear ();
    output.segments.clear ();
    for (size_t i = 0; i < ring.size (); ++i) {
        const Vec3 a { ring[i].x, ring[i].y, 0.0 },
            b { ring[(i + 1) % ring.size ()].x, ring[(i + 1) % ring.size ()].y, 0.0 };
        const auto splits = Splits (a, b, inputEdges);
        for (size_t j = 1; j < splits.size (); ++j) {
            const Vec3 from = Lerp (a, b, splits[j - 1]), to = Lerp (a, b, splits[j]);
            if (DistanceXY (from, to) < kMinSegment) {
                error = "closed offset cleanup would create a sub-minimum edge";
                return false;
            }
            SupportedPiece piece;
            Vec3 supportedA, supportedB;
            if (!Support (from, to, inputEdges, piece.attributes, supportedA, supportedB, error))
                return false;
            if (output.vertices.empty ())
                output.vertices.push_back ({ kNoId, supportedA });
            else if (Distance (output.vertices.back ().p, supportedA) > kSupportTolerance) {
                error = "closed offset cleanup found an elevation or continuity ambiguity";
                return false;
            }
            output.segments.push_back (piece.attributes);
            const bool last = i + 1 == ring.size () && j + 1 == splits.size ();
            if (!last)
                output.vertices.push_back ({ kNoId, supportedB });
            else if (Distance (output.vertices.front ().p, supportedB) > kSupportTolerance) {
                error = "closed offset cleanup could not close the result consistently";
                return false;
            }
        }
    }
    if (RepeatedNonAdjacent (output)) {
        error = "closed offset cleanup produced a pinched or touching contour";
        return false;
    }
    return true;
}

bool RebuildClosed (const Entity& input, Entity& output, std::string& error)
{
    cp::PathD path;
    for (const auto& vertex : input.vertices) {
        if (!Finite (vertex.p) || std::max (std::abs (vertex.p.x), std::abs (vertex.p.y)) > 1.0e9) {
            error = "closed offset cleanup requires finite coordinates within +/-1e9 metres";
            return false;
        }
        path.push_back ({ vertex.p.x, vertex.p.y });
    }
    if (path.size () > 20000) {
        error = "closed offset cleanup exceeds 20000 boundary segments";
        return false;
    }
    cp::ClipperD clipper (kClipperPrecision);
    clipper.AddSubject (cp::PathsD { path });
    cp::PolyTreeD tree;
    if (!clipper.Execute (cp::ClipType::Union, cp::FillRule::NonZero, tree)) {
        error = "closed offset cleanup union refused";
        return false;
    }
    if (tree.Count () != 1 || tree.Child (0)->IsHole () || tree.Child (0)->Count () != 0) {
        error = "closed offset cleanup needs one outer ring without holes or islands";
        return false;
    }
    if (!BuildCleanRing (tree.Child (0)->Polygon (), input, output, error))
        return false;
    const double before = SignedAreaXY (input.vertices), after = SignedAreaXY (output.vertices);
    if (!(std::abs (after) > 0.0) || !std::isfinite (after)) {
        error = "closed offset cleanup produced an empty contour";
        return false;
    }
    // A self-crossing zero-area input has no signed winding to preserve; Clipper's outer order is
    // deterministic there. Otherwise retain the authored sign, and always keep extrusions CCW.
    if ((input.kind == Kind::Extrusion && after < 0.0) || (std::abs (before) > 0.0 && before * after < 0.0))
        ReverseOrder (output);
    return true;
}

} // namespace

bool CleanupOffset (Entity& entity, std::string& error)
{
    if (!RemoveCollapsed (entity, error))
        return false;
    planar::PlanarMap map;
    if (!planar::BuildPlanarMap ({ Curve (entity) }, planar::PlanarOptions {}, map, error)) {
        error = "offset cleanup arrangement refused: " + error;
        return false;
    }
    const bool crossing = Has (map, planar::PlanarDiagnosticCode::SelfIntersection);
    if (!entity.closed && AmbiguousOpenOverlap (map)) {
        error = "an open offset has coincident source intervals; split or edit them explicitly";
        return false;
    }
    bool closedCleanup = false;
    if (!entity.closed && crossing) {
        Entity cleaned;
        if (!RebuildOpen (entity, map, cleaned, error))
            return false;
        entity = std::move (cleaned);
    }
    else if (entity.closed && (crossing || Has (map, planar::PlanarDiagnosticCode::Tangent) ||
                               RepeatedNonAdjacent (entity) || EndpointContact (entity))) {
        Entity cleaned;
        if (!RebuildClosed (entity, cleaned, error))
            return false;
        entity = std::move (cleaned);
        closedCleanup = true;
    }
    if (!RemoveCollapsed (entity, error))
        return false;
    if (closedCleanup) {
        if (RepeatedNonAdjacent (entity) || EndpointContact (entity)) {
            error = "closed offset cleanup left a touching contour";
            return false;
        }
        planar::PlanarMap verified;
        if (!planar::BuildPlanarMap ({ Curve (entity) }, planar::PlanarOptions {}, verified, error) ||
            Has (verified, planar::PlanarDiagnosticCode::SelfIntersection)) {
            error = "closed offset cleanup did not produce one simple contour";
            return false;
        }
    }
    return true;
}

} // namespace geomsrv::edit::polyline_detail
