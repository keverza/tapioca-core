#include "ArchViz/MassingSlices.hpp"
#include "ArchViz/FloorProgramme.hpp"
#include <iterator>

namespace geomsrv::archviz::massingslices {
namespace {
void Append (overlaylayers::Layer& into, overlaylayers::Layer from)
{
    into.texts.insert (into.texts.end (), std::make_move_iterator (from.texts.begin ()),
                       std::make_move_iterator (from.texts.end ()));
    into.polylines.insert (into.polylines.end (), std::make_move_iterator (from.polylines.begin ()),
                           std::make_move_iterator (from.polylines.end ()));
    into.meshes.insert (into.meshes.end (), std::make_move_iterator (from.meshes.begin ()),
                        std::make_move_iterator (from.meshes.end ()));
}
} // namespace
overlaylayers::Layer RowDisplay (const Row& row, const storysliceoverlay::Slice& outside, const std::string& name,
                                 const storysliceoverlay::Controls& display)
{
    overlaylayers::Layer out;
    storysliceoverlay::Slice slice;
    // The floor's slab (user, 2026-10-10): inside the 0.5 m facade wall, 0.3 m thick below the
    // floor's level. The caption keeps the counted area of the whole contour.
    slice.chains = Inset (row.chains, floorprogramme::kFacade);
    slice.z = row.z;
    slice.storey = row.story;
    slice.areaM2 = row.clipped ? row.allowedArea : row.rawArea;
    slice.name = name;
    auto controls = display;
    controls.views = overlaylayers::Views::Both;
    controls.fillColors.clear ();
    controls.fillColormap.stops.clear ();
    controls.fillRgba = row.fillRgba;
    controls.outlineRgba = row.rgba;
    controls.labelMinProjectedPixels = 9;
    controls.liftMetres = 0.015;
    if (!slice.chains.empty ()) {
        auto coloured = storysliceoverlay::BuildLayer ({ slice }, controls).layer;
        // The slab's sides and underside join the slice's fill: one mesh, one hover, one colour.
        overlaylayers::Mesh slab;
        std::string error;
        if (!coloured.meshes.empty () &&
            ExtrudeChains (slice.chains, row.z - floorprogramme::kSlab, row.z, slab, error, false)) {
            auto& fill = coloured.meshes.front ();
            const uint32_t base = static_cast<uint32_t> (fill.points.size () / 3);
            fill.points.insert (fill.points.end (), slab.points.begin (), slab.points.end ());
            for (uint32_t index : slab.indices)
                fill.indices.push_back (base + index);
            fill.style.cullBack = false;
        }
        for (auto& mesh : coloured.meshes)
            mesh.graphicsFunction = row.function;
        for (auto& line : coloured.polylines)
            line.graphicsFunction = row.function;
        Append (out, std::move (coloured));
    }
    controls.label = false; // Excluded regions never contribute on-slice area captions.
    if (!outside.chains.empty ()) {
        auto excess = outside;
        excess.name = name + " (outside envelope)";
        controls.fillRgba = 0xAA446500 | ((display.fillRgba & 0xFF) ? 0xFF : 0);
        controls.fillOpacity = 0.5f * display.fillOpacity;
        controls.outlineRgba = 0xAA4465FF;
        Append (out, storysliceoverlay::BuildLayer ({ excess }, controls).layer);
    }
    if (!row.lowChains.empty ()) {
        slice.chains = row.lowChains;
        slice.areaM2 = row.excludedArea;
        slice.name = name + " (headroom < 1.6 m, excluded)";
        controls.fillRgba = 0xD9DDE200 | ((display.fillRgba & 0xFF) ? 0x80 : 0);
        controls.fillOpacity = display.fillOpacity;
        controls.outlineRgba = 0xD9DDE2FF;
        Append (out, storysliceoverlay::BuildLayer ({ slice }, controls).layer);
    }
    return out;
}
bool LowHeadroomHighlight (const Result& result, overlaylayers::Layer& layer, std::string& error)
{
    error.clear ();
    std::vector<storysliceoverlay::Slice> slices;
    size_t points = 0;
    for (const auto& row : result.rows) {
        for (const auto& chain : row.lowChains)
            points += chain.Count ();
        if (points > 200000)
            return error = "Low-headroom highlight exceeds its contour budget.", false;
        if (!row.lowChains.empty ())
            slices.push_back ({ row.lowChains, row.z, row.excludedArea,
                                row.guid + " floor " + std::to_string (row.story) + " (headroom < 1.6 m, excluded)",
                                row.story });
    }
    storysliceoverlay::Controls controls;
    controls.views = overlaylayers::Views::Both;
    controls.label = false;
    controls.fillRgba = 0xEDF0F4D9;
    controls.fillBehind = overlaylayers::Behind::Fade;
    controls.outlineRgba = 0x707780FF;
    controls.outlineWidthPixels = 3;
    controls.outlineBehind = overlaylayers::Behind::Fade;
    controls.liftMetres = 0.02;
    auto out = storysliceoverlay::BuildLayer (slices, controls).layer;
    out.name = kLowHeadroomLayer;
    out.occlusion = overlaylayers::Behind::Fade;
    size_t vertices = 0;
    for (auto& mesh : out.meshes) {
        vertices += mesh.points.size () / 3;
        mesh.style.hatched = true;
        mesh.style.hatchDirection = 45;
        mesh.style.hatchDensity = 2;
    }
    if (vertices > 600000)
        return error = "Low-headroom highlight exceeds its fill budget.", false;
    error = overlaylayers::Validate (out);
    if (!error.empty ())
        return false;
    layer = std::move (out);
    return true;
}
} // namespace geomsrv::archviz::massingslices
