#include "Edit/EditLandscapeInternal.hpp"
#include "Edit/EditShape.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

namespace tapioca::geometry::landscape_detail {
namespace edit = geomsrv::edit;
namespace {
auto Key (const BoundaryRef& ref)
{
    return std::tuple { ref.source, ref.segment, ref.reversed };
}
void CanonicalCycle (std::vector<BoundaryRef>& cycle)
{
    cycle.erase (std::unique (cycle.begin (), cycle.end ()), cycle.end ());
    if (cycle.size () > 1 && cycle.front () == cycle.back ())
        cycle.pop_back ();
    if (cycle.empty ())
        return;
    size_t i = 0, j = 1, k = 0;
    while (i < cycle.size () && j < cycle.size () && k < cycle.size ()) {
        const auto a = Key (cycle[(i + k) % cycle.size ()]), b = Key (cycle[(j + k) % cycle.size ()]);
        if (a == b) {
            ++k;
            continue;
        }
        if (a < b) {
            j += k + 1;
            if (j <= i)
                j = i + 1;
        }
        else {
            i += k + 1;
            if (i <= j)
                i = j + 1;
        }
        k = 0;
    }
    const size_t best = std::min (i, j) % cycle.size ();
    std::rotate (cycle.begin (), cycle.begin () + static_cast<std::ptrdiff_t> (best), cycle.end ());
}
double Along (Vec3 point, Vec3 a, Vec3 b)
{
    const double dx = b.x - a.x, dy = b.y - a.y;
    return ((point.x - a.x) * dx + (point.y - a.y) * dy) / (dx * dx + dy * dy);
}
bool On (Vec3 point, Vec3 a, Vec3 b)
{
    const double t = Along (point, a, b);
    return t >= -1.0e-12 && t <= 1.0 + 1.0e-12 &&
           edit::DistanceXY (point, edit::Lerp (a, b, std::clamp (t, 0.0, 1.0))) <= kBoundaryEpsilon;
}
void CollectTree (const cp::PolyPathD& node, Shape& out)
{
    if (!node.Polygon ().empty () && !node.IsHole ()) {
        DerivedRegion region;
        const auto append = [&] (const cp::PathD& path) {
            std::vector<Vec3> ring;
            for (auto p : path)
                ring.push_back ({ p.x, p.y, 0.0 });
            region.rings.push_back (std::move (ring));
        };
        append (node.Polygon ());
        for (const auto& child : node)
            if (child->IsHole ())
                append (child->Polygon ());
        region.area = cp::Area (Paths (region));
        out.push_back (std::move (region));
    }
    for (const auto& child : node)
        CollectTree (*child, out);
}
bool Regions (const cp::PathsD& paths, Shape& out, std::string& error)
{
    size_t count = 0;
    for (const auto& ring : paths) {
        count += ring.size ();
        for (auto p : ring)
            if (!std::isfinite (p.x) || !std::isfinite (p.y) || std::max (std::abs (p.x), std::abs (p.y)) > 1.0e9) {
                error = "landscape clipping coordinates must be finite and within +/-1e9 metres";
                return false;
            }
    }
    if (count > 20000) {
        error = "landscape clipping exceeds 20000 boundary segments";
        return false;
    }
    cp::ClipperD clipper (kPrecision);
    cp::PolyTreeD tree;
    clipper.AddSubject (paths);
    if (!clipper.Execute (cp::ClipType::Union, cp::FillRule::NonZero, tree)) {
        error = "landscape polygon assembly refused";
        return false;
    }
    out.clear ();
    CollectTree (tree, out);
    for (auto& region : out) {
        std::vector<double> levels;
        for (const auto& ring : region.rings)
            for (auto p : ring)
                levels.push_back (p.y);
        std::sort (levels.begin (), levels.end ());
        levels.erase (std::unique (levels.begin (), levels.end ()), levels.end ());
        bool found = false;
        size_t work = 0;
        for (size_t i = 1; i < levels.size () && !found; ++i) {
            const double y = levels[i - 1] * 0.5 + levels[i] * 0.5;
            if (!(y > levels[i - 1] && y < levels[i]))
                continue;
            std::vector<double> xs;
            for (const auto& ring : region.rings)
                for (size_t j = 0; j < ring.size (); ++j) {
                    if (++work > 2000000) {
                        error = "landscape strict seed work budget exceeded";
                        return false;
                    }
                    const auto a = ring[j], b = ring[(j + 1) % ring.size ()];
                    if ((a.y > y) != (b.y > y))
                        xs.push_back (a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y));
                }
            std::sort (xs.begin (), xs.end ());
            for (size_t j = 1; j < xs.size (); ++j) {
                const Vec3 seed { xs[j - 1] * 0.5 + xs[j] * 0.5, y, 0 };
                if (PointInside (seed, region.rings)) {
                    region.seed = seed;
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            error = "landscape region has no representable strict interior seed";
            return false;
        }
    }
    return true;
}
bool TouchesBoundary (const DerivedRegion& output, const DerivedRegion& input)
{
    for (const auto& ring : output.rings)
        for (size_t i = 0; i < ring.size (); ++i) {
            const Vec3 a = ring[i], b = ring[(i + 1) % ring.size ()];
            for (const auto& edges : input.boundary)
                for (const auto& edge : edges) {
                    // Positive length overlap, not a tangency at a single point.
                    const double low = std::max (0.0, std::min (Along (edge.a, a, b), Along (edge.b, a, b)));
                    const double high = std::min (1.0, std::max (Along (edge.a, a, b), Along (edge.b, a, b)));
                    if (high > low &&
                        edit::DistanceXY (edit::Lerp (a, b, low), edit::Lerp (a, b, high)) > kBoundaryEpsilon &&
                        On (edit::Lerp (a, b, (low + high) * 0.5), edge.a, edge.b))
                        return true;
                }
        }
    return false;
}
bool Attribute (DerivedRegion& output, const Shape& parents, Id operation, const std::vector<Id>& order, bool generated,
                std::string& error)
{
    size_t outputEdges = 0, inputEdges = 0;
    for (const auto& ring : output.rings)
        outputEdges += ring.size ();
    for (const auto& parent : parents)
        for (const auto& ring : parent.boundary)
            inputEdges += ring.size ();
    if (outputEdges && inputEdges > 2000000 / outputEdges) {
        error = "landscape boundary attribution work budget exceeded";
        return false;
    }
    std::vector<const DerivedRegion*> contributors;
    std::set<Id> sources, history;
    for (const auto& input : parents) {
        const auto overlap = cp::Intersect (Paths (output), Paths (input), cp::FillRule::NonZero, kPrecision);
        if (!generated && !(std::abs (cp::Area (overlap)) > 0.0) && !TouchesBoundary (output, input))
            continue;
        contributors.push_back (&input);
        sources.insert (input.sources.begin (), input.sources.end ());
        history.insert (input.operations.begin (), input.operations.end ());
    }
    if (contributors.empty ()) {
        error = "ProvenanceUnattributable: polygon has no verified contributor";
        return false;
    }
    output.sources.assign (sources.begin (), sources.end ());
    if (operation)
        history.insert (operation);
    for (Id id : order)
        if (history.count (id))
            output.operations.push_back (id);
    for (const auto& ring : output.rings) {
        std::vector<RegionBoundaryPiece> pieces;
        for (size_t i = 0; i < ring.size (); ++i) {
            const Vec3 a = ring[i], b = ring[(i + 1) % ring.size ()];
            std::vector<double> splits { 0.0, 1.0 };
            if (!generated)
                for (const auto* input : contributors)
                    for (const auto& boundary : input->boundary)
                        for (const auto& edge : boundary)
                            for (Vec3 p : { edge.a, edge.b })
                                if (On (p, a, b))
                                    splits.push_back (std::clamp (Along (p, a, b), 0.0, 1.0));
            std::sort (splits.begin (), splits.end ());
            // Clipper snaps output vertices to its lattice, so an input endpoint
            // lands a fraction of the boundary epsilon inside the output edge. A
            // cut there leaves a sliver whose midpoint lies on no input edge;
            // splits within the epsilon of a kept one are the same point.
            const double length = edit::DistanceXY (a, b);
            std::vector<double> kept { 0.0 };
            for (double s : splits)
                if ((s - kept.back ()) * length > kBoundaryEpsilon)
                    kept.push_back (s);
            if (kept.size () > 1 && (1.0 - kept.back ()) * length <= kBoundaryEpsilon)
                kept.back () = 1.0;
            else if (kept.back () != 1.0)
                kept.push_back (1.0);
            splits = std::move (kept);
            for (size_t j = 1; j < splits.size (); ++j) {
                RegionBoundaryPiece piece;
                piece.a = edit::Lerp (a, b, splits[j - 1]);
                piece.b = edit::Lerp (a, b, splits[j]);
                if (edit::DistanceXY (piece.a, piece.b) <= 1.0e-10)
                    continue;
                const Vec3 mid = edit::Lerp (piece.a, piece.b, 0.5);
                piece.generated = generated;
                for (const auto* input : contributors)
                    for (const auto& boundary : input->boundary)
                        for (const auto& edge : boundary) {
                            if (!generated && !On (mid, edge.a, edge.b))
                                continue;
                            const bool reversed = edit::Dot (piece.b - piece.a, edge.b - edge.a) < 0.0;
                            for (auto support : edge.support) {
                                support.reversed = support.reversed != reversed;
                                piece.support.push_back (support);
                            }
                            if (!generated)
                                for (auto parameter : edge.parameters) {
                                    const double ta = std::clamp (Along (piece.a, edge.a, edge.b), 0.0, 1.0);
                                    const double tb = std::clamp (Along (piece.b, edge.a, edge.b), 0.0, 1.0);
                                    const double u0 = parameter.reversed ? parameter.sourceEnd : parameter.sourceStart;
                                    const double u1 = parameter.reversed ? parameter.sourceStart : parameter.sourceEnd;
                                    const double ua = u0 + ta * (u1 - u0), ub = u0 + tb * (u1 - u0);
                                    parameter.sourceStart = std::min (ua, ub);
                                    parameter.sourceEnd = std::max (ua, ub);
                                    parameter.reversed = ua > ub;
                                    piece.parameters.push_back (parameter);
                                }
                            piece.generated = piece.generated || edge.generated;
                        }
                std::sort (piece.support.begin (), piece.support.end (),
                           [] (const auto& x, const auto& y) { return Key (x) < Key (y); });
                piece.support.erase (std::unique (piece.support.begin (), piece.support.end ()), piece.support.end ());
                if (piece.support.empty ()) {
                    error = "ProvenanceUnattributable: output boundary lacks source support";
                    return false;
                }
                pieces.push_back (std::move (piece));
            }
        }
        output.boundary.push_back (std::move (pieces));
    }
    Describe (output);
    return true;
}
} // namespace

bool BoundaryLess (const std::vector<BoundaryRef>& a, const std::vector<BoundaryRef>& b)
{
    return std::lexicographical_compare (a.begin (), a.end (), b.begin (), b.end (),
                                         [] (const auto& x, const auto& y) { return Key (x) < Key (y); });
}
void Describe (DerivedRegion& region)
{
    auto& d = region.descriptor;
    d.object = region.objectId;
    d.sources = region.sources;
    d.operations = region.operations;
    d.boundaries.clear ();
    for (const auto& boundary : region.boundary) {
        std::vector<BoundaryRef> cycle;
        for (const auto& piece : boundary)
            cycle.insert (cycle.end (), piece.support.begin (), piece.support.end ());
        CanonicalCycle (cycle);
        d.boundaries.push_back (std::move (cycle));
    }
    if (d.boundaries.size () > 1)
        std::sort (d.boundaries.begin () + 1, d.boundaries.end (), BoundaryLess);
}
bool ReadSourceCurve (const edit::Document& doc, const LandscapeSource& source, SourceCurve& out, std::string& error)
{
    out.curve.source = source.id;
    const auto* entity = edit::Find (doc, source.entityId);
    if (entity == nullptr) {
        error = "source is not an entity curve";
        return false;
    }
    out.curve.closed = entity->closed;
    out.curve.arcAngles = source.arcAngles;
    const bool radii = std::any_of (entity->vertices.begin (), entity->vertices.end (),
                                    [] (const auto& v) { return v.radius != 0.0; });
    if (radii) {
        if (std::any_of (source.arcAngles.begin (), source.arcAngles.end (), [] (double a) { return a != 0.0; })) {
            error = "source cannot mix live corner radii with explicit arc angles";
            return false;
        }
        double radius = 0.0;
        for (const auto& v : entity->vertices)
            radius = std::max (radius, v.radius);
        const double step = std::min (0.15, std::sqrt (0.008 / std::max (radius, 0.001)));
        const auto features = edit::Features (*entity, step);
        for (const auto& edge : features.edges) {
            out.curve.points.push_back (edge.a);
            out.segments.push_back (entity->segments[edge.segment].id);
        }
        if (!entity->closed && !features.edges.empty ())
            out.curve.points.push_back (features.edges.back ().b);
        out.curve.arcAngles.clear ();
    }
    else {
        for (const auto& v : entity->vertices) {
            out.curve.points.push_back (v.p);
            out.curve.inHandles.push_back (v.in);
            out.curve.outHandles.push_back (v.out);
        }
        for (const auto& segment : entity->segments)
            out.segments.push_back (segment.id);
    }
    return true;
}
DerivedRegion FaceRegion (const PlanarMap& map, const PlanarFace& face, const std::map<Id, SourceCurve>& curves)
{
    DerivedRegion out;
    out.faceId = face.id;
    out.area = face.area;
    out.seed = face.seed;
    out.sources = face.sources;
    const auto append = [&] (const std::vector<uint32_t>& cycle) {
        std::vector<Vec3> ring;
        std::vector<RegionBoundaryPiece> pieces;
        for (uint32_t h : cycle) {
            const auto& half = map.halfEdges[h];
            const auto& edge = map.edges[half.edge];
            RegionBoundaryPiece piece;
            piece.a = map.vertices[half.origin];
            piece.b = map.vertices[map.halfEdges[half.twin].origin];
            for (auto parameter : edge.pieces) {
                parameter.reversed = parameter.reversed != (half.origin != edge.a);
                piece.parameters.push_back (parameter);
                piece.support.push_back ({ parameter.source, curves.at (parameter.source).segments.at (parameter.piece),
                                           parameter.reversed });
            }
            ring.push_back (piece.a);
            pieces.push_back (std::move (piece));
        }
        out.rings.push_back (std::move (ring));
        out.boundary.push_back (std::move (pieces));
    };
    append (face.halfEdges);
    for (uint32_t hole : face.holes)
        append (map.cycles[hole].halfEdges);
    Describe (out);
    return out;
}
cp::PathsD Paths (const DerivedRegion& region)
{
    cp::PathsD paths;
    for (const auto& ring : region.rings) {
        cp::PathD path;
        for (auto p : ring)
            path.push_back ({ p.x, p.y });
        paths.push_back (std::move (path));
    }
    return paths;
}
cp::PathsD Paths (const Shape& shape)
{
    cp::PathsD out;
    for (const auto& region : shape) {
        auto paths = Paths (region);
        out.insert (out.end (), paths.begin (), paths.end ());
    }
    return out;
}
bool BooleanShape (OperationKind kind, const Shape& a, const Shape& b, Id operation, const std::vector<Id>& order,
                   Shape& out, std::string& error)
{
    const auto type = kind == OperationKind::Difference     ? cp::ClipType::Difference
                      : kind == OperationKind::Intersection ? cp::ClipType::Intersection
                      : kind == OperationKind::Xor          ? cp::ClipType::Xor
                                                            : cp::ClipType::Union;
    const auto paths = cp::BooleanOp (type, cp::FillRule::NonZero, Paths (a), Paths (b), kPrecision);
    if (!Regions (paths, out, error))
        return false;
    Shape parents = a;
    parents.insert (parents.end (), b.begin (), b.end ());
    for (auto& region : out)
        if (!Attribute (region, parents, operation, order, false, error))
            return false;
    return true;
}
bool GeneratedShape (const cp::PathsD& paths, const Shape& parents, Id operation, const std::vector<Id>& order,
                     Shape& out, std::string& error)
{
    if (!Regions (paths, out, error))
        return false;
    for (auto& region : out)
        if (!Attribute (region, parents, operation, order, true, error))
            return false;
    return true;
}
bool SourceGuide (const SourceCurve& curve, Shape& guide, std::string& error)
{
    PlanarMap map;
    if (!BuildPlanarMap ({ curve.curve }, PlanarOptions {}, map, error))
        return false;
    DerivedRegion parent;
    parent.sources = { curve.curve.source };
    std::vector<RegionBoundaryPiece> boundary;
    for (const auto& edge : map.edges) {
        RegionBoundaryPiece piece;
        piece.a = map.vertices[edge.a];
        piece.b = map.vertices[edge.b];
        piece.parameters = edge.pieces;
        for (const auto& parameter : edge.pieces)
            piece.support.push_back ({ parameter.source, curve.segments.at (parameter.piece), parameter.reversed });
        boundary.push_back (std::move (piece));
    }
    parent.boundary.push_back (std::move (boundary));
    guide = { std::move (parent) };
    return true;
}

bool SplitShape (const Shape& surface, const Shape& guides, Id operation, const std::vector<Id>& order, Shape& out,
                 std::string& error)
{
    // H1 is used as a topology engine here. Its curve keys are transient local
    // handles; none of its descriptors/provenance escape this composition.
    // Every emitted boundary is re-attributed against the actual authored
    // surface/guide supports below, exactly as for the clipping bridge.
    std::vector<PlanarCurve> curves;
    Shape parents = surface;
    parents.insert (parents.end (), guides.begin (), guides.end ());
    Id key = 1;
    for (const auto& parent : parents)
        for (const auto& boundary : parent.boundary)
            for (const auto& edge : boundary) {
                if (curves.size () >= 20000) {
                    error = "split exceeds the boundary segment budget";
                    return false;
                }
                PlanarCurve curve;
                curve.source = key++;
                curve.points = { edge.a, edge.b };
                curves.push_back (std::move (curve));
            }
    PlanarMap map;
    if (!BuildPlanarMap (curves, PlanarOptions {}, map, error))
        return false;
    out.clear ();
    for (const auto& face : map.faces) {
        DerivedRegion cell;
        const auto append = [&] (const std::vector<uint32_t>& cycle) {
            std::vector<Vec3> ring;
            for (uint32_t h : cycle)
                ring.push_back (map.vertices[map.halfEdges[h].origin]);
            cell.rings.push_back (std::move (ring));
        };
        append (face.halfEdges);
        for (uint32_t hole : face.holes)
            append (map.cycles[hole].halfEdges);
        Shape clipped;
        if (!Regions (cp::Intersect (Paths (cell), Paths (surface), cp::FillRule::NonZero, kPrecision), clipped, error))
            return false;
        for (auto& region : clipped) {
            if (!Attribute (region, parents, operation, order, false, error))
                return false;
            out.push_back (std::move (region));
        }
    }
    return true;
}

bool PointInside (Vec3 point, const std::vector<std::vector<Vec3>>& rings)
{
    if (rings.empty ())
        return false;
    for (size_t i = 0; i < rings.size (); ++i) {
        cp::PathD path;
        for (auto p : rings[i])
            path.push_back ({ p.x, p.y });
        const auto hit = cp::PointInPolygon (cp::PointD { point.x, point.y }, path);
        if ((i == 0 && hit != cp::PointInPolygonResult::IsInside) ||
            (i != 0 && hit != cp::PointInPolygonResult::IsOutside))
            return false;
    }
    return true;
}
void Diagnostic (LandscapeResult& out, LandscapeDiagnosticCode code, Id op, const std::string& message)
{
    out.diagnostics.push_back ({ code, 0, op, 0, message });
}
} // namespace tapioca::geometry::landscape_detail
