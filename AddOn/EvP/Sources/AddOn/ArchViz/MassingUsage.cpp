#include "ArchViz/MassingSlices.hpp"

#include <map>

namespace geomsrv::archviz::massingslices {
std::vector<Usage> UsageMix (const Result& result)
{
    std::map<std::string, Usage> uses;
    double total = 0;
    const auto schema = metadata::DefaultSchema ();
    const auto* palette = schema.FindEnumeration ("building-usage");
    for (const auto& row : result.rows) {
        const double area = result.clipped ? row.allowedArea : row.rawArea;
        const double volume = result.clipped ? row.allowedVolume : row.rawVolume;
        if (area <= 0 && volume <= 0)
            continue;
        auto& use = uses[row.function];
        use.function = use.label = row.function;
        use.rgba = row.rgba;
        if (palette)
            for (const auto& option : palette->options)
                if (option.value == row.function)
                    use.label = option.label;
        use.area += area;
        use.volume += volume > 0 ? volume : area * row.floorHeight;
        total += area;
    }
    std::vector<Usage> out;
    for (auto& item : uses) {
        item.second.percent = total > 0 ? 100 * item.second.area / total : 0;
        out.push_back (std::move (item.second));
    }
    return out;
}
} // namespace geomsrv::archviz::massingslices
