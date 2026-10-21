#include "ArchViz/TapiocaUI/Foundation/Units.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <system_error>

namespace geomsrv::archviz::ui {

namespace {

bool Blank (char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

} // namespace

bool ParseNumber (std::string_view text, double& value)
{
    while (!text.empty () && Blank (text.front ()))
        text.remove_prefix (1);
    while (!text.empty () && Blank (text.back ()))
        text.remove_suffix (1);
    // from_chars takes a '-' but not a '+'. One sign only: "+-5" is a typo, not -5.
    if (!text.empty () && text.front () == '+') {
        text.remove_prefix (1);
        if (!text.empty () && (text.front () == '-' || text.front () == '+'))
            return false;
    }
    if (text.empty ())
        return false;

    std::string number (text);
    const auto commas = std::count (number.begin (), number.end (), ',');
    const auto dots = std::count (number.begin (), number.end (), '.');
    // "1,234.5" could be a thousands separator or a typo; neither is guessed at.
    if (commas > 0 && dots > 0)
        return false;
    std::replace (number.begin (), number.end (), ',', '.');

    double parsed = 0.0;
    const char* const end = number.data () + number.size ();
    const auto result = std::from_chars (number.data (), end, parsed);
    if (result.ec != std::errc () || result.ptr != end || !std::isfinite (parsed))
        return false;
    value = parsed;
    return true;
}

std::string FormatNumber (double value, uint32_t decimals)
{
    if (!std::isfinite (value))
        return "--";
    decimals = (std::min) (decimals, kMaxDecimals);
    // Fixed notation of a finite double is at most 309 integer digits, the sign, the point and
    // kMaxDecimals digits.
    char buffer[330];
    const auto result = std::to_chars (buffer, buffer + sizeof buffer, value, std::chars_format::fixed, int (decimals));
    if (result.ec != std::errc ())
        return "--";
    std::string text (buffer, result.ptr);
    if (!text.empty () && text.front () == '-' &&
        std::all_of (text.begin () + 1, text.end (), [] (char c) { return c == '0' || c == '.'; }))
        text.erase (text.begin ());
    return text;
}

} // namespace geomsrv::archviz::ui
