#include "SunStudy/ViewpointStudy.hpp"
#include "Geometry/QueryEngine.hpp"
#include "SunStudy/SunStudyRoles.hpp"
#include "SunStudy/SunStudyCoreSelection.hpp"

#if defined(TAPIOCA_GEOMETRY_RUST)
#include "SunStudy/SunStudyRustViews.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <unordered_map>

namespace evp::sunstudy {

namespace {

using Point3 = std::array<double, 3>;

constexpr size_t kMaxSurfaceCells = 120000; // cells tested per run, whatever the scene
constexpr size_t kMaxCellSplits = 64;       // per triangle edge
constexpr double kSeenSlackMetres = 0.02;   // a ray stopping this near its cell still reaches it
constexpr double kCrossingMetres = 0.005;   // how closely a patch's border is bisected

double Distance (const Point3& a, const Point3& b)
{
    return std::sqrt ((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

double TriangleArea (const Point3& a, const Point3& b, const Point3& c)
{
    const double u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    const double n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
    return 0.5 * std::sqrt (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
}

// The capture triangles whose bounding box comes within the radius.
std::vector<std::array<Point3, 3>> NearTriangles (const geomsrv::Snapshot& snapshot, const ViewpointOptions& options)
{
    std::vector<std::array<Point3, 3>> near;
    for (const auto& mesh : snapshot.meshes)
        for (size_t i = 0; i + 2 < mesh.triangles.size (); i += 3) {
            std::array<Point3, 3> corners;
            bool ok = true;
            for (size_t k = 0; k < 3; ++k) {
                const size_t v = size_t (mesh.triangles[i + k]) * 3;
                if (v + 2 >= mesh.vertices.size ()) {
                    ok = false;
                    break;
                }
                corners[k] = { mesh.vertices[v], mesh.vertices[v + 1], mesh.vertices[v + 2] };
            }
            if (!ok)
                continue;
            double gap = 0.0;
            for (int axis = 0; axis < 3; ++axis) {
                const double low = (std::min) ({ corners[0][axis], corners[1][axis], corners[2][axis] });
                const double high = (std::max) ({ corners[0][axis], corners[1][axis], corners[2][axis] });
                const double outside = (std::max) ({ low - options.point[axis], options.point[axis] - high, 0.0 });
                gap += outside * outside;
            }
            if (gap <= options.radius * options.radius)
                near.push_back (corners);
        }
    return near;
}

size_t Splits (const std::array<Point3, 3>& t, double cell)
{
    const double longest = (std::max) ({ Distance (t[0], t[1]), Distance (t[1], t[2]), Distance (t[2], t[0]) });
    return (std::min) (kMaxCellSplits, (std::max) (size_t (1), size_t (std::ceil (longest / cell))));
}

// Sphere: lay an n x n barycentric grid on each near triangle and test its
// corners for being seen: within the radius and band, and reached by the ray to
// them. A cell seen at every corner is kept; a cell seen at some is cut at the
// crossing on each of its mixed edges, found by bisecting along that edge.
// ⚠️ CORNERS AND CROSSINGS, NOT CELL CENTRES. Keeping whole cells by their
// centre stepped the patch's border cell by cell along the radius and every
// shadow; a crossing puts it where the radius, the band or the shadow falls.
// The cell side starts from the radius and coarsens until the run fits its budget.
bool SeenSurfaces (const geomsrv::Snapshot& snapshot, const geomsrv::QueryEngine* engine,
                   const std::vector<uint8_t>& mask, bool everything, const ViewpointOptions& options,
                   ViewpointStudyResult& result, const std::function<bool ()>& cancelled)
{
    const auto near = NearTriangles (snapshot, options);
    double cell = options.surfaceCellMetres > 0.0 ? options.surfaceCellMetres
                                                  : (std::min) (1.0, (std::max) (0.1, options.radius / 80.0));
    for (int pass = 0; pass < 12; ++pass) {
        size_t cells = 0;
        for (const auto& t : near)
            cells += Splits (t, cell) * Splits (t, cell);
        if (cells <= kMaxSurfaceCells)
            break;
        cell *= 1.5;
    }
    constexpr double kRadians = 3.14159265358979323846 / 180.0;
    const double sinLow = std::sin (options.minElevationDegrees * kRadians);
    const double sinHigh = std::sin (options.maxElevationDegrees * kRadians);
    const Point3& eye = options.point;
    const auto seen = [&] (const Point3& at) {
        const double d = Distance (eye, at);
        if (d > options.radius)
            return false;
        if (d < 1.0e-9)
            return true;
        const double direction[3] = { (at[0] - eye[0]) / d, (at[1] - eye[1]) / d, (at[2] - eye[2]) / d };
        if (direction[2] < sinLow - 1.0e-12 || direction[2] > sinHigh + 1.0e-12)
            return false;
        if (engine == nullptr || d <= kSeenSlackMetres)
            return true;
        const double reach = d - kSeenSlackMetres;
        if (everything)
            return !engine->Occluded (eye.data (), direction, 0.0, reach);
        return !engine->RaycastMasked (eye.data (), direction, reach, mask).hit;
    };
    // Where seen `from` and unseen `to` change over, to within kCrossingMetres.
    const auto crossing = [&] (Point3 from, Point3 to) {
        for (int step = 0; step < 16 && Distance (from, to) > kCrossingMetres; ++step) {
            Point3 middle;
            for (int axis = 0; axis < 3; ++axis)
                middle[axis] = 0.5 * (from[axis] + to[axis]);
            (seen (middle) ? from : to) = middle;
        }
        Point3 at;
        for (int axis = 0; axis < 3; ++axis)
            at[axis] = 0.5 * (from[axis] + to[axis]);
        return at;
    };
    std::vector<Point3> corners;
    std::vector<uint8_t> visible;
    std::unordered_map<uint64_t, Point3> crossings; // by the edge's two corner indices
    std::vector<std::array<Point3, 3>> pieces;
    for (size_t index = 0; index < near.size (); ++index) {
        if ((index & 63u) == 0 && cancelled ())
            return false;
        const auto& t = near[index];
        const size_t n = Splits (t, cell);
        // Barycentric grid: corner (i, j) is t0 + i/n (t1 - t0) + j/n (t2 - t0),
        // stored at i (n + 1) + j.
        const size_t side = n + 1;
        corners.assign (side * side, Point3 {});
        visible.assign (side * side, 0);
        bool whole = true;
        for (size_t i = 0; i <= n; ++i)
            for (size_t j = 0; i + j <= n; ++j) {
                Point3& at = corners[i * side + j];
                for (int axis = 0; axis < 3; ++axis)
                    at[axis] = t[0][axis] + (t[1][axis] - t[0][axis]) * double (i) / double (n) +
                               (t[2][axis] - t[0][axis]) * double (j) / double (n);
                visible[i * side + j] = seen (at) ? 1 : 0;
                whole = whole && visible[i * side + j] != 0;
            }
        if (whole) {
            result.seenSurface.insert (result.seenSurface.end (), t.begin (), t.end ());
            result.seenArea += TriangleArea (t[0], t[1], t[2]);
            continue;
        }
        crossings.clear ();
        const auto edge = [&] (size_t a, size_t b) {
            const uint64_t key = uint64_t ((std::min) (a, b)) << 32 | uint64_t ((std::max) (a, b));
            const auto found = crossings.find (key);
            if (found != crossings.end ())
                return found->second;
            const Point3 at = visible[a] != 0 ? crossing (corners[a], corners[b]) : crossing (corners[b], corners[a]);
            crossings.emplace (key, at);
            return at;
        };
        pieces.clear ();
        for (size_t i = 0; i < n; ++i)
            for (size_t j = 0; i + j < n; ++j)
                for (int flip = 0; flip < (i + j + 1 < n ? 2 : 1); ++flip) {
                    const size_t cell3[3] = { flip == 0 ? i * side + j : (i + 1) * side + j,
                                              flip == 0 ? (i + 1) * side + j : (i + 1) * side + j + 1,
                                              i * side + j + 1 };
                    // The seen part of the cell: its seen corners, and a crossing
                    // on each edge whose ends disagree. Three or four corners.
                    Point3 polygon[4];
                    size_t count = 0;
                    for (size_t k = 0; k < 3; ++k) {
                        const size_t a = cell3[k], b = cell3[(k + 1) % 3];
                        if (visible[a] != 0)
                            polygon[count++] = corners[a];
                        if (visible[a] != visible[b])
                            polygon[count++] = edge (a, b);
                    }
                    for (size_t k = 1; k + 1 < count; ++k)
                        pieces.push_back ({ polygon[0], polygon[k], polygon[k + 1] });
                }
        for (const auto& piece : pieces) {
            result.seenSurface.insert (result.seenSurface.end (), piece.begin (), piece.end ());
            result.seenArea += TriangleArea (piece[0], piece[1], piece[2]);
        }
    }
    result.surfaceCellMetres = cell;
    return true;
}

} // namespace

ViewpointStudyResult RunViewpointStudy (const std::shared_ptr<const geomsrv::Snapshot>& snapshot,
                                        const std::vector<std::string>& context, const ViewpointOptions& options,
                                        const std::function<bool ()>& isCancelled)
{
    ViewpointStudyResult result;
    result.options = options;
    const auto cancelled = [&] { return isCancelled && isCancelled (); };
    if (snapshot == nullptr || !std::isfinite (options.radius) || options.radius <= 0.0 || options.rays < 32 ||
        options.rays > 1440 || !std::all_of (options.point.begin (), options.point.end (), [] (double value) {
            return std::isfinite (value);
        })) {
        result.error = "viewpoint needs a snapshot, finite point, positive radius and 32..1440 rays";
        return result;
    }
    if (options.mode == ViewpointMode::FieldOfView &&
        (!std::isfinite (options.bearingDegrees) || !std::isfinite (options.fieldOfViewDegrees) ||
         options.fieldOfViewDegrees < 1.0 || options.fieldOfViewDegrees > 359.0)) {
        result.error = "viewpoint field of view needs a finite bearing and a 1..359 degree angle";
        return result;
    }
    if (options.mode == ViewpointMode::Sphere &&
        (!std::isfinite (options.minElevationDegrees) || !std::isfinite (options.maxElevationDegrees) ||
         options.minElevationDegrees < -90.0 || options.maxElevationDegrees > 90.0 ||
         options.minElevationDegrees >= options.maxElevationDegrees || options.sphereColumns < 8 ||
         options.sphereColumns > 720 || options.sphereRows < 2 || options.sphereRows > 361)) {
        result.error = "viewpoint sphere needs elevations -90 <= min < max <= 90, 8..720 columns and 2..361 rows";
        return result;
    }
    if (options.mode != ViewpointMode::Plan && options.mode != ViewpointMode::FieldOfView &&
        options.mode != ViewpointMode::Sphere) {
        result.error = "viewpoint mode must be plan, field of view or sphere";
        return result;
    }
    result.snapshotId = snapshot->id;
    result.captureStamp = snapshot->captureStamp;
    std::set<std::string> selected;
    for (const auto& guid : context)
        selected.insert (CanonicalGuid (guid));
    auto missing = selected;
    std::vector<uint8_t> mask (snapshot->meshes.size (), 0);
    for (size_t mesh = 0; mesh < snapshot->meshes.size (); ++mesh) {
        const auto guid = CanonicalGuid (snapshot->meshes[mesh].guid);
        mask[mesh] = selected.find (guid) != selected.end ();
        missing.erase (guid);
    }
    if (!missing.empty ()) {
        result.error = "viewpoint Context contains objects absent from the capture";
        return result;
    }
    const auto engine = context.empty () ? nullptr : geomsrv::QueryIndexCache::Get ().For (snapshot);
    if (!context.empty () && engine == nullptr) {
        result.error = "viewpoint could not acquire the shared snapshot query index";
        return result;
    }
    constexpr double kRadians = 3.14159265358979323846 / 180.0;
    const bool sphere = options.mode == ViewpointMode::Sphere;
    const bool sector = options.mode == ViewpointMode::FieldOfView;
    // A field of view keeps the plan's angular density and both edges of its angle.
    const size_t sectorRays =
        (std::max) (size_t (2), size_t (std::lround (double (options.rays) * options.fieldOfViewDegrees / 360.0)) + 1);
    const size_t rows = sphere ? options.sphereRows : 1;
    const size_t columns = sphere ? options.sphereColumns : sector ? sectorRays : options.rays;
    result.perimeter.reserve (rows * columns);
    result.rayMesh.reserve (rows * columns);
    double openWeight = 0.0, totalWeight = 0.0;
    for (size_t row = 0; row < rows; ++row) {
        // Elevation of this row, and the band of the sphere it stands for (its
        // solid angle per unit azimuth is the difference of sines across it).
        const double elevation =
            sphere ? options.minElevationDegrees +
                         (options.maxElevationDegrees - options.minElevationDegrees) * double (row) / double (rows - 1)
                   : 0.0;
        const double half =
            sphere ? 0.5 * (options.maxElevationDegrees - options.minElevationDegrees) / double (rows - 1) : 0.0;
        const double low = (std::max) (options.minElevationDegrees, elevation - half);
        const double high = (std::min) (options.maxElevationDegrees, elevation + half);
        const double band = sphere ? std::sin (high * kRadians) - std::sin (low * kRadians) : 1.0;
        for (size_t column = 0; column < columns; ++column) {
            if (cancelled ()) {
                result.perimeter.clear ();
                result.rayMesh.clear ();
                result.error = "viewpoint calculation cancelled";
                return result;
            }
            const double angle = sector ? (options.bearingDegrees - 0.5 * options.fieldOfViewDegrees +
                                           options.fieldOfViewDegrees * double (column) / double (columns - 1)) *
                                              kRadians
                                        : 6.28318530717958647692 * double (column) / double (columns);
            const double up = elevation * kRadians;
            const double direction[3] = { std::cos (up) * std::cos (angle), std::cos (up) * std::sin (angle),
                                          std::sin (up) };
            double distance = options.radius;
            int64_t blocker = -1;
            if (engine != nullptr) {
                const auto hit = engine->RaycastMasked (options.point.data (), direction, options.radius, mask);
                if (hit.hit) {
                    distance = (std::max) (0.0, (std::min) (distance, hit.t));
                    ++result.collisionCount;
                    blocker = static_cast<int64_t> (hit.meshIndex);
                }
            }
            result.rayMesh.push_back (blocker);
            result.perimeter.push_back ({ options.point[0] + distance * direction[0],
                                          options.point[1] + distance * direction[1],
                                          options.point[2] + distance * direction[2] });
            // Each ray stands for band x (2 pi / columns) steradians of the sphere.
            const double weight = band;
            totalWeight += weight;
            openWeight += blocker < 0 ? weight : 0.0;
            if (sphere)
                result.volume +=
                    weight * (6.28318530717958647692 / double (columns)) * distance * distance * distance / 3.0;
        }
    }
    if (!sphere) {
        const size_t count = result.perimeter.size ();
        for (size_t i = 0; i < count; ++i) {
            if (sector && i + 1 == count)
                break; // open: the angle's two edges meet at the point, adding no area
            const auto& a = result.perimeter[i];
            const auto& b = result.perimeter[(i + 1) % count];
            result.area += ((a[0] - options.point[0]) * (b[1] - options.point[1]) -
                            (a[1] - options.point[1]) * (b[0] - options.point[0])) *
                           0.5;
        }
    }
    result.openFraction = totalWeight > 0.0 ? openWeight / totalWeight : 0.0;
    if (sphere && !SeenSurfaces (*snapshot, engine.get (), mask,
                                 std::all_of (mask.begin (), mask.end (), [] (uint8_t on) { return on != 0; }), options,
                                 result, cancelled)) {
        result.perimeter.clear ();
        result.rayMesh.clear ();
        result.seenSurface.clear ();
        result.error = "viewpoint calculation cancelled";
        return result;
    }
    result.valid = true;
    return result;
}

std::vector<std::array<double, 2>> ViewpointPlanOutline (const ViewpointStudyResult& result)
{
    std::vector<std::array<double, 2>> outline;
    if (!result.valid || result.options.mode == ViewpointMode::Sphere)
        return outline;
    const auto add = [&] (double x, double y) {
        if (!outline.empty () && std::hypot (x - outline.back ()[0], y - outline.back ()[1]) < 1.0e-3)
            return;
        outline.push_back ({ x, y });
    };
    if (!result.Closed ())
        add (result.options.point[0], result.options.point[1]);
    for (const auto& end : result.perimeter)
        add (end[0], end[1]);
    while (outline.size () > 1 &&
           std::hypot (outline.front ()[0] - outline.back ()[0], outline.front ()[1] - outline.back ()[1]) < 1.0e-3)
        outline.pop_back ();
    double twiceArea = 0.0;
    for (size_t i = 0; i < outline.size (); ++i) {
        const auto& a = outline[i];
        const auto& b = outline[(i + 1) % outline.size ()];
        twiceArea += (a[0] - outline[0][0]) * (b[1] - outline[0][1]) - (b[0] - outline[0][0]) * (a[1] - outline[0][1]);
    }
    if (outline.size () < 3 || std::abs (twiceArea) < 1.0e-9)
        outline.clear ();
    return outline;
}

ViewpointPlanContour ViewpointBakeContour (const ViewpointStudyResult& result, double straightMetres)
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kLongestArc = kPi * 0.999; // a fill edge's arc may not exceed pi
    ViewpointPlanContour contour;
    auto ring = ViewpointPlanOutline (result);
    const size_t n = ring.size ();
    if (n < 3)
        return contour;
    const double cx = result.options.point[0], cy = result.options.point[1], radius = result.options.radius;
    const double onCircle = 1.0e-4 + radius * 1.0e-9;
    const auto circular = [&] (const std::array<double, 2>& p) {
        return std::abs (std::hypot (p[0] - cx, p[1] - cy) - radius) <= onCircle;
    };
    // Start where the ring must have a vertex: the point of a field of view, else
    // the first vertex off the circle after one on it, else (closed in by
    // Context) the sharpest corner -- so neither an arc nor a straight run wraps
    // index 0.
    size_t start = 0;
    bool wholeCircle = false;
    if (result.Closed ()) {
        size_t off = 0;
        while (off < n && circular (ring[off]))
            ++off;
        wholeCircle = off == n;
        start = n;
        for (size_t i = 0; i < n && start == n && !wholeCircle; ++i)
            if (!circular (ring[i]) && circular (ring[(i + n - 1) % n]))
                start = i;
        if (start == n) {
            start = 0;
            double sharpest = -1.0;
            for (size_t i = 0; i < n && !wholeCircle; ++i) {
                const auto& a = ring[(i + n - 1) % n];
                const auto& b = ring[(i + 1) % n];
                const double dx = b[0] - a[0], dy = b[1] - a[1], length = std::hypot (dx, dy);
                const double bend =
                    length < 1.0e-12 ? 0.0
                                     : std::abs (dx * (ring[i][1] - a[1]) - dy * (ring[i][0] - a[0])) / length;
                if (bend > sharpest) {
                    sharpest = bend;
                    start = i;
                }
            }
        }
    }
    if (!wholeCircle)
        std::rotate (ring.begin (), ring.begin () + static_cast<std::ptrdiff_t> (start), ring.end ());
    std::vector<uint8_t> onRadius (n);
    for (size_t i = 0; i < n; ++i)
        onRadius[i] = circular (ring[i]) ? 1 : 0;
    // The signed angle about the point from ring[i] to ring[j].
    const auto sweep = [&] (size_t i, size_t j) {
        const double ax = ring[i][0] - cx, ay = ring[i][1] - cy, bx = ring[j][0] - cx, by = ring[j][1] - cy;
        return std::atan2 (ax * by - ay * bx, ax * bx + ay * by);
    };
    // Edge i -> i + 1 is an arc when both its ends reached the radius; `kept`
    // marks the vertices that stay, `angle` the arc leaving each kept vertex.
    std::vector<uint8_t> kept (n, 0);
    std::vector<double> angle (n, 0.0);
    const auto arcEdge = [&] (size_t i) { return onRadius[i] && onRadius[(i + 1) % n]; };
    for (size_t i = 0; i < n;) {
        if (!arcEdge (i)) {
            ++i;
            continue;
        }
        // A run of arc edges from i; split wherever it would turn past kLongestArc.
        size_t from = i;
        double turned = 0.0;
        kept[from] = 1;
        size_t j = i;
        do {
            const size_t next = (j + 1) % n;
            const double step = sweep (j, next);
            if (std::abs (turned + step) > kLongestArc) {
                angle[from] = turned;
                from = j;
                kept[from] = 1;
                turned = 0.0;
            }
            turned += step;
            j = next;
        } while (j != 0 && arcEdge (j));
        angle[from] = turned;
        kept[j] = 1;
        if (j == 0)
            break;
        i = j;
    }
    // A vertex not on an arc stays when it ends a run of ray ends along one line:
    // walk from each kept vertex as far as every vertex passed lies within
    // straightMetres of the chord.
    const auto offChord = [&] (size_t a, size_t b, size_t k) {
        const double dx = ring[b][0] - ring[a][0], dy = ring[b][1] - ring[a][1];
        const double length = std::hypot (dx, dy);
        const double px = ring[k][0] - ring[a][0], py = ring[k][1] - ring[a][1];
        if (length < 1.0e-12)
            return std::hypot (px, py);
        return std::abs (dx * py - dy * px) / length;
    };
    kept[0] = 1; // off the circle (the point, for a field of view), or the whole circle's start
    for (size_t anchor = 0; !wholeCircle && anchor < n;) {
        if (arcEdge (anchor)) { // skip to where this arc ends
            do
                ++anchor;
            while (anchor < n && !kept[anchor]);
            continue;
        }
        size_t end = anchor + 1; // n stands for vertex 0, closing the ring
        for (size_t candidate = anchor + 2; candidate <= n && !kept[candidate - 1]; ++candidate) {
            bool straight = true;
            for (size_t k = anchor + 1; k < candidate && straight; ++k)
                straight = offChord (anchor, candidate % n, k) <= straightMetres;
            if (!straight)
                break;
            end = candidate;
        }
        if (end < n)
            kept[end] = 1;
        anchor = end;
    }
    for (size_t i = 0; i < n; ++i)
        if (kept[i]) {
            contour.points.push_back (ring[i]);
            contour.arcAngles.push_back (angle[i]);
        }
    return contour;
}

std::vector<ViewpointBlocker> ViewpointBlockers (const ViewpointStudyResult& result, const geomsrv::Snapshot& snapshot)
{
    std::vector<ViewpointBlocker> blockers;
    if (!result.valid || result.snapshotId != snapshot.id || result.rayMesh.size () != result.perimeter.size ())
        return blockers;
    std::vector<size_t> slot (snapshot.meshes.size (), SIZE_MAX);
    for (size_t ray = 0; ray < result.rayMesh.size (); ++ray) {
        const int64_t mesh = result.rayMesh[ray];
        if (mesh < 0 || static_cast<size_t> (mesh) >= snapshot.meshes.size ())
            continue;
        const auto& end = result.perimeter[ray];
        const double distance = std::hypot (end[0] - result.options.point[0], end[1] - result.options.point[1],
                                            end[2] - result.options.point[2]);
        size_t& index = slot[static_cast<size_t> (mesh)];
        if (index == SIZE_MAX) {
            index = blockers.size ();
            blockers.push_back (
                { static_cast<size_t> (mesh), snapshot.meshes[static_cast<size_t> (mesh)].guid, 0, distance });
        }
        ViewpointBlocker& blocker = blockers[index];
        ++blocker.rays;
        blocker.nearest = (std::min) (blocker.nearest, distance);
    }
    std::stable_sort (blockers.begin (), blockers.end (), [] (const ViewpointBlocker& a, const ViewpointBlocker& b) {
        if (a.rays != b.rays)
            return a.rays > b.rays;
        if (a.nearest != b.nearest)
            return a.nearest < b.nearest;
        return a.mesh < b.mesh;
    });
    return blockers;
}

std::vector<ViewpointArc> ViewpointArcs (const std::vector<int64_t>& rayMesh)
{
    std::vector<ViewpointArc> arcs;
    const size_t count = rayMesh.size ();
    if (count == 0)
        return arcs;
    // Start at a ray whose predecessor differs, so a run crossing ray 0 stays whole.
    size_t start = 0;
    while (start < count && rayMesh[start] == rayMesh[(start + count - 1) % count])
        ++start;
    if (start == count)
        return { { 0, count, rayMesh[0] } }; // one outcome all the way round
    for (size_t offset = 0; offset < count; ++offset) {
        const size_t ray = (start + offset) % count;
        if (arcs.empty () || arcs.back ().mesh != rayMesh[ray])
            arcs.push_back ({ ray, 0, rayMesh[ray] });
        ++arcs.back ().count;
    }
    return arcs;
}

bool ToggleViewpointContext (std::vector<std::string>& context, const std::string& guid)
{
    const std::string key = CanonicalGuid (guid);
    const auto kept = std::remove_if (context.begin (), context.end (),
                                      [&] (const std::string& entry) { return CanonicalGuid (entry) == key; });
    if (kept != context.end ()) {
        context.erase (kept, context.end ()); // every spelling of the same element
        return false;
    }
    context.push_back (guid);
    return true;
}

ViewpointStudyResult RunSelectedViewpointStudy (const std::shared_ptr<const geomsrv::Snapshot>& snapshot,
                                                const std::vector<std::string>& context,
                                                const ViewpointOptions& options,
                                                const std::function<bool ()>& isCancelled)
{
#if defined(TAPIOCA_GEOMETRY_RUST)
    // ⚠️ ONLY THE PLAN ISOVIST REACHES THE RUST KERNEL: its options have no mode,
    // so a field of view or a sphere would come back as a full plan circle.
    if (SelectedSunStudyCore () == SunStudyCoreKind::Rust && options.mode == ViewpointMode::Plan) {
        auto result = rust::RunViewpointStudy (snapshot, context, options, isCancelled);
        if (result.valid && !result.rayMesh.empty ())
            result.openFraction = double (std::count (result.rayMesh.begin (), result.rayMesh.end (), int64_t (-1))) /
                                  double (result.rayMesh.size ());
        return result;
    }
#endif
    return RunViewpointStudy (snapshot, context, options, isCancelled);
}

} // namespace evp::sunstudy
