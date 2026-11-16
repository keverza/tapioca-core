// Bound by OVERLAY-INVARIANTS.md: only layer content, camera-independent area.
#include "SurfaceTakeoffLayer.hpp"
#include "Geometry/GeometryEngine.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <sstream>

namespace geomsrv::archviz::surfacetakeoff {
namespace {
std::string Area (double value)
{
    std::ostringstream text;
    text << std::fixed << std::setprecision (3) << value;
    return text.str ();
}
} // namespace
bool Build (const Snapshot& snapshot, const exposed::Result& result, const exposed::SurfaceSelection& selection,
            const std::vector<int32_t>& surfaces, const std::map<int32_t, double>& nativeAreas,
            const std::string& comparisonNote, Presentation& output, std::string& error,
            const std::vector<uint32_t>& colors, bool allWalls)
{
    output = {};
    error.clear ();
    const auto fail = [&] (const char* message) {
        error = message;
        return false;
    };
    if (snapshot.id != result.snapshotId || snapshot.captureStamp != result.captureStamp || surfaces.empty () ||
        surfaces.size () > kMaxSurfaces)
        return fail ("Presentation capture/surface scope is invalid.");
    Presentation prepared;
    auto& layer = prepared.layer;
    layer.name = kLayer;
    layer.views = overlaylayers::Views::ThreeD;
    layer.occlusion = overlaylayers::Behind::Fade;
    for (const auto& entry : result.byElementGuid) {
        const auto mesh = std::find_if (snapshot.meshes.begin (), snapshot.meshes.end (),
                                        [&] (const auto& value) { return value.guid == entry.first; });
        if (mesh == snapshot.meshes.end ())
            return fail ("Missing element identity in presentation.");
        prepared.elements[entry.first] = { entry.first, surfacecriteria::TypeName (mesh->elemType) };
    }
    if (!colors.empty () && colors.size () != surfaces.size ())
        return fail ("Missing selected surface colors.");
    std::map<int32_t, size_t> positions;
    for (int32_t index : surfaces) {
        const auto found = snapshot.surfaceAttributes.find (index);
        if (found == snapshot.surfaceAttributes.end () || positions.contains (index))
            return fail ("Missing/duplicate selected project surface.");
        positions[index] = prepared.rows.size ();
        prepared.rows.push_back ({ index, found->second.name, found->second.rgba, 0 });
    }
    for (const auto& fragment : result.fragments) {
        const auto mapped = selection.poolToSurface.find (fragment.materialPoolIndex);
        if (mapped == selection.poolToSurface.end ())
            return fail ("Fragment has no captured surface mapping.");
        const bool wanted = positions.contains (mapped->second);
        if (fragment.selected != wanted)
            return fail ("Fragment selection differs from the overlay surface scope.");
        if (!wanted)
            continue;
        if (fragment.points.size () % 3 || fragment.triangles.empty () || fragment.triangles.size () % 3)
            return fail ("Invalid fragment mesh.");
        double area = 0;
        Facing facing = Facing::Side;
        for (size_t i = 0; i < fragment.triangles.size (); i += 3) {
            engine::Vector3 p[3];
            for (size_t j = 0; j < 3; ++j) {
                const size_t at = fragment.triangles[i + j] * size_t (3);
                if (at + 2 >= fragment.points.size ())
                    return fail ("Invalid fragment triangle index.");
                p[j] = { fragment.points[at], fragment.points[at + 1], fragment.points[at + 2] };
            }
            const engine::Vector3 a { p[1].x - p[0].x, p[1].y - p[0].y, p[1].z - p[0].z },
                b { p[2].x - p[0].x, p[2].y - p[0].y, p[2].z - p[0].z };
            const auto cross = engine::Cross (a, b);
            const double measured = std::sqrt (engine::Dot (cross, cross)) / 2;
            if (!std::isfinite (measured) || measured <= 0)
                return fail ("Invalid fragment triangle area.");
            if (i == 0) {
                const double normalZ = cross.z / (2 * measured);
                if (normalZ >= std::sqrt (.5))
                    facing = Facing::Top;
                else if (normalZ <= -std::sqrt (.5))
                    facing = Facing::Bottom;
            }
            area += measured;
        }
        if (!std::isfinite (fragment.areaM2) || std::abs (area - fragment.areaM2) > 1e-6)
            return fail ("Fragment and overlay triangle areas differ.");
        auto& row = prepared.rows[positions.at (mapped->second)];
        row.areaM2 += area;
        overlaylayers::Mesh mesh;
        mesh.points = fragment.points;
        mesh.indices = fragment.triangles;
        mesh.rgba = row.rgba;
        mesh.styled = true;
        mesh.style.opacity = .5f;
        mesh.style.behind = overlaylayers::Behind::Fade;
        mesh.style.occludedOpacity = kOccludedHighlightOpacity;
        mesh.style.cullBack = false; // both sides remain inspectable; hidden fill retains half its alpha
        mesh.style.edgeRgba = 0;     // BoundaryLines owns outlines across fragment/element seams.
        mesh.style.edgeWidthPixels = 1.5f;
        mesh.hoverTitle = row.name;
        mesh.hoverRows = { { "Highlighted", Area (area) + " m2" } };
        prepared.triangles += mesh.indices.size () / 3;
        prepared.areaM2 += area;
        layer.meshes.push_back (std::move (mesh));
        prepared.meshSurfaces.push_back (mapped->second);
        prepared.meshAreas.push_back (area);
        prepared.meshFacings.push_back (facing);
        prepared.meshElements.push_back (snapshot.meshes[fragment.source.body.meshIndex].guid);
    }
    if (!std::isfinite (result.areas.selectedExposedArea) ||
        std::abs (prepared.areaM2 - result.areas.selectedExposedArea) > 1e-6)
        return fail ("Panel and selected fragment totals differ.");
    prepared.scopeLabel = allWalls
                              ? "All captured wall faces: includes interiors, returns and reveals. Not exterior-only."
                              : "User-confirmed exterior seed: connected facade plane(s).";
    prepared.nativeAreas = nativeAreas;
    prepared.comparisonNote = comparisonNote;
    prepared.contactToleranceM = result.contactToleranceM;
    prepared.coverLabel = "Cover scope: " + result.coverScopeId.substr (0, result.coverScopeId.find (':')) + " (" +
                          std::to_string (result.covers.size ()) +
                          " bodies). Other walls/frames count only when supplied. Refresh after edits.";
    prepared.layer = UiLayer (prepared, InitialUi (prepared, "0"));
    const auto invalid = overlaylayers::Validate (layer);
    if (!invalid.empty ()) {
        error = invalid;
        return false;
    }
    output = std::move (prepared);
    return true;
}

bool BuildFromResult (const Snapshot& snapshot, const geomsrv::surfacetakeoff::Result& result, Presentation& output,
                      std::string& error)
{
    output = {};
    error.clear ();
    const auto fail = [&] (const char* message) {
        error = message;
        return false;
    };
    if (snapshot.id != result.snapshotId || snapshot.captureStamp != result.captureStamp || result.surfaces.empty () ||
        result.surfaces.size () > kMaxSurfaces)
        return fail ("Published presentation capture/surface scope is invalid.");

    Presentation prepared;
    auto& layer = prepared.layer;
    layer.name = kLayer;
    layer.views = overlaylayers::Views::ThreeD;
    layer.occlusion = overlaylayers::Behind::Fade;
    for (const auto& guid : result.regionElementGuids) {
        const auto mesh = std::find_if (snapshot.meshes.begin (), snapshot.meshes.end (),
                                        [&] (const auto& value) { return value.guid == guid; });
        if (mesh == snapshot.meshes.end ())
            return fail ("Missing published element identity.");
        prepared.elements[guid] = { guid, surfacecriteria::TypeName (mesh->elemType) };
    }
    std::map<int32_t, size_t> positions;
    for (const auto& row : result.surfaces) {
        const auto found = snapshot.surfaceAttributes.find (row.surfaceAttributeIndex);
        if (found == snapshot.surfaceAttributes.end () || found->second.name != row.name ||
            positions.contains (row.surfaceAttributeIndex) || row.rgba != found->second.rgba ||
            !std::isfinite (row.areaM2) || row.areaM2 < 0)
            return fail ("Invalid published project surface row.");
        positions[row.surfaceAttributeIndex] = prepared.rows.size ();
        prepared.rows.push_back ({ row.surfaceAttributeIndex, row.name, row.rgba, 0 });
    }
    for (const auto& fragment : result.meshes) {
        const auto position = positions.find (fragment.surfaceAttributeIndex);
        if (position == positions.end () ||
            !std::any_of (snapshot.meshes.begin (), snapshot.meshes.end (),
                          [&] (const auto& mesh) { return mesh.guid == fragment.guid; }) ||
            fragment.points.size () % 3 || fragment.triangles.empty () || fragment.triangles.size () % 3)
            return fail ("Invalid published fragment mesh.");
        double area = 0;
        Facing facing = Facing::Side;
        for (size_t i = 0; i < fragment.triangles.size (); i += 3) {
            engine::Vector3 points[3];
            for (size_t corner = 0; corner < 3; ++corner) {
                const size_t at = fragment.triangles[i + corner] * size_t (3);
                if (at + 2 >= fragment.points.size ())
                    return fail ("Invalid published triangle index.");
                points[corner] = { fragment.points[at], fragment.points[at + 1], fragment.points[at + 2] };
            }
            const engine::Vector3 a { points[1].x - points[0].x, points[1].y - points[0].y, points[1].z - points[0].z },
                b { points[2].x - points[0].x, points[2].y - points[0].y, points[2].z - points[0].z };
            const auto cross = engine::Cross (a, b);
            const double measured = std::sqrt (engine::Dot (cross, cross)) / 2;
            if (!std::isfinite (measured) || measured <= 0)
                return fail ("Invalid published triangle area.");
            if (i == 0) {
                const double normalZ = cross.z / (2 * measured);
                if (normalZ >= std::sqrt (.5))
                    facing = Facing::Top;
                else if (normalZ <= -std::sqrt (.5))
                    facing = Facing::Bottom;
            }
            area += measured;
        }
        if (!std::isfinite (fragment.areaM2) || std::abs (area - fragment.areaM2) > 1e-6)
            return fail ("Published fragment and triangle areas differ.");
        auto& row = prepared.rows[position->second];
        row.areaM2 += area;
        overlaylayers::Mesh mesh;
        mesh.points = fragment.points;
        mesh.indices = fragment.triangles;
        mesh.rgba = row.rgba;
        mesh.styled = true;
        mesh.style.opacity = .5f;
        mesh.style.behind = overlaylayers::Behind::Fade;
        mesh.style.occludedOpacity = kOccludedHighlightOpacity;
        mesh.style.cullBack = false;
        mesh.style.edgeRgba = 0;
        mesh.style.edgeWidthPixels = 1.5f;
        mesh.hoverTitle = row.name;
        mesh.hoverRows = { { "Highlighted", Area (area) + " m2" } };
        layer.meshes.push_back (std::move (mesh));
        prepared.meshSurfaces.push_back (fragment.surfaceAttributeIndex);
        prepared.meshAreas.push_back (area);
        prepared.meshFacings.push_back (facing);
        prepared.meshElements.push_back (fragment.guid);
    }
    prepared.triangles =
        std::accumulate (layer.meshes.begin (), layer.meshes.end (), size_t (0),
                         [] (size_t total, const auto& mesh) { return total + mesh.indices.size () / 3; });
    prepared.areaM2 = std::accumulate (prepared.meshAreas.begin (), prepared.meshAreas.end (), 0.0);
    if (prepared.triangles != result.selectedTriangleCount || !std::isfinite (result.selectedAreaM2) ||
        std::abs (prepared.areaM2 - result.selectedAreaM2) > 1e-6)
        return fail ("Published panel and fragment totals differ.");
    for (size_t i = 0; i < prepared.rows.size (); ++i)
        if (std::abs (prepared.rows[i].areaM2 - result.surfaces[i].areaM2) > 1e-6)
            return fail ("Published surface and fragment totals differ.");
    for (const auto& row : result.nativeSurfaces) {
        if (!positions.contains (row.surfaceAttributeIndex) || !std::isfinite (row.areaM2) || row.areaM2 < 0 ||
            !prepared.nativeAreas.emplace (row.surfaceAttributeIndex, row.areaM2).second)
            return fail ("Invalid published native comparison row.");
    }
    if (result.classification == "allElementFaces-contactOnly")
        prepared.scopeLabel = "Criteria candidates: captured walls, slabs, columns and beams. Includes interiors; not "
                              "exterior-only.";
    else if (result.classification == "allWallFaces-contactOnly")
        prepared.scopeLabel = "All captured wall faces: includes interiors, returns and reveals. Not exterior-only.";
    else if (result.classification == "userExteriorSeeds-connectedCoplanar")
        prepared.scopeLabel = "User-confirmed exterior seed: connected facade plane(s).";
    else
        return fail ("Unsupported published region classification.");
    prepared.comparisonNote = result.nativeComparisonNote;
    prepared.contactToleranceM = result.contactToleranceM;
    prepared.coverLabel = "Cover scope: " + result.coverScopeId.substr (0, result.coverScopeId.find (':')) + " (" +
                          std::to_string (result.coverBodyCount) +
                          " bodies). Other walls/frames count only when supplied. Refresh after edits.";
    prepared.layer = UiLayer (prepared, InitialUi (prepared, "0"));
    const auto invalid = overlaylayers::Validate (layer);
    if (!invalid.empty ()) {
        error = invalid;
        return false;
    }
    output = std::move (prepared);
    return true;
}
} // namespace geomsrv::archviz::surfacetakeoff
