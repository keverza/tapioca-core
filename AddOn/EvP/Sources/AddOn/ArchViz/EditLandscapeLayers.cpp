#include "ArchViz/EditLayers.hpp"
#include "Edit/EditShape.hpp"

#include <algorithm>
#include <set>

// OVERLAY-INVARIANTS.md: command-scoped model geometry only. No camera, GPU state,
// native reads or cursor ownership changes. The two renderers keep their own transforms.
namespace geomsrv::archviz::editlayers {
namespace lg = tapioca::geometry;
namespace layers = overlaylayers;

void AddLandscape (const edit::Document& doc, const Feedback& f, const Style& style, layers::Layer& layer)
{
    if (!doc.landscape || !f.landscape || !f.landscape->error.empty ())
        return;
    const auto line = [&] (const std::vector<edit::Vec3>& points, bool closed, uint32_t rgba, float width, bool flat) {
        layers::Polyline out;
        for (const auto& p : points)
            out.points.insert (out.points.end (), { p.x, p.y, flat ? f.landscapeZ : p.z });
        out.closed = closed;
        out.rgba = rgba;
        out.widthPixels = width;
        return out;
    };
    // #160/#189: what a Boolean consumed, and what it built. An input's source line is drawn as
    // construction (grey, fine dashes); a result's outline is drawn heavier than a plain region's.
    std::set<lg::Id> constructionSources, results;
    for (const auto& input : lg::LandscapeConstructionInputs (*doc.landscape)) {
        results.insert (input.consumer);
        for (const auto& object : doc.landscape->objects)
            if (object.id == input.object && object.boundarySource)
                constructionSources.insert (object.boundarySource);
    }
    for (const auto& source : doc.landscape->sources)
        if (!source.deleted && source.role != lg::SourceRole::ReferenceLine &&
            std::none_of (doc.landscape->objects.begin (), doc.landscape->objects.end (),
                          [&] (const auto& object) { return object.boundarySource == source.id; }))
            if (const auto result = lg::LandscapeSourceResult (*doc.landscape, source.id); result != edit::kNoId) {
                constructionSources.insert (source.id); // #210: a line a Boolean took whole
                results.insert (result);
            }
    const auto region = [&] (const lg::DerivedRegion& r, uint32_t edge, bool fill, float width) {
        const lg::RegionSurface* surface = nullptr;
        const lg::HardscapeObject* owner = nullptr;
        for (const auto& object : doc.landscape->objects)
            if (object.id == r.objectId && object.surface) {
                surface = &*object.surface;
                owner = &object;
            }
        lg::LandscapePreviewMesh preview;
        std::string error;
        lg::PreparedRegionSurface prepared;
        if (surface && surface->elevation == lg::RegionElevationMode::Constant)
            if (!lg::PrepareRegionSurface (*surface, prepared, error))
                surface = nullptr;
        const bool boundary = surface && surface->elevation == lg::RegionElevationMode::Boundary;
        if (boundary && !lg::BuildRegionSurfaceMesh (doc, *owner, r, preview, error))
            return;
        size_t boundaryVertex = 0;
        const auto at = [&] (edit::Vec3 p) {
            if (boundary)
                return preview.vertices.at (boundaryVertex++);
            p.z = prepared.Elevation (p).value_or (f.landscapeZ);
            return p;
        };
        for (auto ring : r.rings) {
            for (auto& p : ring)
                p = at (p);
            layer.polylines.push_back (line (ring, true, edge, width, false));
        }
        if (!fill)
            return;
        if (!boundary && !lg::BuildLandscapePreviewMesh (r, preview, error)) {
            layers::Text warning;
            warning.text = error;
            warning.at[0] = r.seed.x;
            warning.at[1] = r.seed.y;
            warning.at[2] = f.landscapeZ;
            warning.rgba = style.propertyLine;
            layer.texts.push_back (std::move (warning));
            return;
        }
        layers::Mesh mesh;
        mesh.indices = std::move (preview.triangles);
        mesh.points.reserve (preview.vertices.size () * 3);
        boundaryVertex = 0;
        for (const auto& p : preview.vertices) {
            const auto q = at (p);
            mesh.points.insert (mesh.points.end (), { q.x, q.y, q.z });
        }
        mesh.rgba = edge == style.landscapeEdge ? style.landscapeFill : (edge & 0xFFFFFF00u) | 0x28u;
        mesh.styled = true;
        mesh.style.shading = layers::Shading::Flat;
        if (!mesh.indices.empty ())
            layer.meshes.push_back (std::move (mesh));
    };
    // #189: a construction input is not drawn as a region: its own dashed source line stays, and
    // only the region built from it is solid and filled.
    for (const auto& r : f.landscape->regions)
        if (!r.constructionOf)
            region (r, style.landscapeEdge, true,
                    results.count (r.objectId) ? style.lineWidth + 1.0f : style.lineWidth);
    for (const auto& face : f.landscape->faces) {
        const edit::Ref ref { edit::kNoId, edit::Part::LandscapeFace, face.faceId, f.landscape->revision };
        const bool selected = f.selection && f.selection->Contains (ref);
        const bool hover = f.selection && f.selection->hover == ref;
        if (!selected && !hover && !f.landscapeFaces)
            continue; // #160: an idle face outline would draw solid over every source's dashes
        region (face,
                selected ? style.landscapeSelected
                : hover  ? style.landscapeHover
                         : style.landscapeEdge,
                selected || hover, style.lineWidth);
    }
    for (const auto& source : doc.landscape->sources) {
        if (source.deleted || (source.role == lg::SourceRole::ReferenceLine && !source.lineStyle.visible))
            continue;
        // A compound reference line (a boolean left it a hole, #158) is drawn contour by contour;
        // every other source is the one ring the landscape reads.
        std::vector<std::vector<edit::Vec3>> rings;
        bool closed = false;
        const edit::Entity* entity = source.entityId ? edit::Find (doc, source.entityId) : nullptr;
        if (source.role == lg::SourceRole::ReferenceLine && entity != nullptr && edit::IsCompound (*entity)) {
            rings = edit::ShapeRings (*entity);
            closed = true;
        }
        else {
            std::vector<edit::Vec3> points;
            std::string error;
            if (!lg::LandscapeSourcePreview (doc, source, points, closed, error))
                continue;
            rings.push_back (std::move (points));
        }
        const bool property = source.id == doc.landscape->propertyLineSourceId;
        const bool construction = !property && constructionSources.count (source.id) > 0;
        for (const auto& points : rings) {
            auto stroke = line (points, closed, property ? style.propertyLine : style.landscapeSource,
                                property ? style.selectedWidth : style.lineWidth, false);
            if (source.role == lg::SourceRole::ReferenceLine) {
                stroke.rgba = source.lineStyle.rgba;
                stroke.widthPixels = static_cast<float> (source.lineStyle.widthPixels);
            }
            stroke.dashMetres = property       ? std::vector<float> { 0.8f, 0.15f, 0.15f, 0.15f }
                                : construction ? std::vector<float> { 0.15f, 0.15f }
                                               : std::vector<float> { 0.4f, 0.25f };
            if (construction) {
                stroke.rgba = style.landscapeConstruction;
                stroke.widthPixels = style.lineWidth - 0.75f;
            }
            layer.polylines.push_back (std::move (stroke));
        }
    }
}
} // namespace geomsrv::archviz::editlayers
