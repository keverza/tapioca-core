#include "Edit/EditLandscapeInternal.hpp"
#include "Geometry/PlanarTriangulation.hpp"
#include <clipper2/clipper.h>
#include <clipper2/clipper.triangulation.h>
#include <cmath>
#include <algorithm>
#include <tuple>
#include <utility>

namespace tapioca::geometry {
namespace edit = geomsrv::edit;

bool LandscapeSourcePreview (const edit::Document& doc, const LandscapeSource& source, std::vector<Vec3>& points,
                             bool& closed, std::string& error)
{
    landscape_detail::SourceCurve curve;
    PlanarMap map;
    if (!landscape_detail::ReadSourceCurve (doc, source, curve, error) ||
        !BuildPlanarMap ({ curve.curve }, {}, map, error))
        return false;
    auto edges = map.edges;
    std::sort (edges.begin (), edges.end (), [] (const auto& a, const auto& b) {
        return std::tie (a.pieces.front ().piece, a.pieces.front ().sourceStart) <
               std::tie (b.pieces.front ().piece, b.pieces.front ().sourceStart);
    });
    points.clear ();
    for (const auto& edge : edges) {
        const auto& piece = edge.pieces.front ();
        Vec3 a = map.vertices[piece.reversed ? edge.b : edge.a], b = map.vertices[piece.reversed ? edge.a : edge.b];
        const auto& controls = curve.curve.points;
        const Vec3 from = controls[piece.piece], to = controls[(piece.piece + 1) % controls.size ()];
        a.z = edit::Lerp (from, to, piece.sourceStart).z;
        b.z = edit::Lerp (from, to, piece.sourceEnd).z;
        if (points.empty ())
            points.push_back (a);
        points.push_back (b);
    }
    closed = curve.curve.closed;
    if (closed && !points.empty ())
        points.pop_back ();
    return true;
}

bool SeedLandscapeDemo (edit::Document& doc, double z, std::string& error)
{
    if (doc.landscape || !doc.entities.empty () || !doc.paths.empty () || !std::isfinite (z)) {
        error = "Landscape demo requires an empty virtual document and finite elevation.";
        return false;
    }
    edit::Document candidate = doc;
    // Reproducible diagnostic UUID, not a native project/session identifier.
    if (!StartLandscape (
            candidate,
            { 0x48, 0x34, 0x64, 0x65, 0x6d, 0x6f, 0x40, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 }, error))
        return false;
    const auto source = [&] (std::vector<Vec3> points, bool closed, SourceRole role, bool circle = false) {
        for (auto& point : points)
            point.z = z;
        LandscapeSource s;
        s.entityId = edit::AddPolyline (candidate, points, closed, circle ? "circle cutter" : "landscape source");
        s.role = role;
        if (circle)
            s.arcAngles.assign (4, 1.57079632679489661923);
        Id id = 0;
        return AddLandscapeSource (candidate, s, id, error) &&
               (role != SourceRole::PropertyLine || SetLandscapePropertyLine (candidate, id, error));
    };
    if (!source ({ { -12, -8, 0 }, { 12, -8, 0 }, { 12, 8, 0 }, { -12, 8, 0 } }, true, SourceRole::PropertyLine) ||
        !source ({ { -8, -4, 0 }, { 4, -4, 0 }, { 4, 4, 0 }, { -8, 4, 0 } }, true, SourceRole::Guide) ||
        !source ({ { 5, 0, 0 }, { 2, 3, 0 }, { -1, 0, 0 }, { 2, -3, 0 } }, true, SourceRole::Cutter, true) ||
        !source ({ { -10, 0, 0 }, { 10, 0, 0 } }, false, SourceRole::Guide) ||
        !source ({ { 6, -3, 0 }, { 10, -3, 0 }, { 10, 3, 0 }, { 6, 3, 0 } }, true, SourceRole::Guide) ||
        !source ({ { -10, -6, 0 }, { -2, 6, 0 } }, false, SourceRole::Guide))
        return false;
    LandscapeResult result;
    if (!RebuildLandscape (candidate, result, error))
        return false;
    doc = std::move (candidate);
    return true;
}

bool BuildLandscapePreviewMesh (const DerivedRegion& region, LandscapePreviewMesh& mesh, std::string& error)
{
    constexpr const char* warning = "Landscape preview triangulation failed; outlines only.";
    if (!std::isfinite (region.seed.z)) {
        error = std::string (warning) + " Preview elevation must be finite.";
        return false;
    }
    std::vector<std::vector<double>> coordinates;
    coordinates.reserve (region.rings.size ());
    for (const auto& ring : region.rings) {
        auto& xy = coordinates.emplace_back ();
        xy.reserve (ring.size () * 2);
        for (const auto& point : ring)
            xy.insert (xy.end (), { point.x, point.y });
    }
    std::vector<geomsrv::engine::PlanarRing> rings;
    rings.reserve (coordinates.size ());
    for (const auto& xy : coordinates)
        rings.push_back ({ xy.data (), xy.size () / 2 });

    geomsrv::engine::PlanarTriangulation triangulation;
    std::string detail;
    if (!geomsrv::engine::TriangulatePlanar (rings, false, triangulation, detail)) {
        error = std::string (warning) + " " + detail;
        return false;
    }
    // Many valid holes select the constrained path. The adapter checks declared
    // area for every cover with holes; a repaired single outer lacks that gate
    // (its non-zero fill could differ from its shoelace area), so refuse it.
    const bool checkedConstrained = triangulation.cover == geomsrv::engine::PlanarCover::Repaired && rings.size () > 1;
    if ((triangulation.cover != geomsrv::engine::PlanarCover::Exact && !checkedConstrained) ||
        !triangulation.generatedXY.empty ()) {
        error = std::string (warning) + " A complete cover without generated vertices is required.";
        return false;
    }

    LandscapePreviewMesh prepared;
    size_t count = 0;
    for (const auto& ring : region.rings)
        count += ring.size ();
    prepared.vertices.reserve (count);
    for (const auto& ring : region.rings)
        for (const auto& point : ring)
            prepared.vertices.push_back ({ point.x, point.y, region.seed.z });
    prepared.triangles = std::move (triangulation.triangles);
    mesh = std::move (prepared);
    error.clear ();
    return true;
}

bool LandscapePreviewTriangles (const DerivedRegion& region, std::vector<Vec3>& triangles, std::string& error)
{
    namespace cp = Clipper2Lib;
    cp::PathsD paths;
    for (const auto& ring : region.rings) {
        cp::PathD path;
        for (const auto& p : ring)
            path.emplace_back (p.x, p.y);
        paths.push_back (std::move (path));
    }
    cp::PathsD result;
    if (cp::Triangulate (paths, 6, result) != cp::TriangulateResult::success) {
        error = "Landscape preview triangulation failed; outlines only.";
        return false;
    }
    std::vector<Vec3> prepared;
    for (auto& triangle : result) {
        if (cp::Area (triangle) < 0)
            std::reverse (triangle.begin (), triangle.end ());
        for (const auto& p : triangle)
            prepared.push_back ({ p.x, p.y, region.seed.z });
    }
    triangles = std::move (prepared);
    return true;
}
} // namespace tapioca::geometry
