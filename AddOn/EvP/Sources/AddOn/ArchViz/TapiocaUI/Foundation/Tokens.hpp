#ifndef EVP_ARCHVIZ_TAPIOCAUI_FOUNDATION_TOKENS_HPP
#define EVP_ARCHVIZ_TAPIOCAUI_FOUNDATION_TOKENS_HPP

// TapiocaUI/Foundation/Tokens -- the sizes the library's controls are built from, in logical
// pixels before the view's DPI and the user's text size. One place for them, so a control
// never carries a literal of its own.
//
// ⚠️ P0 SKELETON: only the spacing scale and the gallery's densities live here yet. The colour,
// type, shape and interaction tokens move here from hudshell in P1 -- values first, unchanged,
// then the code that reads them (HANDOFF-TapiocaUI-Library.md section 6).

#include <cstdint>

namespace geomsrv {
namespace archviz {
namespace ui {
namespace tokens {

// The spacing scale every gap and padding is taken from.
enum class Space : uint8_t { XS = 0, S, M, L, XL };
constexpr float kSpacePixels[] = { 4.0f, 8.0f, 12.0f, 16.0f, 24.0f };

constexpr float Spacing (Space space, float scale = 1.0f)
{
    return kSpacePixels[uint8_t (space)] * scale;
}

// The densities a control is checked at in the gallery: 80, 100, 140 and 200 per cent of what
// the host already applies.
constexpr float kDensitySteps[] = { 0.8f, 1.0f, 1.4f, 2.0f };
constexpr uint32_t kDensityStepCount = uint32_t (sizeof (kDensitySteps) / sizeof (kDensitySteps[0]));
constexpr uint32_t kDensityStepDefault = 1;

} // namespace tokens
} // namespace ui
} // namespace archviz
} // namespace geomsrv

#endif
