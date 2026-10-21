#ifndef EVP_ARCHVIZ_TAPIOCAUI_FOUNDATION_UNITS_HPP
#define EVP_ARCHVIZ_TAPIOCAUI_FOUNDATION_UNITS_HPP

// TapiocaUI/Foundation/Units -- numbers as the user types and reads them.
//
// ⚠️ LOCALE-STABLE BOTH WAYS. A user types "1,5" or "1.5" and means the same number; the
// process's C locale -- which a host may have changed -- decides neither what is accepted nor
// what is printed. Parsing is std::from_chars (RoofSlopeHud.cpp's NumberInput rule, made
// shared); printing is std::to_chars.
//
// The view's DPI scale joins this file in P1, once one source for it is chosen
// (HANDOFF-TapiocaUI-Library.md section 13.4).

#include <cstdint>
#include <string>
#include <string_view>

namespace geomsrv {
namespace archviz {
namespace ui {

// The most decimals a number is printed with.
constexpr uint32_t kMaxDecimals = 9;

// `text` as one finite number: blanks round it ignored, a leading '+' or '-', one decimal
// separator that may be ',' or '.', an exponent. Anything else -- empty, a trailing unit, two
// separators, both kinds of separator, "inf", "nan" -- is refused and `value` is left as it was.
bool ParseNumber (std::string_view text, double& value);

// `value` in fixed notation with `decimals` digits after a '.' (at most kMaxDecimals). A value
// that rounds to zero prints without a sign: never "-0.00". A non-finite value prints as "--".
std::string FormatNumber (double value, uint32_t decimals);

} // namespace ui
} // namespace archviz
} // namespace geomsrv

#endif
