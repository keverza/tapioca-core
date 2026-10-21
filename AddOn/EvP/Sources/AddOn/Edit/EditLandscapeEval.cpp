#include "Edit/EditLandscapeInternal.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>

namespace tapioca::geometry {
namespace edit = geomsrv::edit;
using namespace landscape_detail;
namespace {
const LandscapeSource* Source (const LandscapeSession& session, Id id)
{
    for (const auto& source : session.sources)
        if (source.id == id)
            return &source;
    return nullptr;
}
bool SimpleRing (const PlanarCurve& curve, std::string& error)
{
    if (!curve.closed || curve.points.size () < 3 || curve.points.size () > 20000 ||
        std::any_of (curve.points.begin (), curve.points.end (),
                     [] (Vec3 p) { return !edit::Finite (p) || std::max (std::abs (p.x), std::abs (p.y)) > 1.0e9; })) {
        error = "ring requires 3..20000 finite vertices within +/-1e9 metres";
        return false;
    }
    PlanarMap map;
    if (!BuildPlanarMap ({ curve }, PlanarOptions {}, map, error))
        return false;
    // One bounded face alone also accepts a ring with a retraced spur. Every
    // edge must occur exactly once on that face and have one source interval.
    if (map.faces.size () != 1 || !map.faces[0].holes.empty () || map.faces[0].halfEdges.size () != map.edges.size () ||
        std::any_of (map.edges.begin (), map.edges.end (), [] (const auto& e) { return e.pieces.size () != 1; }) ||
        std::any_of (map.diagnostics.begin (), map.diagnostics.end (),
                     [] (const auto& d) { return d.code == PlanarDiagnosticCode::SelfIntersection; })) {
        error = "ring must be simple, without crossings, holes or retraced edges";
        return false;
    }
    return true;
}
bool PropertyLine (const edit::Document& doc, const std::map<Id, SourceCurve>& curves, LandscapeResult& out)
{
    const auto& session = *doc.landscape;
    if (session.propertyLineSourceId == 0)
        return true;
    const auto* source = Source (session, session.propertyLineSourceId);
    if (!source || source->deleted || source->role != SourceRole::PropertyLine || !curves.count (source->id)) {
        out.error = "PropertyLineOutside: property-line source is absent or invalid";
        Diagnostic (out, LandscapeDiagnosticCode::PropertyLineOutside, 0, out.error);
        return false;
    }
    const auto& curve = curves.at (source->id).curve;
    if (std::any_of (curve.arcAngles.begin (), curve.arcAngles.end (), [] (double a) { return a != 0.0; }) ||
        std::any_of (curve.inHandles.begin (), curve.inHandles.end (), [] (Vec3 h) { return h.x != 0 || h.y != 0; }) ||
        std::any_of (curve.outHandles.begin (), curve.outHandles.end (),
                     [] (Vec3 h) { return h.x != 0 || h.y != 0; }) ||
        std::any_of (edit::Find (doc, source->entityId)->vertices.begin (),
                     edit::Find (doc, source->entityId)->vertices.end (),
                     [] (const auto& v) { return v.radius != 0; })) {
        out.error = "PropertyLineArcUnsupported: property line requires straight segments";
        Diagnostic (out, LandscapeDiagnosticCode::PropertyLineArcUnsupported, 0, out.error);
        return false;
    }
    if (!SimpleRing (curve, out.error)) {
        out.error = "PropertyLineOutside: property line must be a closed simple straight ring: " + out.error;
        Diagnostic (out, LandscapeDiagnosticCode::PropertyLineOutside, 0, out.error);
        return false;
    }
    out.propertyLine = curve.points;
    out.propertyLineRevision = source->revision;
    return true;
}
void AppendHistory (Shape& shape, Id operation, const std::vector<Id>& order)
{
    for (auto& region : shape) {
        std::set<Id> history (region.operations.begin (), region.operations.end ());
        history.insert (operation);
        region.operations.clear ();
        for (Id id : order)
            if (history.count (id))
                region.operations.push_back (id);
        Describe (region);
    }
}
bool UnionStrokeParts (std::vector<Shape> parts, Id operation, const std::vector<Id>& order, Shape& out,
                       std::string& error)
{
    // Pairwise assembly keeps internal capsule caps out of later attribution
    // pools. A dense all-at-once union needlessly hits the work gate on curves.
    while (parts.size () > 1) {
        std::vector<Shape> combined;
        for (size_t i = 0; i < parts.size (); i += 2) {
            if (i + 1 == parts.size ()) {
                combined.push_back (std::move (parts[i]));
                continue;
            }
            Shape joined;
            if (!BooleanShape (OperationKind::Union, parts[i], parts[i + 1], operation, order, joined, error))
                return false;
            combined.push_back (std::move (joined));
        }
        parts = std::move (combined);
    }
    out = parts.empty () ? Shape {} : std::move (parts[0]);
    return true;
}
bool Stroke (const edit::Document& doc, const LandscapeOperation& op, const std::map<Id, SourceCurve>& curves,
             const std::vector<Id>& order, Shape& out, std::string& error)
{
    const auto* source = Source (*doc.landscape, op.inputs.front ());
    if (!source || source->deleted) {
        error = "Stroke requires a source curve/path";
        return false;
    }
    cp::PathsD paths;
    DerivedRegion parent;
    parent.sources = { source->id };
    std::vector<RegionBoundaryPiece> support;
    if (source->pathId) {
        const auto* path = edit::FindPath (doc, source->pathId);
        edit::AlignmentGeometry alignment;
        edit::PathOutline outline;
        if (!path || !edit::BuildAlignment (path->points, alignment, error))
            return false;
        if (std::ceil (alignment.length / 0.25) + alignment.elements.size () + 3 * path->profile.size () +
                2 * path->sections.size () >
            9000) {
            error = "landscape path outline exceeds its sample budget";
            return false;
        }
        if (!edit::Outline (*path, 0.25, outline, error))
            return false;
        cp::PathD ring;
        for (auto p : outline.left)
            ring.push_back ({ p.x, p.y });
        for (auto i = outline.right.rbegin (); i != outline.right.rend (); ++i)
            ring.push_back ({ i->x, i->y });
        if (cp::Area (ring) < 0)
            std::reverse (ring.begin (), ring.end ());
        paths.push_back (std::move (ring));
        // The existing path alignment/profile/section is authoritative; width
        // belongs to its sections rather than a parallel stroke-width model.
        for (size_t i = 1; i < path->points.size (); ++i)
            support.push_back ({ path->points[i - 1].p,
                                 path->points[i].p,
                                 { { source->id, path->points[i - 1].id, false } },
                                 {},
                                 true });
    }
    else {
        const auto& curve = curves.at (source->id);
        // Refuse before Clipper allocates a cap whose sagitta alone needs more
        // vertices than the entire stroke budget (including rounded 1-ratio).
        const double angle = std::acos (1.0 - std::min (1.0, 0.001 / (op.width * 0.5)));
        if (!(angle > 0.0) || std::ceil (3.14159265358979323846 / angle) + 2 > 20000) {
            error = "landscape stroke exceeds the 20000 capsule boundary segment budget";
            return false;
        }
        PlanarMap map;
        if (!BuildPlanarMap ({ curve.curve }, PlanarOptions {}, map, error))
            return false;
        // Retain flattened arcs/cubics and authored segment support. A round
        // stroke is the union of segment capsules, not one generated boundary
        // that can be attributed to every segment of the parent curve.
        struct Piece {
            uint32_t ordinal;
            double start;
            Vec3 a, b;
            std::vector<Id> segments;
        };
        std::vector<Piece> pieces;
        for (const auto& edge : map.edges)
            for (const auto& p : edge.pieces) {
                const Vec3 a = map.vertices[p.reversed ? edge.b : edge.a],
                           b = map.vertices[p.reversed ? edge.a : edge.b];
                pieces.push_back ({ p.piece, p.sourceStart, a, b, curve.segments.at (p.piece) });
            }
        std::sort (pieces.begin (), pieces.end (), [] (const auto& a, const auto& b) {
            return std::tie (a.ordinal, a.start) < std::tie (b.ordinal, b.start);
        });
        std::vector<Shape> capsules;
        size_t capsuleEdges = 0;
        for (const auto& piece : pieces) {
            const cp::PathD line { { piece.a.x, piece.a.y }, { piece.b.x, piece.b.y } };
            const auto capsulePaths = cp::InflatePaths ({ line }, op.width * 0.5, cp::JoinType::Round,
                                                        cp::EndType::Round, 2.0, kPrecision, 0.001);
            for (const auto& ring : capsulePaths)
                capsuleEdges += ring.size ();
            if (capsuleEdges > 20000) {
                error = "landscape stroke exceeds the 20000 capsule boundary segment budget";
                return false;
            }
            DerivedRegion segment;
            segment.sources = { source->id };
            std::vector<BoundaryRef> support;
            for (Id id : piece.segments)
                support.push_back ({ source->id, id, false });
            segment.boundary = { { { piece.a, piece.b, support, {}, true } } };
            Shape generated;
            if (!GeneratedShape (capsulePaths, { segment }, op.id, order, generated, error))
                return false;
            capsules.push_back (std::move (generated));
        }
        // The existing Boolean bridge reconstructs boundary contributors from
        // the capsule edges that actually survive, including at joins/crossings.
        return UnionStrokeParts (std::move (capsules), op.id, order, out, error);
    }
    parent.boundary.push_back (std::move (support));
    return GeneratedShape (paths, { parent }, op.id, order, out, error);
}
bool Evaluate (const edit::Document& doc, LandscapeResult& out)
{
    out.revision = doc.revision;
    if (!doc.landscape)
        return true;
    if (!ValidateLandscape (doc, out.error))
        return false;
    const auto& session = *doc.landscape;
    std::vector<Id> order;
    if (!ValidateLandscapeDag (session, order, out.error))
        return false;
    std::map<Id, SourceCurve> curves;
    std::vector<PlanarCurve> input;
    std::map<Id, Shape> values;
    std::set<Id> unavailable;
    for (const auto& source : session.sources) {
        if (source.deleted) {
            unavailable.insert (source.id);
            continue;
        }
        if (source.pathId) {
            values[source.id] = {};
            continue;
        }
        SourceCurve curve;
        if (!ReadSourceCurve (doc, source, curve, out.error))
            return false;
        input.push_back (curve.curve);
        curves[source.id] = std::move (curve);
    }
    if (!PropertyLine (doc, curves, out))
        return false;
    PlanarMap arrangement;
    if (!BuildPlanarMap (input, PlanarOptions {}, arrangement, out.error)) {
        out.planarDiagnostics = std::move (arrangement.diagnostics);
        return false;
    }
    out.planarDiagnostics = arrangement.diagnostics;
    for (const auto& face : arrangement.faces)
        out.faces.push_back (FaceRegion (arrangement, face, curves));
    for (const auto& [id, curve] : curves) {
        Shape shape;
        if (curve.curve.closed) {
            PlanarMap map;
            if (!BuildPlanarMap ({ curve.curve }, PlanarOptions {}, map, out.error))
                return false;
            for (const auto& face : map.faces)
                shape.push_back (FaceRegion (map, face, curves));
        }
        values[id] = std::move (shape);
    }
    for (Id id : order) {
        const auto opIt = std::find_if (session.operations.begin (), session.operations.end (),
                                        [=] (const auto& o) { return o.id == id; });
        const auto& op = *opIt;
        if (std::any_of (op.inputs.begin (), op.inputs.end (),
                         [&] (Id inputId) { return !values.count (inputId) || unavailable.count (inputId); })) {
            unavailable.insert (id);
            values[id] = {};
            Diagnostic (out, LandscapeDiagnosticCode::OrphanedOperation, id,
                        "operation retains an absent/deleted input");
            continue;
        }
        Shape shape = op.inputs.empty () ? Shape {} : values.at (op.inputs.front ());
        if (!op.enabled) {
            values[id] = std::move (shape);
            continue;
        }
        if (op.kind == OperationKind::Stroke) {
            if (!Stroke (doc, op, curves, order, shape, out.error))
                return false;
        }
        else if (op.kind == OperationKind::RegionSelect && !op.faces.empty ()) {
            shape.clear ();
            for (const auto& selector : op.faces) {
                const DerivedRegion* match = nullptr;
                for (const auto& face : out.faces)
                    if (face.descriptor == selector) {
                        if (match) {
                            out.error = "ProvenanceUnattributable: ambiguous semantic face selector";
                            Diagnostic (out, LandscapeDiagnosticCode::AmbiguousFace, id, out.error);
                            return false;
                        }
                        match = &face;
                    }
                if (!match) {
                    out.error = "ProvenanceUnattributable: selected face changed topology; select it explicitly again";
                    Diagnostic (out, LandscapeDiagnosticCode::AmbiguousFace, id, out.error);
                    return false;
                }
                shape.push_back (*match);
            }
            AppendHistory (shape, id, order);
        }
        else if (op.kind == OperationKind::Offset) {
            Shape offset;
            // Offset each disconnected attributed parent separately, then
            // assemble with verified contributors; never tag all inputs onto
            // every disconnected output.
            for (const auto& parent : shape) {
                const auto paths = cp::InflatePaths (Paths (parent), op.offset, cp::JoinType::Round,
                                                     cp::EndType::Polygon, 2.0, kPrecision, 0.001);
                Shape generated;
                if (!GeneratedShape (paths, { parent }, id, order, generated, out.error))
                    return false;
                Shape combined;
                if (!BooleanShape (OperationKind::Union, offset, generated, id, order, combined, out.error))
                    return false;
                offset = std::move (combined);
            }
            shape = std::move (offset);
            if (shape.empty ())
                Diagnostic (out, LandscapeDiagnosticCode::OffsetCollapsed, id, "offset consumed the footprint");
        }
        else if (op.kind == OperationKind::MaterialMask || op.kind == OperationKind::GradingLimit ||
                 op.kind == OperationKind::ProtectedZone) {
            // Constraints retain the footprint; only affected regions acquire
            // the mask's actual sources/history. H3/H11 consume these masks.
            // A grading limit or protected zone acts on terrain, which only a
            // property line bounds: M1 still evaluates it, flagged.
            if (op.kind != OperationKind::MaterialMask && out.propertyLine.empty ())
                Diagnostic (out, LandscapeDiagnosticCode::PropertyLineOutside, id,
                            "terrain constraint has no property line to bound it");
            bool reached = false;
            for (auto& region : shape) {
                std::set<Id> sources (region.sources.begin (), region.sources.end ());
                std::set<Id> history (region.operations.begin (), region.operations.end ());
                for (size_t i = 1; i < op.inputs.size (); ++i)
                    for (const auto& mask : values.at (op.inputs[i]))
                        if (std::abs (cp::Area (
                                cp::Intersect (Paths (region), Paths (mask), cp::FillRule::NonZero, kPrecision))) > 0) {
                            reached = true;
                            sources.insert (mask.sources.begin (), mask.sources.end ());
                            history.insert (mask.operations.begin (), mask.operations.end ());
                        }
                region.sources.assign (sources.begin (), sources.end ());
                region.operations.clear ();
                for (Id parent : order)
                    if (history.count (parent))
                        region.operations.push_back (parent);
            }
            // The footprint survives; a mask that clips none of it constrains nothing.
            if (!reached && !shape.empty ())
                Diagnostic (out, LandscapeDiagnosticCode::ClipEmpty, id, "constraint mask reaches no region");
            AppendHistory (shape, id, order);
        }
        else if (op.kind == OperationKind::RegionSelect)
            AppendHistory (shape, id, order);
        else
            for (size_t i = 1; i < op.inputs.size (); ++i) {
                Shape combined;
                if (op.kind == OperationKind::Split) {
                    Shape guides = values.at (op.inputs[i]);
                    if (guides.empty () && curves.count (op.inputs[i]) &&
                        !SourceGuide (curves.at (op.inputs[i]), guides, out.error))
                        return false;
                    if (!SplitShape (shape, guides, id, order, combined, out.error))
                        return false;
                }
                else if (!BooleanShape (op.kind, shape, values.at (op.inputs[i]), id, order, combined, out.error))
                    return false;
                shape = std::move (combined);
            }
        if (shape.empty () && !(op.kind == OperationKind::RegionSelect && op.inputs.empty ()))
            Diagnostic (out, LandscapeDiagnosticCode::ClipEmpty, id, "operation produced no footprint");
        values[id] = std::move (shape);
    }
    for (const auto& object : session.objects) {
        if (unavailable.count (object.footprintOperation))
            continue;
        for (auto region : values.at (object.footprintOperation)) {
            region.objectId = object.id;
            region.material = object.baseMaterial;
            Describe (region);
            for (const auto& identity : session.regions)
                if (identity.active && identity.descriptor == region.descriptor) {
                    if (region.regionId) {
                        out.error = "ProvenanceConflict: duplicate active semantic region";
                        return false;
                    }
                    region.regionId = identity.id;
                    region.predecessorRegions = identity.predecessors;
                }
            const MaterialAssignment* chosen = nullptr;
            for (const auto& assignment : session.assignments)
                if (assignment.valid && assignment.mask == 0 && assignment.region == region.regionId &&
                    (!chosen || assignment.priority > chosen->priority))
                    chosen = &assignment;
            // Only the winning rank can conflict. A lower-rank tie must not
            // refuse a higher-rank override based on catalog iteration order.
            if (chosen) {
                for (const auto& assignment : session.assignments)
                    if (assignment.valid && assignment.mask == 0 && assignment.region == region.regionId &&
                        assignment.priority == chosen->priority && !(assignment.material == chosen->material)) {
                        out.error = "ProvenanceConflict: equally ranked region assignments require explicit resolution";
                        Diagnostic (out, LandscapeDiagnosticCode::ProvenanceConflict, 0, out.error);
                        out.diagnostics.back ().object = object.id;
                        return false;
                    }
                region.material = chosen->material;
            }
            out.regions.push_back (std::move (region));
        }
    }
    return true;
}
// Regions of the identities the before-state emits, keyed by region ID. An
// identity the before-state does not emit has no footprint and cannot be proven.
std::map<Id, DerivedRegion> BeforeRegions (const edit::Document* before, const std::vector<RegionIdentity>& previous)
{
    std::map<Id, DerivedRegion> regions;
    LandscapeResult evaluated;
    if (!before || !before->landscape || before->landscape->regions != previous ||
        !EvaluateLandscape (*before, evaluated))
        return regions;
    for (auto& region : evaluated.regions)
        if (region.regionId)
            regions[region.regionId] = std::move (region);
    return regions;
}
// Shared authored segments are not lineage: disconnected siblings share them.
// Area flows only where footprints overlap by more than boundary contact, so an
// overlap whose mean width (2 area / perimeter) is within tolerance is a touch.
bool Overlaps (const cp::PathsD& before, const cp::PathsD& after, double tolerance)
{
    const auto common = cp::Intersect (before, after, cp::FillRule::NonZero, kPrecision);
    double area = 0.0, perimeter = 0.0;
    for (const auto& ring : common) {
        area += cp::Area (ring);
        for (size_t i = 0; i < ring.size (); ++i) {
            const auto& a = ring[i];
            const auto& b = ring[(i + 1) % ring.size ()];
            perimeter += std::hypot (b.x - a.x, b.y - a.y);
        }
    }
    return perimeter > 0.0 && 2.0 * std::abs (area) > tolerance * perimeter;
}
// One source's current flattened chords by H1 piece ordinal, each spanning the
// analytic source parameters u0 -> u1 from a to b.
struct SourceChords {
    struct Chord {
        double u0, u1;
        Vec3 a, b;
    };
    std::vector<std::vector<Id>> segments;
    std::map<uint32_t, std::vector<Chord>> pieces;
};
bool ReadChords (const edit::Document& doc, const LandscapeSource& source, SourceChords& out)
{
    SourceCurve curve;
    PlanarMap map;
    std::string error;
    if (source.deleted || source.pathId || !ReadSourceCurve (doc, source, curve, error) ||
        !BuildPlanarMap ({ curve.curve }, PlanarOptions {}, map, error))
        return false;
    out.segments = std::move (curve.segments);
    for (const auto& edge : map.edges)
        for (const auto& p : edge.pieces)
            out.pieces[p.piece].push_back ({ p.sourceStart, p.sourceEnd, map.vertices[p.reversed ? edge.b : edge.a],
                                             map.vertices[p.reversed ? edge.a : edge.b] });
    return true;
}
bool At (const SourceChords& chords, uint32_t piece, double u, Vec3& point)
{
    const auto found = chords.pieces.find (piece);
    if (found == chords.pieces.end ())
        return false;
    for (const auto& chord : found->second)
        if (u >= std::min (chord.u0, chord.u1) - 1.0e-12 && u <= std::max (chord.u0, chord.u1) + 1.0e-12) {
            const double t = chord.u1 == chord.u0 ? 0.0 : (u - chord.u0) / (chord.u1 - chord.u0);
            point = edit::Lerp (chord.a, chord.b, std::clamp (t, 0.0, 1.0));
            return true;
        }
    return false;
}
// An old fragment re-evaluated on the current revision of its authored sources.
// Each boundary piece keeps its source subdomain and takes that subdomain's
// current position, so a dragged vertex carries the fragment with it and a moved
// cutter carries its hole away. Generated pieces, and pieces whose ordinal no
// longer names the same authored segment, keep their old coordinates.
cp::PathsD Transport (const DerivedRegion& old, const std::function<const SourceChords*(Id)>& chords)
{
    cp::PathsD outer, holes;
    for (size_t r = 0; r < old.boundary.size (); ++r) {
        cp::PathD path;
        for (const auto& piece : old.boundary[r]) {
            Vec3 a = piece.a, b = piece.b;
            for (const auto& p : piece.parameters) {
                const auto* table = chords (p.source);
                if (!table || p.piece >= table->segments.size () ||
                    std::none_of (piece.support.begin (), piece.support.end (), [&] (const BoundaryRef& ref) {
                        const auto& now = table->segments[p.piece];
                        return ref.source == p.source &&
                               std::find (now.begin (), now.end (), ref.segment) != now.end ();
                    }))
                    continue;
                Vec3 ta, tb;
                if (At (*table, p.piece, p.reversed ? p.sourceEnd : p.sourceStart, ta) &&
                    At (*table, p.piece, p.reversed ? p.sourceStart : p.sourceEnd, tb)) {
                    a = ta;
                    b = tb;
                    break;
                }
            }
            path.push_back ({ a.x, a.y });
            path.push_back ({ b.x, b.y });
        }
        (r == 0 ? outer : holes).push_back (std::move (path));
    }
    // A dragged vertex can fold the outer ring into lobes of either winding.
    return cp::Difference (outer, holes, cp::FillRule::NonZero, kPrecision);
}
void RefreshSources (edit::Document& doc)
{
    if (!doc.landscape)
        return;
    for (auto& source : doc.landscape->sources) {
        if (source.deleted)
            continue;
        uint64_t hash = 14695981039346656037ull;
        const auto word = [&] (uint64_t value) {
            for (int i = 0; i < 8; ++i) {
                hash ^= value & 255;
                hash *= 1099511628211ull;
                value >>= 8;
            }
        };
        const auto number = [&] (double value) { word (std::bit_cast<uint64_t> (value)); };
        const auto point = [&] (Vec3 p) {
            number (p.x);
            number (p.y);
            number (p.z);
        };
        for (double angle : source.arcAngles)
            number (angle);
        if (const auto* entity = edit::Find (doc, source.entityId)) {
            word (entity->closed);
            for (const auto& v : entity->vertices) {
                word (v.id);
                point (v.p);
                point (v.in);
                point (v.out);
                number (v.radius);
            }
            for (const auto& s : entity->segments)
                word (s.id);
        }
        if (const auto* path = edit::FindPath (doc, source.pathId)) {
            for (const auto& p : path->points) {
                word (p.id);
                point (p.p);
                number (p.radius);
            }
            for (const auto& p : path->profile) {
                word (p.id);
                number (p.station);
                number (p.z);
                number (p.curve);
            }
            for (const auto& s : path->sections) {
                word (s.id);
                number (s.station);
                number (s.widthLeft);
                number (s.widthRight);
                number (s.crossfall);
                word (uint64_t (s.fall));
                number (s.shoulder);
                number (s.cutSlope);
                number (s.fillSlope);
            }
            number (path->edgeRadius);
        }
        source.revision = hash;
    }
}
// A kernel refusal names its code first ("Code: why"). Sites without a result
// (Boolean attribution) and product diagnostics (AmbiguousFace) still owe the
// normative typed record, so a refusal never carries only a string.
void TypeRefusal (LandscapeResult& result)
{
    result.regions.clear ();
    for (int i = 0; i <= int (LandscapeDiagnosticCode::ClipEmpty); ++i) {
        const auto code = LandscapeDiagnosticCode (i);
        if (result.error.rfind (std::string (LandscapeDiagnosticCodeName (code)) + ":", 0) == 0 &&
            std::none_of (result.diagnostics.begin (), result.diagnostics.end (),
                          [=] (const auto& d) { return d.code == code; }))
            Diagnostic (result, code, 0, result.error);
    }
}
} // namespace

bool EvaluateLandscape (const edit::Document& doc, LandscapeResult& result)
{
    LandscapeResult candidate;
    try {
        if (!Evaluate (doc, candidate)) {
            TypeRefusal (candidate);
            result = std::move (candidate);
            return false;
        }
    }
    catch (const std::exception& e) {
        candidate.error = std::string ("landscape evaluation refused: ") + e.what ();
        candidate.regions.clear ();
        result = std::move (candidate);
        return false;
    }
    result = std::move (candidate);
    return true;
}

static bool Rebuild (edit::Document& doc, LandscapeResult& result, std::string& error, const edit::Document* before);

bool RebuildLandscape (edit::Document& doc, LandscapeResult& result, std::string& error, const edit::Document* before)
{
    if (Rebuild (doc, result, error, before))
        return true;
    if (result.error.empty ())
        result.error = error;
    TypeRefusal (result);
    return false;
}

static bool Rebuild (edit::Document& doc, LandscapeResult& result, std::string& error, const edit::Document* before)
{
    error.clear ();
    auto candidate = doc;
    RefreshSources (candidate);
    if (!EvaluateLandscape (candidate, result)) {
        error = result.error;
        return false;
    }
    if (!doc.landscape)
        return true;
    auto& session = *candidate.landscape;
    const auto previous = session.regions;
    // Lineage needs the before-state and current chords only for new regions.
    std::optional<std::map<Id, DerivedRegion>> beforeRegions;
    std::map<Id, std::optional<SourceChords>> chordCache;
    const auto chords = [&] (Id id) -> const SourceChords* {
        auto found = chordCache.find (id);
        if (found == chordCache.end ()) {
            found = chordCache.emplace (id, std::nullopt).first;
            for (const auto& source : session.sources)
                if (SourceChords table; source.id == id && ReadChords (candidate, source, table))
                    found->second = std::move (table);
        }
        return found->second ? &*found->second : nullptr;
    };
    for (auto& region : session.regions)
        region.active = false;
    for (size_t i = 0; i < result.regions.size (); ++i) {
        auto& region = result.regions[i];
        for (size_t j = 0; j < i; ++j)
            if (region.descriptor == result.regions[j].descriptor) {
                error = "ProvenanceUnattributable: disconnected outputs share an ambiguous semantic region descriptor";
                Diagnostic (result, LandscapeDiagnosticCode::AmbiguousFace, 0, error);
                result.error = error;
                return false;
            }
        if (region.regionId) {
            for (auto& identity : session.regions)
                if (identity.id == region.regionId)
                    identity.active = true;
            continue;
        }
        if (candidate.nextId == 0 || candidate.nextId == std::numeric_limits<Id>::max ()) {
            error = "landscape authored ID allocator exhausted";
            return false;
        }
        region.regionId = edit::NewId (candidate);
        std::set<Id> predecessors;
        const auto paths = Paths (region);
        for (const auto& identity : previous) {
            if (!identity.active || identity.descriptor.object != region.descriptor.object)
                continue;
            if (!beforeRegions)
                beforeRegions = BeforeRegions (before, previous);
            const auto old = beforeRegions->find (identity.id);
            if (old == beforeRegions->end ()) {
                error = "ProvenanceUnattributable: region lineage needs the before-state that carries this identity "
                        "catalog";
                Diagnostic (result, LandscapeDiagnosticCode::ProvenanceUnattributable, 0, error);
                result.error = error;
                return false;
            }
            if (Overlaps (Transport (old->second, chords), paths, result.boundaryTolerance))
                predecessors.insert (identity.id);
        }
        region.predecessorRegions.assign (predecessors.begin (), predecessors.end ());
        session.regions.push_back ({ region.regionId, region.descriptor, region.predecessorRegions, true });
    }
    for (auto& assignment : session.assignments) {
        const auto region = std::find_if (session.regions.begin (), session.regions.end (),
                                          [&] (const auto& r) { return r.id == assignment.region; });
        if (region != session.regions.end () && !region->active) {
            assignment.valid = false;
            Diagnostic (result, LandscapeDiagnosticCode::ProvenanceUnattributable, 0,
                        "region assignment invalidated by topology change; explicit reassignment required");
        }
    }
    if (!ValidateLandscape (candidate, error))
        return false;
    doc = std::move (candidate);
    return true;
}

const DerivedRegion* LandscapeFaceAt (const LandscapeResult& result, Vec3 point)
{
    const DerivedRegion* best = nullptr;
    for (const auto& face : result.faces)
        if (PointInside (point, face.rings) && (!best || face.area < best->area))
            best = &face;
    return best;
}

bool ValidateLandscapeTerrainEdit (const edit::Document& doc, const std::vector<Vec3>& terrainBoundary,
                                   std::string& error)
{
    LandscapeResult result;
    if (!EvaluateLandscape (doc, result)) {
        error = result.error;
        return false;
    }
    if (result.propertyLine.empty ()) {
        error = "PropertyLineOutside: terrain edit requires a property line";
        return false;
    }
    PlanarCurve footprint;
    footprint.source = 1; // Request-local locator; no authored identity is allocated.
    footprint.closed = true;
    footprint.points = terrainBoundary;
    if (!SimpleRing (footprint, error)) {
        error = "PropertyLineOutside: terrain footprint is invalid: " + error;
        return false;
    }
    DerivedRegion property, terrain;
    property.rings = { result.propertyLine };
    terrain.rings = { terrainBoundary };
    if (std::abs (cp::Area (cp::Difference (Paths (property), Paths (terrain), cp::FillRule::NonZero, kPrecision))) >
        0) {
        error = "PropertyLineOutside: property line lies outside the terrain footprint";
        return false;
    }
    error.clear ();
    return true;
}
} // namespace tapioca::geometry
