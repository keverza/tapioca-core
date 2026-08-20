#ifndef GEOMETRYSERVER_METADATAJSON_HPP
#define GEOMETRYSERVER_METADATAJSON_HPP

#include "ElementMeta.hpp"
#include <string>

// Plain-C++ JSON serialization for element metadata (UTF-8 in, JSON out).
namespace geomsrv {

inline void JsonEscape (const std::string& s, std::string& out)
{
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf (buf, sizeof (buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char> (c);   // UTF-8 bytes pass through
                }
        }
    }
    out += '"';
}

inline void PairsToJson (const std::vector<std::pair<std::string, std::string>>& v, std::string& out)
{
    out += '{';
    for (size_t i = 0; i < v.size (); ++i) {
        if (i) out += ',';
        JsonEscape (v[i].first, out);
        out += ':';
        JsonEscape (v[i].second, out);
    }
    out += '}';
}

inline void ElementMetaToJson (const ElementMeta& m, std::string& out)
{
    out += "{\"guid\":";     JsonEscape (m.guid, out);
    out += ",\"type\":";     JsonEscape (m.typeName, out);
    out += ",\"elemId\":";   JsonEscape (m.elemId, out);
    out += ",\"layer\":";    JsonEscape (m.layer, out);
    out += ",\"story\":";    JsonEscape (m.story, out);
    out += ",\"classifications\":"; PairsToJson (m.classifications, out);
    out += ",\"properties\":";      PairsToJson (m.properties, out);
    out += '}';
}

} // namespace geomsrv

#endif
