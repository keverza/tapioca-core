// ArchViz/GradientLibrary -- see the header.

#include "ArchViz/GradientLibrary.hpp"

namespace geomsrv {
namespace archviz {
namespace overlaylayers {

namespace {

constexpr GradientPreset kPresets[] = {
#include "ArchViz/GradientLibraryTable.inc"
};

constexpr const char* kCategoryLabels[kGradientCategoryCount] = {
    "General", "Sun", "Daylight", "View", "Terrain", "Comfort", "Wind", "Energy", "Analysis", "Natural", "Chromatic",
};

} // namespace

std::span<const GradientPreset> GradientPresets ()
{
    return kPresets;
}

const GradientPreset* FindGradientPreset (std::string_view id)
{
    for (const GradientPreset& preset : kPresets)
        if (id == preset.id)
            return &preset;
    return nullptr;
}

const char* GradientCategoryLabel (GradientCategory category)
{
    const size_t index = size_t (category);
    return index < kGradientCategoryCount ? kCategoryLabels[index] : "";
}

} // namespace overlaylayers
} // namespace archviz
} // namespace geomsrv
