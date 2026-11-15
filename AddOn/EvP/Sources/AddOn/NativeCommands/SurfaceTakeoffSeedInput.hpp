#ifndef TAPIOCA_SURFACETAKEOFFSEEDINPUT_HPP
#define TAPIOCA_SURFACETAKEOFFSEEDINPUT_HPP

#include "Geometry/SurfaceTakeoffResult.hpp"
#include "NativeCommands/PolygonPickState.hpp"

#include <functional>

namespace geomsrv::surfacetakeoff {

struct SeedPicker {
    std::function<bool (const std::string&, std::string&, std::string&)> start;
    std::function<polygonpick::Outcome (const std::string&)> poll;
    std::function<void (const std::string&)> cancel;
    std::function<bool ()> interrupted;
    std::function<void ()> wait;
};

struct SeedInputResult {
    std::vector<Seed> seeds;
    std::string error;
    bool cancelled = false;
};

// Collect ordered, callback-matched faces under the request's pinned capture.
// The caller owns the worker ticket and supplies its cancellation predicate.
SeedInputResult CollectExteriorSeeds (const Request& request, size_t count, const SeedPicker& picker);

} // namespace geomsrv::surfacetakeoff

#endif
