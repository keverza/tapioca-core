#ifndef TAPIOCA_ARCHVIZ_SURFACETAKEOFFPAGE_HPP
#define TAPIOCA_ARCHVIZ_SURFACETAKEOFFPAGE_HPP

#include <array>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <map>
#include <string>
#include <set>

namespace geomsrv::surfacetakeoff {
struct ControllerView;
}
namespace geomsrv::archviz::hudshell {
struct HostTab;
}

namespace geomsrv::archviz::surfacetakeoff {

enum class TargetScope { Construction, Walls };
constexpr float kDefaultContextOpacity = 0.38f;
struct PageAction {
    enum class Kind { Calculate, Cancel, Isolate, FilterMaterials, ContextOpacity };
    Kind kind;
    TargetScope scope;
    uint64_t generation = 0;
    std::optional<int32_t> material = {};
    std::set<int32_t> materials = {};
    double contextOpacity = kDefaultContextOpacity;
};
struct HighlightSummary {
    uint64_t generation = 0;
    std::optional<int32_t> material;
    float contextOpacity = kDefaultContextOpacity;
    size_t triangles = 0;
    double areaM2 = 0;
    std::set<int32_t> materials;
    std::map<int32_t, double> rowAreas;
    bool Focused () const
    {
        return material.has_value () || materials.size () == 1;
    }
};
struct PageState {
    TargetScope scope = TargetScope::Construction;
    uint64_t shownGeneration = 0;
    size_t page = 0;
    std::array<char, 128> materialSearch {};
    std::set<int32_t> draftMaterials;
    bool textActive = false;
};

hudshell::HostTab WorkflowTab ();
const char* RegionMode (TargetScope scope);
const char* LauncherScope (TargetScope scope);

// Only transient page inputs are mutable here. Hosts execute the returned action after layout.
std::optional<PageAction> DrawPage (PageState& page, const geomsrv::surfacetakeoff::ControllerView* view,
                                    const std::string& actionError, const HighlightSummary& highlight = {});

} // namespace geomsrv::archviz::surfacetakeoff

#endif
