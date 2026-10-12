#include "SurfaceSourceMap.hpp"
#include "SurfaceSourceBoundary.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace geomsrv {
namespace surfacemap {

namespace {

constexpr double kEps = 1.0e-12;

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
};

Vec3 Sub (const Vec3& a, const Vec3& b)
{
    return Vec3 { a.x - b.x, a.y - b.y, a.z - b.z };
}
Vec3 Sum (const Vec3& a, const Vec3& b)
{
    return Vec3 { a.x + b.x, a.y + b.y, a.z + b.z };
}
Vec3 Scale (const Vec3& a, double s)
{
    return Vec3 { a.x * s, a.y * s, a.z * s };
}
Vec3 Cross (const Vec3& a, const Vec3& b)
{
    return Vec3 { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
double Dot (const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
double Length (const Vec3& a)
{
    return std::sqrt (Dot (a, a));
}
bool Finite (const Vec3& a)
{
    return std::isfinite (a.x) && std::isfinite (a.y) && std::isfinite (a.z);
}
Vec3 At (const std::vector<double>& xyz, size_t point)
{
    return Vec3 { xyz[point * 3], xyz[point * 3 + 1], xyz[point * 3 + 2] };
}

bool ValidTolerance (const MatchTolerance& tolerance, std::string& reason)
{
    if (!std::isfinite (tolerance.normalDot) || tolerance.normalDot <= 0.0 || tolerance.normalDot > 1.0) {
        reason = "normalDot must be finite and in (0, 1]";
        return false;
    }
    if (!std::isfinite (tolerance.planeDistance) || tolerance.planeDistance <= 0.0) {
        reason = "planeDistance must be finite and positive";
        return false;
    }
    if (!std::isfinite (tolerance.vertexTolerance) || tolerance.vertexTolerance <= 0.0) {
        reason = "vertexTolerance must be finite and positive";
        return false;
    }
    return true;
}

// Newell vector area of a planar contour: magnitude is the area, direction is
// the winding normal. Finite-checked so a huge coordinate is refused rather than
// silently becoming inf.
bool NewellAreaVector (const std::vector<double>& xyz, Vec3& area, std::string& reason)
{
    const size_t count = xyz.size () / 3;
    if (count < 3) {
        reason = "a contour needs at least three points";
        return false;
    }
    Vec3 accum;
    const Vec3 origin = At (xyz, 0);
    for (size_t i = 0; i < count; ++i) {
        const Vec3 p = At (xyz, i);
        const Vec3 q = At (xyz, (i + 1) % count);
        if (!Finite (p) || !Finite (q)) {
            reason = "contour has a non-finite coordinate";
            return false;
        }
        accum = Sum (accum, Cross (Sub (p, origin), Sub (q, origin)));
    }
    area = Scale (accum, 0.5);
    if (!Finite (area)) {
        reason = "contour area overflowed";
        return false;
    }
    return true;
}

bool ValidateMesh (const Mesh& mesh, std::string& reason)
{
    if (mesh.triangles.size () % 3 != 0) {
        reason = "triangles length is not a multiple of 3";
        return false;
    }
    if (mesh.vertices.size () % 3 != 0) {
        reason = "vertices length is not a multiple of 3";
        return false;
    }

    const size_t triangleCount = mesh.triangles.size () / 3;
    if (triangleCount == 0)
        return true; // an empty mesh is valid; it simply has no faces

    if (mesh.triSourceBody.size () != triangleCount || mesh.triSourcePgon.size () != triangleCount) {
        reason = "provenance sidecar length (" + std::to_string (mesh.triSourceBody.size ()) + "/" +
                 std::to_string (mesh.triSourcePgon.size ()) + ") != triangle count (" +
                 std::to_string (triangleCount) + ")";
        return false;
    }

    const size_t vertexCount = mesh.vertices.size () / 3;
    for (size_t tri = 0; tri < triangleCount; ++tri) {
        if (mesh.triSourceBody[tri] <= 0 || mesh.triSourcePgon[tri] <= 0) {
            reason = "provenance id is not 1-based at triangle " + std::to_string (tri);
            return false;
        }
        for (int corner = 0; corner < 3; ++corner) {
            const uint32_t index = mesh.triangles[tri * 3 + corner];
            if (index >= vertexCount) {
                reason = "triangle " + std::to_string (tri) + " indexes missing vertex " + std::to_string (index);
                return false;
            }
        }
    }

    for (size_t i = 0; i < mesh.vertices.size (); ++i) {
        if (!std::isfinite (mesh.vertices[i])) {
            reason = "non-finite vertex coordinate at " + std::to_string (i);
            return false;
        }
    }
    return true;
}

std::vector<std::vector<double>> FacePieces (const std::vector<double>& trianglePoints)
{
    std::vector<std::vector<double>> pieces;
    for (size_t i = 0; i + 8 < trianglePoints.size (); i += 9)
        pieces.emplace_back (trianglePoints.begin () + static_cast<ptrdiff_t> (i),
                             trianglePoints.begin () + static_cast<ptrdiff_t> (i + 9));
    return pieces;
}

// Validate and summarise one (body, pgon) face. Refuses a face whose triangles
// are non-planar, inconsistently wound or degenerate; whose centroid is not
// finite (a huge coordinate can overflow the area-weighted sum while the area
// stays finite); or whose boundary is not one outer contour with oppositely
// wound holes.
bool BuildFace (const Mesh& mesh, const std::vector<uint32_t>& tris, const MatchTolerance& tolerance, SourceFace& face,
                std::string& reason)
{
    face.trianglePoints.reserve (tris.size () * 9);
    Vec3 normal;
    bool haveNormal = false;
    double area = 0.0;
    Vec3 centroidAccum;

    for (uint32_t tri : tris) {
        const Vec3 a = At (mesh.vertices, mesh.triangles[tri * 3 + 0]);
        const Vec3 b = At (mesh.vertices, mesh.triangles[tri * 3 + 1]);
        const Vec3 c = At (mesh.vertices, mesh.triangles[tri * 3 + 2]);

        const double edgeScale = Length (Sub (b, a)) + Length (Sub (c, b)) + Length (Sub (a, c));
        const Vec3 cross = Cross (Sub (b, a), Sub (c, a));
        const double magnitude = Length (cross);
        if (!std::isfinite (edgeScale) || !std::isfinite (magnitude)) {
            reason = "face triangle area overflowed";
            return false;
        }
        if (!(edgeScale > 0.0) || magnitude <= kEps * edgeScale * edgeScale) {
            reason = "face has a degenerate triangle";
            return false;
        }

        const Vec3 unit = Scale (cross, 1.0 / magnitude);
        if (!haveNormal) {
            normal = unit;
            haveNormal = true;
        }
        else if (Dot (unit, normal) < tolerance.normalDot) {
            reason = "face has non-planar or inconsistently wound triangles";
            return false;
        }

        const double triArea = 0.5 * magnitude;
        area += triArea;
        const Vec3 triCentroid { (a.x + b.x + c.x) / 3.0, (a.y + b.y + c.y) / 3.0, (a.z + b.z + c.z) / 3.0 };
        centroidAccum = Sum (centroidAccum, Scale (triCentroid, triArea));

        face.trianglePoints.push_back (a.x);
        face.trianglePoints.push_back (a.y);
        face.trianglePoints.push_back (a.z);
        face.trianglePoints.push_back (b.x);
        face.trianglePoints.push_back (b.y);
        face.trianglePoints.push_back (b.z);
        face.trianglePoints.push_back (c.x);
        face.trianglePoints.push_back (c.y);
        face.trianglePoints.push_back (c.z);
    }

    if (!haveNormal || !std::isfinite (area) || !(area > 0.0)) {
        reason = "face has no finite, positive area";
        return false;
    }
    if (!Finite (centroidAccum)) {
        reason = "face centroid overflowed";
        return false;
    }

    const Vec3 p0 { face.trianglePoints[0], face.trianglePoints[1], face.trianglePoints[2] };
    for (size_t i = 0; i < face.trianglePoints.size (); i += 3) {
        const Vec3 p { face.trianglePoints[i], face.trianglePoints[i + 1], face.trianglePoints[i + 2] };
        if (std::fabs (Dot (Sub (p, p0), normal)) > tolerance.planeDistance) {
            reason = "face is not planar";
            return false;
        }
    }

    const Vec3 centroid = Scale (centroidAccum, 1.0 / area);
    if (!Finite (centroid)) {
        reason = "face centroid overflowed";
        return false;
    }

    // Inventory and AnalyticArea refuse malformed tessellation too, not only
    // Resolve, so duplicated triangles cannot publish an inflated area.
    const double normalArray[3] = { normal.x, normal.y, normal.z };
    const double originArray[3] = { p0.x, p0.y, p0.z };
    boundary::Frame frame;
    if (!boundary::MakeFrame (normalArray, originArray, frame, reason))
        return false;
    std::vector<boundary::Loop> loops;
    if (!boundary::BuildCanonicalLoops (FacePieces (face.trianglePoints), frame, tolerance.vertexTolerance, loops,
                                        reason))
        return false;

    face.normal[0] = normal.x;
    face.normal[1] = normal.y;
    face.normal[2] = normal.z;
    face.areaM2 = area;
    face.centroid[0] = centroid.x;
    face.centroid[1] = centroid.y;
    face.centroid[2] = centroid.z;

    return true;
}

// Validate and summarise a query: every piece planar, every piece parallel to
// the first, area finite. Returns the overall normal and a point on the plane.
bool BuildQuery (const FaceQuery& query, const MatchTolerance& tolerance, Vec3& normal, Vec3& origin, double& area,
                 std::string& reason)
{
    std::vector<const std::vector<double>*> pieces;
    if (!query.points.empty ())
        pieces.push_back (&query.points);
    for (const std::vector<double>& piece : query.pieces)
        if (!piece.empty ())
            pieces.push_back (&piece);
    if (pieces.empty ()) {
        reason = "query has no contour";
        return false;
    }

    bool haveNormal = false;
    bool haveOrigin = false;
    area = 0.0;
    for (const std::vector<double>* piece : pieces) {
        if (piece->size () % 3 != 0) {
            reason = "query contour is not a multiple of xyz triples";
            return false;
        }
        Vec3 pieceArea;
        if (!NewellAreaVector (*piece, pieceArea, reason))
            return false;
        const double magnitude = Length (pieceArea);
        if (!std::isfinite (magnitude) || !(magnitude > kEps)) {
            reason = "query contour is degenerate";
            return false;
        }
        const Vec3 unit = Scale (pieceArea, 1.0 / magnitude);
        if (!haveNormal) {
            normal = unit;
            haveNormal = true;
        }
        // A hole is wound opposite, so only parallelism matters here, not sense.
        else if (std::fabs (Dot (unit, normal)) < tolerance.normalDot) {
            reason = "query contours are not coplanar";
            return false;
        }
        area += magnitude;
        if (!haveOrigin) {
            origin = At (*piece, 0);
            haveOrigin = true;
        }
    }
    if (!std::isfinite (area) || !(area > 0.0)) {
        reason = "query area is not finite";
        return false;
    }

    for (const std::vector<double>* piece : pieces) {
        for (size_t i = 0; i < piece->size () / 3; ++i) {
            const Vec3 p = At (*piece, i);
            if (std::fabs (Dot (Sub (p, origin), normal)) > tolerance.planeDistance) {
                reason = "query contour is not planar";
                return false;
            }
        }
    }
    return true;
}

std::vector<SourceFace> EnumerateFaces (const Mesh& mesh, std::string* reason, const MatchTolerance& tolerance,
                                        const FaceQuery* query)
{
    if (reason != nullptr)
        reason->clear ();

    std::vector<SourceFace> faces;
    std::string failure;
    if (!ValidTolerance (tolerance, failure)) {
        if (reason != nullptr)
            *reason = failure;
        return faces;
    }
    if (!ValidateMesh (mesh, failure)) {
        if (reason != nullptr)
            *reason = failure;
        return faces;
    }

    Vec3 queryNormal, queryOrigin;
    double queryLow[3] = {}, queryHigh[3] = {};
    if (query != nullptr) {
        double area = 0;
        if (!BuildQuery (*query, tolerance, queryNormal, queryOrigin, area, failure)) {
            if (reason != nullptr)
                *reason = failure;
            return faces;
        }
        bool first = true;
        const auto bounds = [&] (const std::vector<double>& points) {
            for (size_t i = 0; i + 2 < points.size (); i += 3) {
                for (int axis = 0; axis < 3; ++axis) {
                    queryLow[axis] = first ? points[i + axis] : std::min (queryLow[axis], points[i + axis]);
                    queryHigh[axis] = first ? points[i + axis] : std::max (queryHigh[axis], points[i + axis]);
                }
                first = false;
            }
        };
        bounds (query->points);
        for (const auto& piece : query->pieces)
            bounds (piece);
    }

    const size_t triangleCount = mesh.triangles.size () / 3;
    if (triangleCount == 0)
        return faces;

    std::map<std::pair<int32_t, int32_t>, std::vector<uint32_t>> grouped;
    for (size_t tri = 0; tri < triangleCount; ++tri) {
        const int32_t body = mesh.triSourceBody[tri];
        const int32_t pgon = mesh.triSourcePgon[tri];
        grouped[std::make_pair (body, pgon)].push_back (static_cast<uint32_t> (tri));
    }

    faces.reserve (grouped.size ());
    for (auto& entry : grouped) {
        if (query != nullptr) {
            double low[3] = {}, high[3] = {};
            bool onQueryPlane = true;
            bool first = true;
            for (uint32_t tri : entry.second) {
                for (int corner = 0; corner < 3; ++corner) {
                    const Vec3 point = At (mesh.vertices, mesh.triangles[tri * 3 + corner]);
                    const double xyz[3] = { point.x, point.y, point.z };
                    const double distance = Dot (Sub (point, queryOrigin), queryNormal);
                    if (!std::isfinite (distance)) {
                        if (reason != nullptr)
                            *reason = "candidate plane distance overflowed";
                        return {};
                    }
                    onQueryPlane &= std::fabs (distance) <= tolerance.planeDistance;
                    for (int axis = 0; axis < 3; ++axis) {
                        low[axis] = first ? xyz[axis] : std::min (low[axis], xyz[axis]);
                        high[axis] = first ? xyz[axis] : std::max (high[axis], xyz[axis]);
                    }
                    first = false;
                }
            }
            // Conservative broadphase: projected grid displacement plus the
            // allowed plane offset must not exclude a potential boundary match.
            const double margin = 3 * std::max (tolerance.vertexTolerance, tolerance.planeDistance);
            // A complete planar face match requires every source vertex to lie
            // on the query plane. Merely touching/straddling it admits unrelated
            // reveals with sliver triangles and would still poison the query.
            bool candidate = onQueryPlane;
            for (int axis = 0; axis < 3; ++axis)
                candidate &= low[axis] <= queryHigh[axis] + margin && high[axis] >= queryLow[axis] - margin;
            if (!candidate)
                continue;
        }
        SourceFace face;
        face.sourceBody = entry.first.first;
        face.sourcePgon = entry.first.second;
        face.triangles = entry.second;
        if (!BuildFace (mesh, face.triangles, tolerance, face, failure)) {
            if (reason != nullptr)
                *reason = "face (" + std::to_string (face.sourceBody) + ", " + std::to_string (face.sourcePgon) +
                          "): " + failure;
            return {};
        }
        faces.push_back (std::move (face));
    }
    return faces;
}

} // namespace

std::vector<SourceFace> EnumerateSourceFaces (const Mesh& mesh, std::string* reason, const MatchTolerance& tolerance)
{
    return EnumerateFaces (mesh, reason, tolerance, nullptr);
}

std::vector<SourceFace> EnumerateQuerySourceFaces (const Mesh& mesh, const FaceQuery& query, std::string* reason,
                                                   const MatchTolerance& tolerance)
{
    return EnumerateFaces (mesh, reason, tolerance, &query);
}

bool AnalyticArea (const Mesh& mesh, int32_t sourceBody, int32_t sourcePgon, double* areaM2, std::string* reason,
                   const MatchTolerance& tolerance)
{
    if (reason != nullptr)
        reason->clear ();
    if (areaM2 == nullptr) {
        if (reason != nullptr)
            *reason = "null output";
        return false;
    }
    std::string failure;
    const std::vector<SourceFace> faces = EnumerateSourceFaces (mesh, &failure, tolerance);
    if (!failure.empty ()) {
        if (reason != nullptr)
            *reason = failure;
        return false;
    }
    for (const SourceFace& face : faces) {
        if (face.sourceBody == sourceBody && face.sourcePgon == sourcePgon) {
            *areaM2 = face.areaM2;
            return true;
        }
    }
    if (reason != nullptr)
        *reason = "no face with body " + std::to_string (sourceBody) + " polygon " + std::to_string (sourcePgon);
    return false;
}

MatchResult Resolve (const std::vector<SourceFace>& faces, const FaceQuery& query, const MatchTolerance& tolerance)
{
    MatchResult result;
    std::string reason;
    if (!ValidTolerance (tolerance, reason)) {
        result.status = MatchStatus::Invalid;
        result.reason = reason;
        return result;
    }
    if (query.expectedBody < 0 || query.expectedPgon < 0) {
        result.status = MatchStatus::Invalid;
        result.reason = "expected ids must not be negative";
        return result;
    }

    Vec3 normal;
    Vec3 origin;
    double queryArea = 0.0;
    if (!BuildQuery (query, tolerance, normal, origin, queryArea, reason)) {
        result.status = MatchStatus::Invalid;
        result.reason = reason;
        return result;
    }

    const double normalArray[3] = { normal.x, normal.y, normal.z };
    const double originArray[3] = { origin.x, origin.y, origin.z };
    boundary::Frame frame;
    if (!boundary::MakeFrame (normalArray, originArray, frame, reason)) {
        result.status = MatchStatus::Invalid;
        result.reason = reason;
        return result;
    }

    std::vector<std::vector<double>> queryPieces;
    if (!query.points.empty ())
        queryPieces.push_back (query.points);
    for (const std::vector<double>& piece : query.pieces)
        if (!piece.empty ())
            queryPieces.push_back (piece);

    std::vector<boundary::Loop> queryLoops;
    if (!boundary::BuildCanonicalLoops (queryPieces, frame, tolerance.vertexTolerance, queryLoops, reason)) {
        result.status = MatchStatus::Invalid;
        result.reason = "query: " + reason;
        return result;
    }

    std::vector<size_t> candidates;
    for (size_t index = 0; index < faces.size (); ++index) {
        const SourceFace& face = faces[index];
        const Vec3 faceNormal { face.normal[0], face.normal[1], face.normal[2] };
        if (Length (faceNormal) <= kEps)
            continue;
        if (Dot (normal, faceNormal) < tolerance.normalDot)
            continue; // the world orientation must match
        const Vec3 faceCentroid { face.centroid[0], face.centroid[1], face.centroid[2] };
        if (std::fabs (Dot (Sub (faceCentroid, origin), normal)) > tolerance.planeDistance)
            continue; // not on the query plane

        std::vector<boundary::Loop> faceLoops;
        std::string faceReason;
        if (!boundary::BuildCanonicalLoops (FacePieces (face.trianglePoints), frame, tolerance.vertexTolerance,
                                            faceLoops, faceReason)) {
            result.status = MatchStatus::Invalid;
            result.reason = "face (" + std::to_string (face.sourceBody) + ", " + std::to_string (face.sourcePgon) +
                            "): " + faceReason;
            return result;
        }
        if (faceLoops == queryLoops)
            candidates.push_back (index);
    }

    if (candidates.empty ()) {
        result.status = MatchStatus::NoMatch;
        result.reason = "the complete boundary matched no source face";
        return result;
    }
    if (candidates.size () > 1) {
        result.status = MatchStatus::Ambiguous;
        result.reason = "the complete boundary matched " + std::to_string (candidates.size ()) + " distinct faces";
        return result;
    }

    const SourceFace& match = faces[candidates[0]];
    if ((query.expectedBody != 0 && match.sourceBody != query.expectedBody) ||
        (query.expectedPgon != 0 && match.sourcePgon != query.expectedPgon)) {
        result.status = MatchStatus::Invalid;
        result.reason = "expected ids contradict the uniquely matched face";
        return result;
    }

    result.status = MatchStatus::Ok;
    result.face = match;
    result.reason = "matched body " + std::to_string (match.sourceBody) + " polygon " +
                    std::to_string (match.sourcePgon) + " by complete boundary";
    return result;
}

MatchResult Resolve (const Mesh& mesh, const FaceQuery& query, const MatchTolerance& tolerance)
{
    std::string failure;
    const std::vector<SourceFace> faces = EnumerateQuerySourceFaces (mesh, query, &failure, tolerance);
    if (!failure.empty ()) {
        MatchResult result;
        result.status = MatchStatus::Invalid;
        result.reason = failure;
        return result;
    }
    return Resolve (faces, query, tolerance);
}

} // namespace surfacemap
} // namespace geomsrv
