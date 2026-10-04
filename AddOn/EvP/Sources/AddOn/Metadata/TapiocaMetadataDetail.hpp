#ifndef EVP_METADATA_TAPIOCAMETADATADETAIL_HPP
#define EVP_METADATA_TAPIOCAMETADATADETAIL_HPP

// Metadata/TapiocaMetadataDetail -- what the metadata's translation units share and nothing else
// includes: which value types hold text and which a measured number, a colour as #RRGGBBAA, and a
// number with its decimals. Not an interface.

#include "Metadata/TapiocaMetadata.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace geomsrv {
namespace metadata {
namespace detail {

inline bool Textual (ValueType type)
{
    return type == ValueType::String || type == ValueType::Enum || type == ValueType::DateTime ||
           type == ValueType::Reference;
}

inline bool Measured (ValueType type)
{
    return type == ValueType::Double || type == ValueType::Length || type == ValueType::Area ||
           type == ValueType::Volume || type == ValueType::Angle || type == ValueType::Percentage;
}

inline std::string Hex (uint32_t rgba)
{
    char text[16] = {};
    std::snprintf (text, sizeof (text), "#%08X", rgba);
    return text;
}

inline bool FromHex (const std::string& text, uint32_t& rgba)
{
    if (text.size () != 9 || text[0] != '#')
        return false;
    uint32_t value = 0;
    for (size_t i = 1; i < text.size (); ++i) {
        const char c = text[i];
        uint32_t digit = 0;
        if (c >= '0' && c <= '9')
            digit = uint32_t (c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = uint32_t (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            digit = uint32_t (c - 'A' + 10);
        else
            return false;
        value = (value << 4) | digit;
    }
    rgba = value;
    return true;
}

inline std::string Number (double value, int decimals)
{
    char text[64] = {};
    std::snprintf (text, sizeof (text), "%.*f", (std::max) (0, (std::min) (decimals, 9)), value);
    return text;
}

} // namespace detail
} // namespace metadata
} // namespace geomsrv

#endif
