#include "ExposedSurfaceInternal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <numeric>

namespace geomsrv::exposed::detail {
namespace {
bool Fail (Refusal& refusal, const std::string& reason, RefusalCode code = RefusalCode::InvalidQuery)
{
    refusal.code = code;
    refusal.message = reason;
    return false;
}

std::string GuidKey (std::string guid)
{
    if (guid.size () == 38 && guid.front () == '{' && guid.back () == '}')
        guid = guid.substr (1, 36);
    std::transform (guid.begin (), guid.end (), guid.begin (),
                    [] (unsigned char c) { return static_cast<char> (std::toupper (c)); });
    return guid;
}

bool ValidateMesh (const Mesh& mesh, Refusal& refusal)
{
    const size_t count = mesh.TriangleCount ();
    if (mesh.guid.empty () || mesh.vertices.size () % 3 || mesh.triangles.size () % 3 || !count ||
        mesh.triSourceBody.size () != count || mesh.triSourcePgon.size () != count)
        return Fail (refusal, "Missing/misaligned geometry, identity or source provenance.");
    for (double value : mesh.vertices)
        if (!std::isfinite (value) || std::abs (value) > 1e9)
            return Fail (refusal, "Invalid/unsupported exposed-surface coordinate.");
    for (uint32_t index : mesh.triangles)
        if (index >= mesh.VertexCount ())
            return Fail (refusal, "Exposed-surface triangle references a missing vertex.");
    for (size_t i = 0; i < count; ++i)
        if (mesh.triSourceBody[i] <= 0 || mesh.triSourcePgon[i] <= 0)
            return Fail (refusal, "Source body/polygon identifiers must be ModelerAPI 1-based.");
    return true;
}

struct Triangle {
    std::array<V, 3> points;
    V normal;
    Aabb bounds;
    size_t sourceTriangle = 0;
};

V Subtract (const V& a, const V& b)
{
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}

V Interpolate (const V& a, const V& b, double t)
{
    return { a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z) };
}

double Length (const V& value)
{
    return std::sqrt (engine::Dot (value, value));
}

void AddUnique (std::vector<V>& points, const V& point)
{
    if (std::none_of (points.begin (), points.end (), [&] (const V& existing) {
            return Length (Subtract (existing, point)) <= surfacecoverage::kPlaneTolerance;
        }))
        points.push_back (point);
}

std::vector<V> PlaneContacts (const Triangle& triangle, const V& normal, const V& origin)
{
    std::vector<V> contacts;
    for (size_t i = 0; i < 3; ++i) {
        const V& a = triangle.points[i];
        const V& b = triangle.points[(i + 1) % 3];
        const double da = engine::Dot (Subtract (a, origin), normal), db = engine::Dot (Subtract (b, origin), normal);
        if (std::abs (da) <= surfacecoverage::kPlaneTolerance)
            AddUnique (contacts, a);
        if ((da > surfacecoverage::kPlaneTolerance && db < -surfacecoverage::kPlaneTolerance) ||
            (da < -surfacecoverage::kPlaneTolerance && db > surfacecoverage::kPlaneTolerance))
            AddUnique (contacts, Interpolate (a, b, da / (da - db)));
    }
    return contacts;
}

bool OnSegment (const V& point, const V& a, const V& b)
{
    const V edge = Subtract (b, a), delta = Subtract (point, a);
    const double length = Length (edge);
    if (length <= surfacecoverage::kPlaneTolerance)
        return Length (delta) <= surfacecoverage::kPlaneTolerance;
    const double t = engine::Dot (delta, edge) / (length * length);
    if (t < -surfacecoverage::kPlaneTolerance / length || t > 1 + surfacecoverage::kPlaneTolerance / length)
        return false;
    return Length (Subtract (delta, { t * edge.x, t * edge.y, t * edge.z })) <= surfacecoverage::kPlaneTolerance;
}

bool OnAnyEdge (const std::array<V, 2>& segment, const Triangle& triangle)
{
    for (size_t i = 0; i < 3; ++i)
        if (OnSegment (segment[0], triangle.points[i], triangle.points[(i + 1) % 3]) &&
            OnSegment (segment[1], triangle.points[i], triangle.points[(i + 1) % 3]))
            return true;
    return false;
}

std::optional<std::array<V, 2>> IntersectionSegment (const Triangle& a, const Triangle& b)
{
    auto contacts = PlaneContacts (a, b.normal, b.points[0]);
    if (contacts.size () < 2)
        return std::nullopt;
    size_t first = 0, second = 1;
    double longest = 0;
    for (size_t i = 0; i < contacts.size (); ++i)
        for (size_t j = i + 1; j < contacts.size (); ++j) {
            const double length = Length (Subtract (contacts[j], contacts[i]));
            if (length > longest) {
                longest = length;
                first = i;
                second = j;
            }
        }
    if (longest <= surfacecoverage::kPlaneTolerance)
        return std::nullopt;

    const V& start = contacts[first];
    const V& end = contacts[second];
    double low = 0, high = 1;
    for (size_t i = 0; i < 3; ++i) {
        const V& origin = b.points[i];
        const V edge = Subtract (b.points[(i + 1) % 3], origin);
        const double edgeLength = Length (edge);
        if (edgeLength <= surfacecoverage::kPlaneTolerance)
            continue;
        const auto side = [&] (const V& point) {
            return engine::Dot (engine::Cross (edge, Subtract (point, origin)), b.normal) / edgeLength;
        };
        const double before = side (start), change = side (end) - before;
        if (std::abs (change) <= 1e-15) {
            if (before < -surfacecoverage::kPlaneTolerance)
                return std::nullopt;
            continue;
        }
        const double crossing = -before / change;
        if (change > 0)
            low = std::max (low, crossing);
        else
            high = std::min (high, crossing);
    }
    if (high - low <= surfacecoverage::kPlaneTolerance / longest)
        return std::nullopt;
    return std::array<V, 2> { Interpolate (start, end, low), Interpolate (start, end, high) };
}

bool Coplanar (const Triangle& a, const Triangle& b)
{
    if (std::abs (engine::Dot (a.normal, b.normal)) < 1 - 1e-10)
        return false;
    for (const auto& point : b.points) {
        const V delta { point.x - a.points[0].x, point.y - a.points[0].y, point.z - a.points[0].z };
        if (std::abs (engine::Dot (delta, a.normal)) > surfacecoverage::kPlaneTolerance)
            return false;
    }
    return true;
}

size_t ProjectionAxis (const V& normal)
{
    const V magnitude { std::abs (normal.x), std::abs (normal.y), std::abs (normal.z) };
    return magnitude.x > magnitude.y ? (magnitude.x > magnitude.z ? 0 : 2) : (magnitude.y > magnitude.z ? 1 : 2);
}

double Side (const V& a, const V& b, const V& point, size_t drop)
{
    const double av[] { a.x, a.y, a.z }, bv[] { b.x, b.y, b.z }, pv[] { point.x, point.y, point.z };
    const size_t first = (drop + 1) % 3, second = (drop + 2) % 3;
    const double dx = bv[first] - av[first], dy = bv[second] - av[second];
    const double length = std::sqrt (dx * dx + dy * dy);
    return length > 0 ? (dx * (pv[second] - av[second]) - dy * (pv[first] - av[first])) / length : 0;
}

bool StrictlyInside (const V& point, const Triangle& triangle, size_t drop)
{
    for (size_t i = 0; i < 3; ++i) {
        const V& a = triangle.points[i];
        const V& b = triangle.points[(i + 1) % 3];
        const double side = Side (a, b, point, drop);
        if (std::abs (side) <= surfacecoverage::kPlaneTolerance ||
            side * Side (a, b, triangle.points[(i + 2) % 3], drop) <= 0)
            return false;
    }
    return true;
}

bool ProperCross (const V& a0, const V& a1, const V& b0, const V& b1, size_t drop)
{
    const auto opposite = [&] (double a, double b) {
        return (a > surfacecoverage::kPlaneTolerance && b < -surfacecoverage::kPlaneTolerance) ||
               (a < -surfacecoverage::kPlaneTolerance && b > surfacecoverage::kPlaneTolerance);
    };
    return opposite (Side (a0, a1, b0, drop), Side (a0, a1, b1, drop)) &&
           opposite (Side (b0, b1, a0, drop), Side (b0, b1, a1, drop));
}

V Centre (const Triangle& triangle)
{
    return { (triangle.points[0].x + triangle.points[1].x + triangle.points[2].x) / 3,
             (triangle.points[0].y + triangle.points[1].y + triangle.points[2].y) / 3,
             (triangle.points[0].z + triangle.points[1].z + triangle.points[2].z) / 3 };
}

bool CoplanarOverlap (const Triangle& a, const Triangle& b)
{
    const size_t drop = ProjectionAxis (a.normal);
    for (const auto& point : a.points)
        if (StrictlyInside (point, b, drop))
            return true;
    for (const auto& point : b.points)
        if (StrictlyInside (point, a, drop))
            return true;
    for (size_t i = 0; i < 3; ++i)
        for (size_t j = 0; j < 3; ++j)
            if (ProperCross (a.points[i], a.points[(i + 1) % 3], b.points[j], b.points[(j + 1) % 3], drop))
                return true;
    // Identical/contained triangles need no proper edge crossing or strict
    // vertex. Original-coordinate predicates avoid grid-induced T-junction gaps.
    return StrictlyInside (Centre (a), b, drop) || StrictlyInside (Centre (b), a, drop);
}

bool Crosses (const Triangle& a, const Triangle& b)
{
    if (Coplanar (a, b))
        return CoplanarOverlap (a, b);
    const auto segment = IntersectionSegment (a, b);
    // Point contacts do not cross. A positive segment is valid only when it is
    // a subset of existing edges on both triangles (adjacency/T-junction).
    return segment && !(OnAnyEdge (*segment, a) && OnAnyEdge (*segment, b));
}

bool SelfIntersectionFree (std::vector<Triangle> triangles, Work& work, Refusal& refusal)
{
    std::sort (triangles.begin (), triangles.end (),
               [] (const Triangle& a, const Triangle& b) { return a.bounds.mn[0] < b.bounds.mn[0]; });
    for (size_t i = 0; i < triangles.size (); ++i) {
        for (size_t j = i + 1;
             j < triangles.size () &&
             triangles[j].bounds.mn[0] <= triangles[i].bounds.mx[0] + surfacecoverage::kPlaneTolerance;
             ++j) {
            if (!work.Check (1, refusal))
                return false;
            if (!Overlaps (triangles[i].bounds, triangles[j].bounds))
                continue;
            if (Crosses (triangles[i], triangles[j]))
                return Fail (refusal,
                             "Cover body has self-intersecting shells or overlapping surface triangles (" +
                                 std::to_string (triangles[i].sourceTriangle) + ", " +
                                 std::to_string (triangles[j].sourceTriangle) + ").",
                             RefusalCode::InvalidCover);
        }
    }
    return true;
}

bool ClosedSolid (const Mesh& mesh, Work& work, Refusal& refusal)
{
    using Key = std::array<int64_t, 3>;
    struct Incidence {
        size_t count = 0;
        int balance = 0;
    };
    std::map<std::pair<Key, Key>, Incidence> edges;
    const V origin = Point (mesh.vertices, 0);
    std::vector<Key> keys;
    for (size_t i = 0; i < mesh.VertexCount (); ++i) {
        const V p = Point (mesh.vertices, i);
        keys.push_back ({ std::llround ((p.x - origin.x) / surfacecoverage::kPlaneTolerance),
                          std::llround ((p.y - origin.y) / surfacecoverage::kPlaneTolerance),
                          std::llround ((p.z - origin.z) / surfacecoverage::kPlaneTolerance) });
    }
    double volume6 = 0;
    std::vector<Triangle> triangles;
    triangles.reserve (mesh.TriangleCount ());
    for (size_t i = 0; i < mesh.triangles.size (); i += 3) {
        if (!work.Check (1, refusal))
            return false;
        surfacecoverage::Frame frame;
        const V a = Point (mesh.vertices, mesh.triangles[i]), b = Point (mesh.vertices, mesh.triangles[i + 1]),
                c = Point (mesh.vertices, mesh.triangles[i + 2]);
        if (!surfacecoverage::MakeFrame (a, b, c, frame))
            return Fail (refusal, "Cover body contains a degenerate triangle.");
        Triangle triangle;
        for (size_t corner = 0; corner < 3; ++corner) {
            const V point = Point (mesh.vertices, mesh.triangles[i + corner]);
            triangle.points[corner] = { point.x - origin.x, point.y - origin.y, point.z - origin.z };
            triangle.bounds.Expand (point.x, point.y, point.z);
        }
        triangle.normal = frame.normal;
        triangle.sourceTriangle = i / 3;
        triangles.push_back (std::move (triangle));
        const V da { a.x - origin.x, a.y - origin.y, a.z - origin.z },
            db { b.x - origin.x, b.y - origin.y, b.z - origin.z },
            dc { c.x - origin.x, c.y - origin.y, c.z - origin.z };
        volume6 += engine::Dot (da, engine::Cross (db, dc));
        for (size_t j = 0; j < 3; ++j) {
            const Key& first = keys[mesh.triangles[i + j]];
            const Key& second = keys[mesh.triangles[i + (j + 1) % 3]];
            if (first == second)
                return Fail (refusal, "Cover edge collapses at the contact tolerance.");
            const bool forward = first < second;
            auto& edge = edges[forward ? std::make_pair (first, second) : std::make_pair (second, first)];
            ++edge.count;
            edge.balance += forward ? 1 : -1;
        }
    }
    // Convex source polygons can leave a long triangle edge opposite several
    // shorter collinear edges. Split only unmatched edges at existing snapped
    // vertices; do not close gaps, weld near lines or discard bad triangles.
    // Reduce the integer lattice direction to avoid cross-product overflow.
    const std::set<Key> vertices (keys.begin (), keys.end ());
    std::map<std::pair<Key, Key>, Incidence> normalized;
    for (const auto& [edge, incidence] : edges) {
        std::vector<Key> split { edge.first, edge.second };
        if (incidence.count == 1) {
            Key delta {}, direction {};
            int64_t divisor = 0;
            for (size_t axis = 0; axis < 3; ++axis) {
                delta[axis] = edge.second[axis] - edge.first[axis];
                divisor = std::gcd (divisor, delta[axis]);
            }
            for (size_t axis = 0; axis < 3; ++axis)
                direction[axis] = delta[axis] / divisor;
            for (const Key& vertex : vertices) {
                if (!work.Check (1, refusal))
                    return false;
                bool onSegment = vertex != edge.first && vertex != edge.second;
                int64_t step = 0;
                bool haveStep = false;
                for (size_t axis = 0; axis < 3 && onSegment; ++axis) {
                    const int64_t offset = vertex[axis] - edge.first[axis];
                    onSegment &= vertex[axis] >= std::min (edge.first[axis], edge.second[axis]) &&
                                 vertex[axis] <= std::max (edge.first[axis], edge.second[axis]);
                    if (direction[axis] == 0) {
                        onSegment &= offset == 0;
                    }
                    else {
                        onSegment &= offset % direction[axis] == 0;
                        const int64_t candidate = offset / direction[axis];
                        onSegment &= !haveStep || step == candidate;
                        step = candidate;
                        haveStep = true;
                    }
                }
                if (onSegment)
                    split.push_back (vertex);
            }
            std::sort (split.begin (), split.end ());
        }
        for (size_t i = 1; i < split.size (); ++i) {
            auto& segment = normalized[{ split[i - 1], split[i] }];
            segment.count += incidence.count;
            segment.balance += incidence.balance;
        }
    }
    for (const auto& entry : normalized)
        if (entry.second.count != 2 || entry.second.balance != 0)
            return Fail (refusal, "Cover body is open/non-manifold or has unmatched solid edges.");
    if (!std::isfinite (volume6) || volume6 <= 1e-12)
        return Fail (refusal, "Cover body has no positive outward-wound solid volume.");
    return SelfIntersectionFree (std::move (triangles), work, refusal);
}

bool PrepareFace (const Mesh& mesh, const FaceKey& key, Face& face, Work& work, Refusal& refusal)
{
    if (mesh.triMaterial.size () != mesh.TriangleCount ())
        return Fail (refusal, "Target material pool sidecar is missing/misaligned.");
    Mesh selected;
    std::vector<uint32_t> original;
    bool haveMaterial = false;
    for (size_t i = 0; i < mesh.TriangleCount (); ++i) {
        if (!work.Check (1, refusal))
            return false;
        if (mesh.triSourceBody[i] != key.body.sourceBody || mesh.triSourcePgon[i] != key.sourcePgon)
            continue;
        if (mesh.triMaterial[i] < 0 || (haveMaterial && face.material != mesh.triMaterial[i]))
            return Fail (refusal, "Source face has invalid or conflicting surface pool assignments.");
        haveMaterial = true;
        face.material = mesh.triMaterial[i];
        original.push_back (static_cast<uint32_t> (i));
        for (size_t j = 0; j < 3; ++j) {
            const V p = Point (mesh.vertices, mesh.triangles[i * 3 + j]);
            selected.triangles.push_back (static_cast<uint32_t> (selected.VertexCount ()));
            selected.vertices.insert (selected.vertices.end (), { p.x, p.y, p.z });
            face.bounds.Expand (p.x, p.y, p.z);
        }
        selected.triSourceBody.push_back (key.body.sourceBody);
        selected.triSourcePgon.push_back (key.sourcePgon);
    }
    if (original.empty ())
        return Fail (refusal, "Requested source face is absent from this capture.");
    const surfacemap::MatchTolerance tolerance { 1 - 1e-10, surfacecoverage::kPlaneTolerance,
                                                 surfacecoverage::kPlaneTolerance };
    std::string sourceError;
    const auto inventory = surfacemap::EnumerateSourceFaces (selected, &sourceError, tolerance);
    if (!sourceError.empty () || inventory.size () != 1)
        return Fail (refusal,
                     sourceError.empty () ? "Requested source face is not one supported planar source." : sourceError,
                     RefusalCode::UnsupportedTarget);
    face.key = key;
    face.source = inventory.front ();
    face.source.triangles = std::move (original);
    const auto& points = face.source.trianglePoints;
    if (!surfacecoverage::MakeFrame (Point (points, 0), Point (points, 1), Point (points, 2), face.frame))
        return Fail (refusal, "Cannot form a planar source frame.");
    for (size_t i = 0; i < points.size (); i += 9)
        face.subject.push_back ({ face.frame.Project (Point (points, i / 3)),
                                  face.frame.Project (Point (points, i / 3 + 1)),
                                  face.frame.Project (Point (points, i / 3 + 2)) });
    cp::PathsD normalized;
    std::string booleanError;
    if (!surfacecoverage::Boolean (cp::ClipType::Union, face.subject, {}, normalized, booleanError))
        return Fail (refusal, booleanError, RefusalCode::NumericalFailure);
    face.subject = std::move (normalized);
    if (face.subject.empty () ||
        std::abs (cp::Area (face.subject) - face.source.areaM2) > 1e-12 + 1e-6 * face.source.areaM2)
        return Fail (refusal, "Source boundary union disagrees with its standalone area.",
                     RefusalCode::NumericalFailure);
    return true;
}
} // namespace

V Point (const std::vector<double>& points, size_t index)
{
    return { points[index * 3], points[index * 3 + 1], points[index * 3 + 2] };
}

bool Work::Check (size_t amount, Refusal& refusal)
{
    if (cancelled && cancelled ())
        return Fail (refusal, "Exposed-surface calculation cancelled; no partial result.", RefusalCode::Cancelled);
    if (amount > limits.maxWork || statistics.work > limits.maxWork - amount)
        return Fail (refusal, "Exposed-surface work budget exceeded; no partial result.", RefusalCode::WorkLimit);
    statistics.work += amount;
    if (std::chrono::duration<double> (std::chrono::steady_clock::now () - began).count () > limits.maxSeconds)
        return Fail (refusal, "Exposed-surface time budget exceeded; no partial result.", RefusalCode::WorkLimit);
    return true;
}

bool Work::Check (size_t amount, std::string& error)
{
    Refusal refusal;
    const bool valid = Check (amount, refusal);
    error = std::move (refusal.message);
    return valid;
}

bool Overlaps (const Aabb& a, const Aabb& b, double margin)
{
    for (size_t i = 0; i < 3; ++i)
        if (a.mn[i] > b.mx[i] + surfacecoverage::kPlaneTolerance + margin ||
            b.mn[i] > a.mx[i] + surfacecoverage::kPlaneTolerance + margin)
            return false;
    return true;
}

bool Prepare (const Snapshot& snapshot, const Request& request, std::vector<Face>& faces, std::vector<Body>& covers,
              Work& work, Refusal& refusal)
{
    if (!snapshot.id || !snapshot.captureStamp || request.snapshotId != snapshot.id ||
        request.captureStamp != snapshot.captureStamp)
        return Fail (refusal, "Snapshot/capture identity is absent or differs from the request.",
                     RefusalCode::StaleCapture);
    if (request.coverScopeId.empty ())
        return Fail (refusal, "An explicit cover/model-state scope identity is required.");
    if (request.faces.empty ())
        return Fail (refusal, "An explicit nonempty target face scope is required.");
    if (!std::isfinite (request.contactToleranceM) || request.contactToleranceM < 0 || request.contactToleranceM > .001)
        return Fail (refusal, "Contact tolerance must be finite and in 0..0.001 m.");
    if (request.materialPoolFilter)
        for (int32_t material : *request.materialPoolFilter)
            if (material < 0)
                return Fail (refusal, "Material pool filters cannot contain negative indices.");
    if (!std::isfinite (work.limits.maxSeconds) || work.limits.maxSeconds <= 0)
        return Fail (refusal, "Invalid exposed-surface time budget.");
    const std::set<FaceKey> keys (request.faces.begin (), request.faces.end ());
    const std::set<BodyKey> bodies (request.covers.begin (), request.covers.end ());
    if (keys.size () > work.limits.maxFaces || bodies.size () > work.limits.maxCoverBodies)
        return Fail (refusal, "Exposed-surface face/body budget exceeded.", RefusalCode::WorkLimit);
    std::set<size_t> meshIndices;
    for (const auto& key : keys) {
        if (key.body.sourceBody <= 0 || key.sourcePgon <= 0)
            return Fail (refusal, "Target source identifiers must be 1-based.");
        meshIndices.insert (key.body.meshIndex);
    }
    for (const auto& key : bodies) {
        if (key.sourceBody <= 0)
            return Fail (refusal, "Cover source body must be 1-based.");
        meshIndices.insert (key.meshIndex);
    }
    size_t triangles = 0;
    std::set<std::string> guids;
    for (size_t index : meshIndices) {
        const auto target = std::find_if (keys.begin (), keys.end (),
                                          [index] (const FaceKey& key) { return key.body.meshIndex == index; });
        const auto cover = std::find_if (bodies.begin (), bodies.end (),
                                         [index] (const BodyKey& key) { return key.meshIndex == index; });
        const auto failMesh = [&] {
            // A shared target/cover mesh is reported as a target failure. Preserve
            // work/cancellation and more specific numerical categories.
            if (refusal.code == RefusalCode::None || refusal.code == RefusalCode::InvalidQuery)
                refusal.code = target != keys.end () ? RefusalCode::UnsupportedTarget : RefusalCode::InvalidCover;
            if (index < snapshot.meshes.size ())
                refusal.elementGuid = snapshot.meshes[index].guid;
            if (target != keys.end ()) {
                refusal.sourceBody = target->body.sourceBody;
                refusal.sourcePgon = target->sourcePgon;
            }
            else if (cover != bodies.end ()) {
                refusal.sourceBody = cover->sourceBody;
                refusal.sourcePgon = 0;
            }
            return false;
        };
        if (index >= snapshot.meshes.size ()) {
            Fail (refusal, "Requested mesh is absent from this capture.");
            return failMesh ();
        }
        const auto& mesh = snapshot.meshes[index];
        if (mesh.TriangleCount () > work.limits.maxInputTriangles ||
            triangles > work.limits.maxInputTriangles - mesh.TriangleCount ()) {
            Fail (refusal, "Exposed-surface input triangle budget exceeded.", RefusalCode::WorkLimit);
            return failMesh ();
        }
        triangles += mesh.TriangleCount ();
        if (!work.Check (mesh.TriangleCount () + mesh.VertexCount (), refusal))
            return failMesh ();
        if (!ValidateMesh (mesh, refusal))
            return failMesh ();
        if (!guids.insert (GuidKey (mesh.guid)).second) {
            Fail (refusal, "Ambiguous duplicate element identity in the requested capture.",
                  RefusalCode::AmbiguousOwnership);
            return failMesh ();
        }
    }
    for (const auto& key : keys) {
        Face face;
        if (!PrepareFace (snapshot.meshes[key.body.meshIndex], key, face, work, refusal)) {
            if (refusal.code == RefusalCode::InvalidQuery)
                refusal.code = RefusalCode::UnsupportedTarget;
            refusal.elementGuid = snapshot.meshes[key.body.meshIndex].guid;
            refusal.sourceBody = key.body.sourceBody;
            refusal.sourcePgon = key.sourcePgon;
            return false;
        }
        faces.push_back (std::move (face));
    }
    for (const auto& key : bodies) {
        Body body;
        body.key = key;
        const auto& mesh = snapshot.meshes[key.meshIndex];
        if (!work.Check (mesh.TriangleCount (), refusal))
            return false; // account for the full sidecar scan even in a many-body library part
        std::map<uint32_t, uint32_t> remap;
        for (size_t i = 0; i < mesh.TriangleCount (); ++i) {
            if (!work.Check (1, refusal))
                return false;
            if (mesh.triSourceBody[i] != key.sourceBody)
                continue;
            for (size_t j = 0; j < 3; ++j) {
                const uint32_t source = mesh.triangles[i * 3 + j];
                const auto [it, inserted] = remap.emplace (source, static_cast<uint32_t> (body.mesh.VertexCount ()));
                if (inserted) {
                    const V p = Point (mesh.vertices, source);
                    body.mesh.vertices.insert (body.mesh.vertices.end (), { p.x, p.y, p.z });
                    body.mesh.bounds.Expand (p.x, p.y, p.z);
                }
                body.mesh.triangles.push_back (it->second);
            }
        }
        if (body.mesh.triangles.empty ()) {
            refusal.elementGuid = mesh.guid;
            refusal.sourceBody = key.sourceBody;
            return Fail (refusal, "Requested cover body is absent from this capture.", RefusalCode::InvalidCover);
        }
        if (!ClosedSolid (body.mesh, work, refusal)) {
            if (refusal.code == RefusalCode::InvalidQuery)
                refusal.code = RefusalCode::InvalidCover;
            refusal.elementGuid = mesh.guid;
            refusal.sourceBody = key.sourceBody;
            refusal.message =
                "Cover " + mesh.guid + " / source body " + std::to_string (key.sourceBody) + ": " + refusal.message;
            return false;
        }
        covers.push_back (std::move (body));
    }
    return work.Check (0, refusal);
}
} // namespace geomsrv::exposed::detail
