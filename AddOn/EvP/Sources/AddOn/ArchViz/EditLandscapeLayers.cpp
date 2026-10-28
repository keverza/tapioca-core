#include "ArchViz/EditLayers.hpp"

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
    const auto region = [&] (const lg::DerivedRegion& r, uint32_t edge, bool fill) {
        for (const auto& ring : r.rings)
            layer.polylines.push_back (line (ring, true, edge, style.lineWidth, true));
        if (!fill)
            return;
        lg::LandscapePreviewMesh preview;
        std::string error;
        if (!lg::BuildLandscapePreviewMesh (r, preview, error)) {
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
        for (const auto& p : preview.vertices) {
            mesh.points.insert (mesh.points.end (), { p.x, p.y, f.landscapeZ });
        }
        mesh.rgba = edge == style.landscapeEdge ? style.landscapeFill : (edge & 0xFFFFFF00u) | 0x28u;
        mesh.styled = true;
        mesh.style.shading = layers::Shading::Flat;
        if (!mesh.indices.empty ())
            layer.meshes.push_back (std::move (mesh));
    };
    for (const auto& r : f.landscape->regions)
        region (r, style.landscapeEdge, true);
    for (const auto& face : f.landscape->faces) {
        const edit::Ref ref { edit::kNoId, edit::Part::LandscapeFace, face.faceId, f.landscape->revision };
        const bool selected = f.selection && f.selection->Contains (ref);
        const bool hover = f.selection && f.selection->hover == ref;
        region (face,
                selected ? style.landscapeSelected
                : hover  ? style.landscapeHover
                         : style.landscapeEdge,
                selected || hover);
    }
    for (const auto& source : doc.landscape->sources) {
        if (source.deleted || (source.role == lg::SourceRole::ReferenceLine && !source.lineStyle.visible))
            continue;
        std::vector<edit::Vec3> points;
        bool closed = false;
        std::string error;
        if (!lg::LandscapeSourcePreview (doc, source, points, closed, error))
            continue;
        const bool property = source.id == doc.landscape->propertyLineSourceId;
        auto stroke = line (points, closed, property ? style.propertyLine : style.landscapeSource,
                            property ? style.selectedWidth : style.lineWidth, false);
        if (source.role == lg::SourceRole::ReferenceLine) {
            stroke.rgba = source.lineStyle.rgba;
            stroke.widthPixels = static_cast<float> (source.lineStyle.widthPixels);
        }
        stroke.dashMetres =
            property ? std::vector<float> { 0.8f, 0.15f, 0.15f, 0.15f } : std::vector<float> { 0.4f, 0.25f };
        layer.polylines.push_back (std::move (stroke));
    }
}
} // namespace geomsrv::archviz::editlayers
