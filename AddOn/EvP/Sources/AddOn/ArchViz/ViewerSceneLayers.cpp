// ArchViz/ViewerSceneLayers -- see the header.
#include "ArchViz/ViewerSceneLayers.hpp"
#include "ArchViz/EditLayers.hpp"
#include "ArchViz/MassingBuildings.hpp"
#include "ArchViz/MassingSlices.hpp"
#include "ArchViz/SurfaceTakeoffLayer.hpp"

namespace geomsrv::archviz::viewerscene {
namespace layers = overlaylayers;

LayerPtr GuestDrawn (const LayerPtr& layer)
{
    if (!layer)
        return layer;
    bool raw = false;
    for (const auto& mesh : layer->meshes)
        raw = raw || !layers::DrawnByGuest (mesh, *layer);
    for (const auto& polyline : layer->polylines)
        raw = raw || !layers::DrawnByGuest (polyline, *layer);
    if (!raw)
        return layer;
    auto copy = std::make_shared<layers::Layer> (*layer);
    // Raw drawing happens only for Hide or Show (RawOcclusion), so naming it per item
    // keeps where it is drawn and only changes who draws it.
    const layers::Behind occlusion = layer->occlusion;
    for (auto& mesh : copy->meshes)
        if (!layers::DrawnByGuest (mesh, *layer)) {
            mesh.styled = true;
            mesh.style = layers::MeshStyle {};
            mesh.style.shading = layers::Shading::Flat;
            mesh.style.behind = occlusion;
        }
    for (auto& polyline : copy->polylines)
        if (!layers::DrawnByGuest (polyline, *layer))
            polyline.behind = occlusion;
    return copy;
}

bool DrawsStoreysInPlace (const std::string& layer)
{
    for (const char* name :
         { massingslices::kLayer, massingslices::kHighlightLayer, massingslices::kLargeFloorsLayer,
           massingslices::kLowHeadroomLayer, massingbuildings::kUniqueLayer, massingbuildings::kSelectedLayer,
           massingbuildings::kFloorsLayer, buildingplan::kUnitsLayer, buildingplan::kStairsLayer })
        if (layer == name)
            return true;
    return false;
}

std::vector<LayerPtr> Select (const std::vector<LayerPtr>& all, const Selection& selection)
{
    std::vector<LayerPtr> out;
    for (const auto& layer : all) {
        if (!layer || !layers::DrawnIn (layer->views, layers::Views::ThreeD))
            continue;
        if (layer->name == surfacetakeoff::kLayer)
            continue;
        if (layer->name != editlayers::kLayer && !selection.massing)
            continue;
        if (selection.diagramStandsIn && DrawsStoreysInPlace (layer->name))
            continue;
        if (selection.shown && !selection.shown (layer->name))
            continue;
        out.push_back (layer);
    }
    return out;
}

std::vector<LayerPtr> Converter::Convert (const std::vector<LayerPtr>& layers)
{
    std::vector<std::pair<LayerPtr, LayerPtr>> next;
    std::vector<LayerPtr> out;
    next.reserve (layers.size ());
    out.reserve (layers.size ());
    for (const auto& layer : layers) {
        LayerPtr converted;
        for (const auto& [source, guest] : last_)
            if (source == layer) {
                converted = guest;
                break;
            }
        if (!converted)
            converted = GuestDrawn (layer);
        next.emplace_back (layer, converted);
        out.push_back (converted);
    }
    last_ = std::move (next);
    return out;
}
} // namespace geomsrv::archviz::viewerscene
