#ifndef TAPIOCA_ARCHVIZ_SURFACETAKEOFFPAGE_HPP
#define TAPIOCA_ARCHVIZ_SURFACETAKEOFFPAGE_HPP

#include <array>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <map>
#include <string>
#include <set>
#include <vector>

namespace geomsrv::surfacetakeoff {
struct ControllerView;
}
namespace geomsrv::archviz::hudshell {
struct HostTab;
}

namespace geomsrv::archviz::surfacetakeoff {

enum class TargetScope { Construction, Walls };
constexpr float kDefaultContextOpacity = 0.38f;
// How the rest of the model reads while one material is focused. Display only:
// neither style changes the selected fragments, their areas or the result.
//   Desaturated: the model keeps its shading but loses its colour.
//   WhiteModel:  the model is a white architectural render; the selected surface
//                keeps its captured colour, shows through the context at
//                kWhiteModelHiddenOpacity and its hidden contours are dashed.
enum class ContextStyle : uint8_t { Desaturated = 0, WhiteModel = 1 };
constexpr float kWhiteModelHiddenOpacity = 0.3f;
struct PageAction {
    enum class Kind { Calculate, Cancel, Isolate, FilterMaterials, ContextOpacity, ContextStyle, Screenshot };
    Kind kind;
    TargetScope scope;
    uint64_t generation = 0;
    std::optional<int32_t> material = {};
    std::set<int32_t> materials = {};
    double contextOpacity = kDefaultContextOpacity;
    ContextStyle contextStyle = ContextStyle::Desaturated;
};
struct HighlightSummary {
    uint64_t generation = 0;
    std::optional<int32_t> material;
    float contextOpacity = kDefaultContextOpacity;
    ContextStyle contextStyle = ContextStyle::Desaturated;
    size_t triangles = 0;
    double areaM2 = 0;
    std::set<int32_t> materials;
    std::map<int32_t, double> rowAreas;
    // Distinct contributing elements per row under the applied filters, and the
    // distinct union over what is highlighted -- not the sum of the rows.
    std::map<int32_t, size_t> rowElements;
    size_t elements = 0;
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

// The screenshot row, for a host that can take one (the standalone viewer). The
// injected overlay passes none and the page says where to take it instead.
struct ShotControls {
    int preset = 1;                   // index into `presets`; the page may change it
    std::vector<std::string> presets; // "2x  3840 x 2160"
    bool busy = false;
    bool failed = false;
    std::string result; // the saved path, or why it was not saved
};

hudshell::HostTab WorkflowTab ();
const char* RegionMode (TargetScope scope);
const char* LauncherScope (TargetScope scope);

// Only transient page inputs are mutable here. Hosts execute the returned action after layout.
std::optional<PageAction> DrawPage (PageState& page, const geomsrv::surfacetakeoff::ControllerView* view,
                                    const std::string& actionError, const HighlightSummary& highlight = {},
                                    ShotControls* shot = nullptr);

} // namespace geomsrv::archviz::surfacetakeoff

#endif
