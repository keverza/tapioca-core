#ifndef TAPIOCA_ARCHVIZ_SURFACETAKEOFFPAGE_HPP
#define TAPIOCA_ARCHVIZ_SURFACETAKEOFFPAGE_HPP

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>

namespace geomsrv::surfacetakeoff {
struct ControllerView;
}
namespace geomsrv::archviz::hudshell {
struct HostTab;
}

namespace geomsrv::archviz::surfacetakeoff {

enum class TargetScope { Construction, Walls };
struct PageAction {
    enum class Kind { Calculate, Cancel, Isolate };
    Kind kind;
    TargetScope scope;
    uint64_t generation = 0;
    std::optional<int32_t> material = {};
};
struct HighlightSummary {
    uint64_t generation = 0;
    std::optional<int32_t> material;
    size_t triangles = 0;
    double areaM2 = 0;
};
struct PageState {
    TargetScope scope = TargetScope::Construction;
    uint64_t shownGeneration = 0;
    size_t page = 0;
};

hudshell::HostTab WorkflowTab ();
const char* RegionMode (TargetScope scope);
const char* LauncherScope (TargetScope scope);

// Only transient page inputs are mutable here. Hosts execute the returned action after layout.
std::optional<PageAction> DrawPage (PageState& page, const geomsrv::surfacetakeoff::ControllerView* view,
                                    const std::string& actionError, HighlightSummary highlight = {});

} // namespace geomsrv::archviz::surfacetakeoff

#endif
