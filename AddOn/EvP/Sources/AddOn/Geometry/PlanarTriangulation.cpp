#include "Geometry/PlanarTriangulation.hpp"

#include <include/earstrip.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace geomsrv::engine {
namespace {

using Triangulator = earstrip::Triangulator<uint32_t>;

thread_local Triangulator s_triangulator;

bool Fail (std::string& error, std::string message)
{
    error = std::move (message);
    return false;
}

double Cross (double ax, double ay, double bx, double by, double cx, double cy)
{
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

double RingArea (const PlanarRing& ring)
{
    if (ring.count < 3)
        return 0.0;
    // A ring-local origin keeps survey offsets out of the shoelace products.
    const double originX = ring.xy[0];
    const double originY = ring.xy[1];
    double sum = 0.0;
    double compensation = 0.0;
    for (size_t index = 0; index < ring.count; ++index) {
        const size_t next = (index + 1) % ring.count;
        const double term = Cross (originX, originY, ring.xy[index * 2], ring.xy[index * 2 + 1], ring.xy[next * 2],
                                   ring.xy[next * 2 + 1]);
        const double corrected = term - compensation;
        const double total = sum + corrected;
        compensation = (total - sum) - corrected;
        sum = total;
    }
    return std::abs (sum) * 0.5;
}

enum class PointLocation { Outside, Boundary, Inside };

PointLocation LocatePoint (const PlanarRing& ring, double x, double y)
{
    int winding = 0;
    for (size_t index = 0; index < ring.count; ++index) {
        const size_t next = (index + 1) % ring.count;
        const double ax = ring.xy[index * 2];
        const double ay = ring.xy[index * 2 + 1];
        const double bx = ring.xy[next * 2];
        const double by = ring.xy[next * 2 + 1];
        const double dx = bx - ax;
        const double dy = by - ay;
        const double length2 = dx * dx + dy * dy;
        if (length2 > 0.0) {
            const double projection = ((x - ax) * dx + (y - ay) * dy) / length2;
            if (projection >= 0.0 && projection <= 1.0) {
                const double nearestX = ax + projection * dx;
                const double nearestY = ay + projection * dy;
                const double tolerance = 1.0e-12 * std::max (1.0, std::sqrt (length2));
                const double distanceX = x - nearestX;
                const double distanceY = y - nearestY;
                if (distanceX * distanceX + distanceY * distanceY <= tolerance * tolerance)
                    return PointLocation::Boundary;
            }
        }
        else if (x == ax && y == ay) {
            return PointLocation::Boundary;
        }

        const double side = Cross (ax, ay, bx, by, x, y);
        if (ay <= y) {
            if (by > y && side > 0.0)
                ++winding;
        }
        else if (by <= y && side < 0.0) {
            --winding;
        }
    }
    return winding == 0 ? PointLocation::Outside : PointLocation::Inside;
}

bool HolesAreContained (const std::vector<PlanarRing>& rings, std::string& error)
{
    for (size_t ringIndex = 1; ringIndex < rings.size (); ++ringIndex) {
        const PlanarRing& hole = rings[ringIndex];
        for (size_t index = 0; index < hole.count; ++index) {
            const double x = hole.xy[index * 2];
            const double y = hole.xy[index * 2 + 1];
            if (LocatePoint (rings[0], x, y) == PointLocation::Outside) {
                return Fail (error, "HoleClassificationFailed: hole " + std::to_string (ringIndex) +
                                        " has a vertex outside the outer ring");
            }
            // Also catch an edge leaving a concave outer ring between two
            // contained vertices. Crossings may be repaired only when the hole's
            // declared domain remains inside the outer boundary.
            const size_t next = (index + 1) % hole.count;
            const double midpointX = (x + hole.xy[next * 2]) * 0.5;
            const double midpointY = (y + hole.xy[next * 2 + 1]) * 0.5;
            if (LocatePoint (rings[0], midpointX, midpointY) == PointLocation::Outside) {
                return Fail (error, "HoleClassificationFailed: hole " + std::to_string (ringIndex) +
                                        " crosses outside the outer ring");
            }
        }
    }
    return true;
}

bool IndicesAreValid (const std::vector<uint32_t>& indices, size_t vertexCount)
{
    return std::all_of (indices.begin (), indices.end (),
                        [vertexCount] (uint32_t index) { return index < vertexCount; });
}

bool ValidateStrips (const PlanarTriangulation& result, size_t vertexCount)
{
    if (result.stripEnds.size () != result.stripClockwise.size () || !IndicesAreValid (result.strips, vertexCount))
        return false;
    size_t begin = 0;
    for (uint32_t end : result.stripEnds) {
        if (end < begin + 3 || end > result.strips.size ())
            return false;
        begin = end;
    }
    return begin == result.strips.size ();
}

bool ValidateTriangles (const std::vector<double>& coordinates, PlanarTriangulation& result, double expectedArea,
                        bool checkDeclaredArea, bool hasHoles, std::string& error)
{
    if (result.triangles.empty () || result.triangles.size () % 3 != 0)
        return Fail (error, "TriangulationDegenerate: the polygon produced no triangle cover");
    if (result.generatedXY.size () % 2 != 0)
        return Fail (error, "TriangulationDegenerate: generated points are not x,y pairs");
    for (double coordinate : result.generatedXY) {
        if (!std::isfinite (coordinate))
            return Fail (error, "TriangulationDegenerate: a generated point is not finite");
    }

    const size_t generatedCount = result.generatedXY.size () / 2;
    const size_t inputCount = coordinates.size () / 2;
    if (generatedCount > std::numeric_limits<uint32_t>::max () - inputCount)
        return Fail (error, "TriangulationDegenerate: the result has too many vertices");
    const size_t vertexCount = inputCount + generatedCount;
    if (!IndicesAreValid (result.triangles, vertexCount))
        return Fail (error, "TriangulationDegenerate: a triangle index is outside the input and generated vertices");

    std::vector<double> allCoordinates = coordinates;
    allCoordinates.insert (allCoordinates.end (), result.generatedXY.begin (), result.generatedXY.end ());
    double coveredArea = 0.0;
    double compensation = 0.0;
    for (size_t index = 0; index < result.triangles.size (); index += 3) {
        const size_t a = static_cast<size_t> (result.triangles[index]) * 2;
        const size_t b = static_cast<size_t> (result.triangles[index + 1]) * 2;
        const size_t c = static_cast<size_t> (result.triangles[index + 2]) * 2;
        const double doubledArea = Cross (allCoordinates[a], allCoordinates[a + 1], allCoordinates[b],
                                          allCoordinates[b + 1], allCoordinates[c], allCoordinates[c + 1]);
        if (!std::isfinite (doubledArea) || doubledArea <= 0.0)
            return Fail (error, "TriangulationDegenerate: a triangle is not counter-clockwise with positive area");
        const double term = doubledArea * 0.5 - compensation;
        const double total = coveredArea + term;
        compensation = (total - coveredArea) - term;
        coveredArea = total;
    }
    if (!std::isfinite (coveredArea))
        return Fail (error, "TriangulationDegenerate: the covered area is not finite");

    if (checkDeclaredArea) {
        const double tolerance = 1.0e-9 * std::max (expectedArea, coveredArea);
        if (coveredArea > expectedArea + tolerance && hasHoles)
            return Fail (error, "HoleClassificationFailed: the covered area grew, so a hole was not preserved");
        if (std::abs (coveredArea - expectedArea) > tolerance)
            return Fail (error, "TriangulationConstraint: the covered area does not match the declared rings");
    }
    return true;
}

} // namespace

bool TriangulatePlanar (const std::vector<PlanarRing>& rings, bool wantStrips, PlanarTriangulation& out,
                        std::string& error)
{
    if (rings.empty ())
        return Fail (error, "TriangulationDegenerate: an outer ring is required");

    size_t inputCount = 0;
    // The ear path stores ring IDs in uint16_t and reserves two bridge nodes
    // per ring in uint32_t arithmetic. Validate descriptors before reading XY.
    if (rings.size () > std::numeric_limits<uint16_t>::max ())
        return Fail (error, "TriangulationDegenerate: the polygon has too many rings");
    const size_t maxInput = std::numeric_limits<uint32_t>::max () - rings.size () * 2;
    for (const PlanarRing& ring : rings) {
        if (ring.xy == nullptr || ring.count < 3)
            return Fail (error, "TriangulationDegenerate: every ring needs at least three x,y points");
        if (ring.count > maxInput - inputCount)
            return Fail (error, "TriangulationDegenerate: the polygon has too many vertices");
        inputCount += ring.count;
    }
    for (const PlanarRing& ring : rings) {
        for (size_t index = 0; index < ring.count * 2; ++index) {
            if (!std::isfinite (ring.xy[index]))
                return Fail (error, "TriangulationDegenerate: every coordinate must be finite");
        }
    }

    double expectedArea = RingArea (rings[0]);
    for (size_t ringIndex = 1; ringIndex < rings.size (); ++ringIndex) {
        const double holeArea = RingArea (rings[ringIndex]);
        // Upstream's ear path silently skips a zero-signed-area hole without
        // setting holesLeftOut. Its non-zero-winding repair would need a domain
        // contract, not an accidental filled hole.
        if (holeArea == 0.0)
            return Fail (error,
                         "HoleClassificationFailed: hole " + std::to_string (ringIndex) + " has no declared area");
        expectedArea -= holeArea;
    }
    if (!std::isfinite (expectedArea))
        return Fail (error, "TriangulationDegenerate: the declared ring area is not finite");
    if (!HolesAreContained (rings, error))
        return false;

    std::vector<double> coordinates;
    coordinates.reserve (inputCount * 2);
    std::vector<uint32_t> holeStarts;
    holeStarts.reserve (rings.size () - 1);
    for (size_t ringIndex = 0; ringIndex < rings.size (); ++ringIndex) {
        if (ringIndex > 0)
            holeStarts.push_back (static_cast<uint32_t> (coordinates.size () / 2));
        const PlanarRing& ring = rings[ringIndex];
        coordinates.insert (coordinates.end (), ring.xy, ring.xy + ring.count * 2);
    }

    s_triangulator.setTriangleList (!wantStrips);
    const earstrip::Status status =
        s_triangulator.run (coordinates.data (), inputCount, 2, holeStarts.data (), holeStarts.size ());
    if (status == earstrip::Status::HolesLeftOut || s_triangulator.holesLeftOut ())
        return Fail (error, "HoleClassificationFailed: a hole could not be joined to the outer ring");

    PlanarTriangulation result;
    switch (status) {
        case earstrip::Status::Exact:
            result.cover = PlanarCover::Exact;
            break;
        case earstrip::Status::Constrained:
            result.cover = PlanarCover::Repaired;
            break;
        case earstrip::Status::Approximate:
            result.cover = PlanarCover::Approximate;
            break;
        case earstrip::Status::Refused:
            return Fail (error, "TriangulationDegenerate: the polygon could not be covered");
        case earstrip::Status::HolesLeftOut:
            return Fail (error, "HoleClassificationFailed: a hole could not be joined to the outer ring");
        default:
            return Fail (error, "TriangulationDegenerate: unknown triangulation status");
    }

    result.generatedXY = s_triangulator.extraPoints ();
    if (wantStrips) {
        result.strips = s_triangulator.strips ();
        result.stripEnds = s_triangulator.stripEnds ();
        result.stripClockwise = s_triangulator.stripClockwise ();
        s_triangulator.triangles (result.triangles);
    }
    else {
        result.triangles = s_triangulator.triangleList ();
    }

    // A repaired single outer ring can intentionally have non-zero-winding area
    // different from its shoelace area (for example a bow-tie or a doubled ring).
    // With holes, equality remains mandatory because growth can reveal a hole
    // silently dropped by the constrained path.
    const bool checkDeclaredArea = result.cover == PlanarCover::Exact || rings.size () > 1;
    if (!ValidateTriangles (coordinates, result, expectedArea, checkDeclaredArea, rings.size () > 1, error))
        return false;
    if (wantStrips && !ValidateStrips (result, inputCount + result.generatedXY.size () / 2))
        return Fail (error, "TriangulationDegenerate: invalid strip output");

    out = std::move (result);
    error.clear ();
    return true;
}

void ShrinkPlanarTriangulationScratch ()
{
    s_triangulator.shrink ();
}

} // namespace geomsrv::engine
