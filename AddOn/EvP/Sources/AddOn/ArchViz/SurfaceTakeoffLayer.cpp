// Bound by OVERLAY-INVARIANTS.md: only layer content, camera-independent area.
#include "SurfaceTakeoffLayer.hpp"
#include "Geometry/GeometryEngine.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
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
uint32_t DefaultColor (size_t index)
{
    (void) index;
    return kHighlightRgba;
}
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
    layer.occlusion = overlaylayers::Behind::Show;
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
        prepared.rows.push_back ({ index, found->second.name, kHighlightRgba, 0 });
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
        mesh.style.behind = overlaylayers::Behind::Show;
        mesh.style.cullBack = false; // selected faces stay inspectable through the model from either side
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
} // namespace geomsrv::archviz::surfacetakeoff
