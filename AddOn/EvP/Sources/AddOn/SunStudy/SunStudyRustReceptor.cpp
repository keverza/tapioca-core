#include "SunStudy/SunStudyReceptor.hpp"
#include "SunStudy/SunStudyRustDetail.hpp"

#include <algorithm>

namespace evp::sunstudy {

using namespace rust::detail;

namespace {

// One pass through the Rust kernel; `ceilings` empty for none.
bool Run (const SampleGrid& grid, double normalOffset, const ReceptorOptions& options,
          const std::vector<double>& ceilings, ReceptorLayer& layer, std::string& error)
{
    TapiocaReceptorLayerRequest request {};
    request.positions = { grid.positions.data (), grid.positions.size () };
    request.normals = { grid.normals.data (), grid.normals.size () };
    request.areas = { grid.areas.data (), grid.areas.size () };
    request.ceilings = { ceilings.data (), ceilings.size () };
    request.normalOffset = normalOffset;
    request.height = options.height;
    request.minHostUp = options.minHostUp;
    request.normal = options.faceUp ? 1 : 0;
    request.area = options.planArea ? 1 : 0;

    const size_t count = grid.areas.size ();
    layer = ReceptorLayer ();
    layer.options = options;
    layer.hosts.resize (count * 3);
    layer.positions.resize (count * 3);
    layer.normals.resize (count * 3);
    layer.areas.resize (count);
    std::vector<uint8_t> status (count);
    TapiocaReceptorLayerSummary summary {};
    const TapiocaReceptorLayerOutput output { layer.hosts.data (),
                                              layer.positions.data (),
                                              layer.normals.data (),
                                              layer.areas.data (),
                                              status.data (),
                                              count,
                                              &summary };
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_receptor_layer (&request, &output, m, n); },
                 error)) {
        layer = ReceptorLayer ();
        return false;
    }
    layer.status.reserve (count);
    for (const uint8_t s : status)
        layer.status.push_back (static_cast<ReceptorStatus> (s));
    layer.available = summary.available;
    layer.steepHost = summary.steepHost;
    layer.blocked = summary.blocked;
    layer.invalidHost = summary.invalidHost;
    layer.availableArea = summary.availableArea;
    layer.unavailableArea = summary.unavailableArea;
    return true;
}

} // namespace

bool BuildReceptorLayer (const SampleGrid& grid, double normalOffset, const ReceptorOptions& options,
                         const OverheadDistance* overhead, ReceptorLayer& out, std::string& error)
{
    out = ReceptorLayer ();
    ReceptorLayer layer;
    if (!Run (grid, normalOffset, options, {}, layer, error))
        return false;
    if (overhead == nullptr || !*overhead) {
        out = std::move (layer);
        return true;
    }
    // Query only receptors that still have an answer; the rest keep their reason.
    std::vector<double> ceilings (layer.Count (), -1.0);
    for (size_t i = 0; i < layer.Count (); ++i)
        if (layer.status[i] == ReceptorStatus::Available)
            ceilings[i] = (*overhead) (&layer.hosts[i * 3]);
    if (!Run (grid, normalOffset, options, ceilings, layer, error))
        return false;
    out = std::move (layer);
    return true;
}

std::vector<ReceptorCell> UnavailableReceptorCells (const SampleGrid& grid, const ReceptorLayer& layer)
{
    std::vector<ReceptorCell> cells;
    const size_t count = grid.Count ();
    if (layer.Count () != count || grid.cellColumns.size () != count || grid.cellRows.size () != count)
        return cells;
    for (size_t i = 0; i < count; ++i)
        if (layer.status[i] != ReceptorStatus::Available)
            cells.push_back ({ grid.faces[i], grid.cellColumns[i], grid.cellRows[i] });
    return cells;
}

std::vector<uint32_t> ReceptorCellTexels (const SunStudyAtlas& atlas, const std::vector<ReceptorCell>& cells)
{
    std::vector<uint32_t> texels;
    if (!atlas.valid || atlas.width == 0)
        return texels;
    texels.reserve (cells.size ());
    for (const ReceptorCell& cell : cells) {
        if (cell.face >= atlas.tiles.size () || !atlas.tiles[cell.face].Placed ())
            continue;
        const AtlasTile& tile = atlas.tiles[cell.face];
        const uint32_t column = std::min (cell.column, tile.width - 1);
        const uint32_t row = std::min (cell.row, tile.height - 1);
        texels.push_back ((tile.y + row) * atlas.width + tile.x + column);
    }
    return texels;
}

size_t StampUnavailableTexels (std::vector<float>& image, const std::vector<uint32_t>& texels)
{
    size_t written = 0;
    for (const uint32_t texel : texels)
        if (texel < image.size () && image[texel] < 0.0f) {
            image[texel] = kUnavailableTexel;
            ++written;
        }
    return written;
}

std::vector<ReceptorHostLoss> UnavailableByGroup (const SampleGrid& grid, const ReceptorLayer& layer)
{
    std::vector<ReceptorHostLoss> losses;
    const size_t count = grid.Count ();
    if (layer.Count () != count || grid.groups.size () != count || layer.areas.size () != count)
        return losses;
    for (size_t i = 0; i < count; ++i) {
        const ReceptorStatus status = layer.status[i];
        if (status == ReceptorStatus::Available)
            continue;
        const uint32_t group = grid.groups[i];
        auto at = std::lower_bound (losses.begin (), losses.end (), group,
                                    [] (const ReceptorHostLoss& loss, uint32_t g) { return loss.group < g; });
        if (at == losses.end () || at->group != group)
            at = losses.insert (at, ReceptorHostLoss { group });
        const double area = layer.areas[i];
        (status == ReceptorStatus::SteepHost ? at->steepArea
         : status == ReceptorStatus::Blocked ? at->blockedArea
                                             : at->invalidArea) += area;
    }
    return losses;
}

bool KeepAvailableReceptors (SampleGrid& grid, const ReceptorLayer& layer)
{
    const size_t count = grid.Count ();
    const bool cells = !grid.cellColumns.empty ();
    if (layer.Count () != count || grid.groups.size () != count ||
        (cells && (grid.cellColumns.size () != count || grid.cellRows.size () != count)))
        return false;
    SampleGrid kept;
    kept.layouts = std::move (grid.layouts);
    kept.degenerateFaces = grid.degenerateFaces;
    kept.excludedFaces = grid.excludedFaces;
    kept.undersizedFaces = grid.undersizedFaces;
    kept.valid = grid.valid;
    for (size_t i = 0; i < count; ++i) {
        if (layer.status[i] != ReceptorStatus::Available)
            continue;
        kept.positions.insert (kept.positions.end (), &layer.positions[i * 3], &layer.positions[i * 3] + 3);
        kept.normals.insert (kept.normals.end (), &layer.normals[i * 3], &layer.normals[i * 3] + 3);
        kept.areas.push_back (layer.areas[i]);
        kept.faces.push_back (grid.faces[i]);
        kept.groups.push_back (grid.groups[i]);
        if (cells) {
            kept.cellColumns.push_back (grid.cellColumns[i]);
            kept.cellRows.push_back (grid.cellRows[i]);
        }
    }
    grid = std::move (kept);
    return true;
}

} // namespace evp::sunstudy
