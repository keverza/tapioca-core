#include "SunStudy/SunStudyReceptor.hpp"
#include "SunStudy/SunStudyRustDetail.hpp"

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

} // namespace evp::sunstudy
