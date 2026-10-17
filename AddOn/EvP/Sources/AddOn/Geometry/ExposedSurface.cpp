#include "ExposedSurfaceInternal.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <iomanip>
#include <sstream>

namespace geomsrv::exposed {
namespace {
using namespace detail;
namespace cp = Clipper2Lib;
constexpr int kPrecision = surfacecoverage::kPrecision;

bool Fail (std::string& error, const char* reason)
{
    error = reason;
    return false;
}

void Add (Areas& sum, const Areas& value)
{
    sum.grossGeometricArea += value.grossGeometricArea;
    sum.exposedArea += value.exposedArea;
    sum.selectedExposedArea += value.selectedExposedArea;
}

bool Coverage (const Face& face, const std::vector<Face>& faces, size_t faceIndex, const std::vector<Body>& covers,
               double contactTolerance, cp::PathsD& paths, Work& work, std::string& error)
{
    for (const auto& cover : covers) {
        if (!work.Check (1, error))
            return false;
        if (cover.key == face.key.body || !Overlaps (face.bounds, cover.mesh.bounds, contactTolerance))
            continue; // own operated source body already has its internal faces removed
        ++work.statistics.candidateCoverPairs;
        if (!work.Check (cover.mesh.TriangleCount () + cover.mesh.VertexCount (), error))
            return false;
        cp::PathsD section;
        if (!surfacecoverage::Section (cover.mesh, face.frame, section, error))
            return false;
        if (!section.empty ())
            ++work.statistics.sectionCount;
        paths.insert (paths.end (), section.begin (), section.end ());
        if (contactTolerance > 0 && !NearContact (face, cover, contactTolerance, paths, work, error))
            return false;
    }
    // Own same-facing overlapping source patches once, BEFORE material filtering.
    // Different materials cannot be silently assigned to an arbitrary owner.
    for (size_t i = 0; i < faceIndex; ++i) {
        if (!work.Check (1, error))
            return false;
        const auto& previous = faces[i];
        if (!Overlaps (face.bounds, previous.bounds) ||
            engine::Dot (face.frame.normal, previous.frame.normal) < 1 - 1e-10)
            continue;
        cp::PathsD projected;
        bool coplanar = true;
        const auto& points = previous.source.trianglePoints;
        if (!work.Check (points.size () / 3, error))
            return false;
        for (size_t j = 0; j < points.size (); j += 9) {
            cp::PathD triangle;
            for (size_t k = 0; k < 3; ++k) {
                const V p = Point (points, j / 3 + k);
                const V d { p.x - face.frame.origin.x, p.y - face.frame.origin.y, p.z - face.frame.origin.z };
                coplanar &= std::abs (engine::Dot (d, face.frame.normal)) <= surfacecoverage::kPlaneTolerance;
                triangle.push_back (face.frame.Project (p));
            }
            projected.push_back (std::move (triangle));
        }
        if (!coplanar)
            continue;
        if (previous.material != face.material) {
            cp::PathsD overlap;
            if (!surfacecoverage::Boolean (cp::ClipType::Intersection, face.subject, projected, overlap, error))
                return false;
            if (std::abs (cp::Area (overlap)) > 1e-12)
                return Fail (error,
                             "Coincident source faces have conflicting surface materials; ownership is ambiguous.");
        }
        paths.insert (paths.end (), projected.begin (), projected.end ());
    }
    return work.Check (0, error);
}

bool Emit (const cp::PathsD& loops, double grid, const Face& face, bool selected, Result& result, FaceAreas& row,
           Work& work, std::string& error)
{
    size_t vertices = 0;
    for (const auto& loop : loops)
        vertices += loop.size ();
    if (!work.Check (vertices, error))
        return false;
    cp::PathsD triangles;
    if (!surfacecoverage::TriangulateOnGrid (loops, grid, triangles, error))
        return false;
    if (triangles.size () > work.limits.maxOutputTriangles ||
        work.statistics.outputTriangles > work.limits.maxOutputTriangles - triangles.size ())
        return Fail (error, "Exposed fragment output budget exceeded; no partial result.");
    work.statistics.outputTriangles += triangles.size ();
    Fragment fragment;
    fragment.source = face.key;
    fragment.materialPoolIndex = face.material;
    fragment.selected = selected;
    for (const auto& loop : loops) {
        std::vector<double> world;
        for (const auto& p : loop) {
            const V lifted = face.frame.Lift (p);
            world.insert (world.end (), { lifted.x, lifted.y, lifted.z });
        }
        fragment.loops.push_back (std::move (world));
    }
    for (auto& triangle : triangles) {
        if (!work.Check (1, error))
            return false;
        if (triangle.size () != 3)
            return Fail (error, "Fragment triangulator returned a non-triangle.");
        if (cp::Area (triangle) < 0)
            std::swap (triangle[1], triangle[2]);
        const V a = face.frame.Lift (triangle[0]), b = face.frame.Lift (triangle[1]), c = face.frame.Lift (triangle[2]);
        const V ab { b.x - a.x, b.y - a.y, b.z - a.z }, ac { c.x - a.x, c.y - a.y, c.z - a.z };
        const V cross = engine::Cross (ab, ac);
        const double area = std::sqrt (engine::Dot (cross, cross)) / 2;
        if (!std::isfinite (area) || area <= 0 || engine::Dot (cross, face.frame.normal) <= 0)
            return Fail (error, "Exposed fragment world triangle is degenerate or inconsistently wound.");
        fragment.areaM2 += area;
        for (const V& p : { a, b, c }) {
            fragment.triangles.push_back (static_cast<uint32_t> (fragment.points.size () / 3));
            fragment.points.insert (fragment.points.end (), { p.x, p.y, p.z });
        }
    }
    const double planarArea = cp::Area (loops);
    if (!std::isfinite (planarArea) || planarArea <= 0 ||
        std::abs (planarArea - fragment.areaM2) > 1e-12 + 1e-6 * planarArea) {
        std::ostringstream detail;
        detail << std::setprecision (17)
               << "Fragment triangulation area disagrees with its clipped boundary: " << fragment.areaM2 << " vs "
               << planarArea << " m2 (" << triangles.size () << " triangles).";
        error = detail.str ();
        return false;
    }
    row.areas.exposedArea += fragment.areaM2;
    if (selected)
        row.areas.selectedExposedArea += fragment.areaM2;
    result.fragments.push_back (std::move (fragment));
    return true;
}

bool EmitTree (const cp::PolyPathD& tree, const Face& face, bool selected, Result& result, FaceAreas& row, Work& work,
               std::string& error)
{
    for (const auto& child : tree) {
        if (!work.Check (1, error))
            return false;
        if (!child->IsHole ()) {
            cp::PathsD loops { child->Polygon () };
            for (const auto& hole : *child)
                loops.push_back (hole->Polygon ());
            if (!Emit (loops, child->Scale (), face, selected, result, row, work, error))
                return false;
        }
        if (!EmitTree (*child, face, selected, result, row, work, error))
            return false;
    }
    return true;
}

bool Run (const Snapshot& snapshot, const Request& request, Result& result, Work& work, std::string& error)
{
    std::vector<Face> faces;
    std::vector<Body> covers;
    if (!Prepare (snapshot, request, faces, covers, work, error))
        return false;
    result.snapshotId = snapshot.id;
    result.captureStamp = snapshot.captureStamp;
    result.coverScopeId = request.coverScopeId;
    result.contactToleranceM = request.contactToleranceM;
    for (const auto& body : covers)
        result.covers.push_back (body.key);
    for (size_t i = 0; i < faces.size (); ++i) {
        const auto& face = faces[i];
        cp::PathsD coverage, normalized;
        if (!Coverage (face, faces, i, covers, request.contactToleranceM, coverage, work, error) ||
            !surfacecoverage::Boolean (cp::ClipType::Union, coverage, {}, normalized, error))
            return false;
        cp::ClipperD clipper (kPrecision);
        clipper.AddSubject (face.subject);
        clipper.AddClip (normalized);
        cp::PolyTreeD fragments;
        if (!clipper.Execute (cp::ClipType::Difference, cp::FillRule::NonZero, fragments) || clipper.ErrorCode ())
            return Fail (error, "Exposed surface difference failed; no partial result.");
        const bool selected = !request.materialPoolFilter || request.materialPoolFilter->contains (face.material);
        FaceAreas row { face.key, face.material, { face.source.areaM2, 0, 0 } };
        if (!EmitTree (fragments, face, selected, result, row, work, error)) {
            error = "Face " + snapshot.meshes[face.key.body.meshIndex].guid + " / body " +
                    std::to_string (face.key.body.sourceBody) + " / polygon " + std::to_string (face.key.sourcePgon) +
                    ": " + error;
            return false;
        }
        if (row.areas.exposedArea > row.areas.grossGeometricArea + 1e-12 + 1e-6 * row.areas.grossGeometricArea)
            return Fail (error, "Exposed fragments exceed their source area.");
        result.faces.push_back (row);
        Add (result.areas, row.areas);
        Add (result.byMaterialPool[face.material], row.areas);
        Add (result.byElementGuid[snapshot.meshes[face.key.body.meshIndex].guid], row.areas);
    }
    if (!std::isfinite (result.areas.grossGeometricArea) || !std::isfinite (result.areas.exposedArea) ||
        !std::isfinite (result.areas.selectedExposedArea))
        return Fail (error, "Exposed-surface totals are non-finite.");
    result.statistics = work.statistics;
    return work.Check (0, error);
}
} // namespace

bool Calculate (const Snapshot& snapshot, const Request& request, Result& result, std::string& error,
                const Limits& limits, const std::function<bool ()>& cancelled)
{
    result = {};
    error.clear ();
    try {
        detail::Work work { limits, cancelled };
        Result prepared;
        if (!Run (snapshot, request, prepared, work, error))
            return false;
        result = std::move (prepared);
        return true;
    }
    catch (const std::exception& exception) {
        error = std::string ("Exposed-surface calculation refused: ") + exception.what ();
        return false;
    }
}
} // namespace geomsrv::exposed
