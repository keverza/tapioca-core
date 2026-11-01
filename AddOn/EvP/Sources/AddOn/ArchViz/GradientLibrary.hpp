#ifndef EVP_ARCHVIZ_GRADIENTLIBRARY_HPP
#define EVP_ARCHVIZ_GRADIENTLIBRARY_HPP

// ArchViz/GradientLibrary -- the overlay's named colour ramps (#102). One list,
// GradientPresets.json beside this file; the table it compiles into, the Python
// list and every schema `preset` enum are generated from it by
// tools/generate_gradient_presets.py, and nothing repeats it by hand.
//
// A preset is display data only: choosing one recolours a ramp and never re-runs
// the analysis it shows. Its stops are exact -- positions along [min, max] and
// sRGB bytes -- and are interpolated linearly, as every ramp is.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace geomsrv {
namespace archviz {
namespace overlaylayers {

// A value -> colour ramp: sun hours, shadow, clearance, slope, visibility. Stops are
// positions 0..1 along [min, max], ascending, at most `kMaxStops`.
struct ColourStop {
    float at = 0.0f;
    uint32_t rgba = 0xFFFFFFFFu;
};
constexpr size_t kMaxStops = 16;

// The picker's groups, in the order it shows them.
enum class GradientCategory : uint8_t {
    General,
    Sun,
    Daylight,
    View,
    Terrain,
    Comfort,
    Wind,
    Energy,
    Analysis,
    Natural,
    Chromatic,
};
constexpr size_t kGradientCategoryCount = 11;

struct GradientPreset {
    const char* id;   // stable, public: what `colormap.preset` names
    const char* name; // the picker's label
    GradientCategory category;
    bool stepped;      // reads best as flat bands
    double defaultMin; // the range the preset is drawn for; an explicit range wins
    double defaultMax;
    uint8_t count;
    ColourStop stops[kMaxStops];

    std::span<const ColourStop> Stops () const
    {
        return { stops, count };
    }
};

std::span<const GradientPreset> GradientPresets ();
// Nullptr for an id the library does not know.
const GradientPreset* FindGradientPreset (std::string_view id);
const char* GradientCategoryLabel (GradientCategory category);

} // namespace overlaylayers
} // namespace archviz
} // namespace geomsrv

#endif
