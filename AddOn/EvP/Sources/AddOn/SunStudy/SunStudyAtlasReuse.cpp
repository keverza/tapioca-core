#include "SunStudy/SunStudyAtlasReuse.hpp"
#include "SunStudy/SunStudyRoles.hpp"

#include <algorithm>
#include <set>

namespace evp::sunstudy {

SunStudyAtlas BuildStableTriangleAtlas (const SampleGrid& grid, const geomsrv::Snapshot& snapshot,
                                        SunStudyPatchAtlas& allocations, const SunStudyPatchAtlas* previous,
                                        const AtlasOptions& options)
{
    PatchSampleGrid rectangles;
    std::vector<size_t> faces;
    std::set<std::string> identities;
    size_t face = 0;
    for (const auto& mesh : snapshot.meshes) {
        const auto guid = CanonicalGuid (mesh.guid);
        if (guid.empty () || !identities.insert (guid).second || face + mesh.TriangleCount () > grid.layouts.size ()) {
            allocations.Clear ();
            return BuildSunStudyAtlas (grid, options); // ambiguous identity cannot reuse
        }
        for (size_t local = 0; local < mesh.TriangleCount (); ++local, ++face) {
            const auto& layout = grid.layouts[face];
            if (!layout.gridded || layout.columns == 0 || layout.rows == 0)
                continue;
            PatchSampleSpan span;
            span.key = { guid, local, 0 };
            span.columns = layout.columns;
            span.rows = layout.rows;
            rectangles.spans.push_back (span);
            faces.push_back (face);
        }
    }
    if (face != grid.layouts.size ()) {
        allocations.Clear ();
        return BuildSunStudyAtlas (grid, options);
    }
    if (previous != nullptr && previous->Width () <= options.maxDimension)
        allocations = *previous;
    allocations.Fit (rectangles, options.gutter, options.maxDimension);
    SunStudyAtlas atlas;
    atlas.width = allocations.Width ();
    atlas.height = allocations.Height ();
    atlas.tiles.resize (grid.layouts.size ());
    if (atlas.width == 0 && !faces.empty ())
        return atlas;
    for (size_t i = 0; i < faces.size (); ++i) {
        const auto* placed = allocations.Find (rectangles.spans[i].key);
        if (placed == nullptr || !placed->Placed ())
            return SunStudyAtlas ();
        atlas.tiles[faces[i]] = { placed->x, placed->y, placed->width, placed->height };
    }
    atlas.placedFaces = faces.size ();
    atlas.texels.assign (grid.Count (), 0u);
    if (grid.faces.size () != grid.Count () || grid.cellColumns.size () != grid.Count () ||
        grid.cellRows.size () != grid.Count ())
        return SunStudyAtlas ();
    for (size_t i = 0; i < grid.Count (); ++i) {
        if (grid.faces[i] >= atlas.tiles.size ())
            return SunStudyAtlas ();
        const auto& tile = atlas.tiles[grid.faces[i]];
        if (tile.Placed ())
            atlas.texels[i] = (tile.y + std::min (grid.cellRows[i], tile.height - 1)) * atlas.width + tile.x +
                              std::min (grid.cellColumns[i], tile.width - 1);
    }
    atlas.valid = true;
    return atlas;
}

} // namespace evp::sunstudy
