#include "ArchViz/PlantHardiness.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace geomsrv::archviz::planting {

std::string UsdaName (int halfZone)
{
    if (halfZone < 0 || halfZone >= kUsdaHalfZones)
        return "";
    return std::to_string (halfZone / 2 + 1) + (halfZone % 2 == 0 ? "a" : "b");
}

int UsdaHalfZoneOf (double minimumC)
{
    const double f = minimumC * 9.0 / 5.0 + 32.0;
    // 1e-9 keeps an exact band edge (-10 °F) in the warmer band after the
    // Celsius round trip.
    const int h = int (std::floor ((f + 60.0) / 5.0 + 1e-9));
    return std::clamp (h, 0, kUsdaHalfZones - 1);
}

namespace {

// One zone token at `i`: digits and an optional a/b. Whole zones give their
// a (cold end) or b (warm end) as `warmEnd` asks.
bool Zone (const std::string& s, size_t& i, bool warmEnd, int& out)
{
    size_t j = i;
    int n = 0;
    while (j < s.size () && std::isdigit ((unsigned char) s[j]))
        n = n * 10 + (s[j++] - '0');
    if (j == i || n < 1 || n > 13)
        return false;
    int half = warmEnd ? 1 : 0;
    if (j < s.size () && (s[j] == 'a' || s[j] == 'b'))
        half = s[j++] == 'a' ? 0 : 1;
    out = (n - 1) * 2 + half;
    i = j;
    return true;
}

} // namespace

bool ParseUsdaRange (const std::string& text, int& lo, int& hi)
{
    lo = hi = -1;
    std::string s;
    for (unsigned char c : text)
        s += char (std::tolower (c));
    // Dashes of any kind (UTF-8 en and em dash) read as '-'.
    for (const char* dash : { "\xE2\x80\x93", "\xE2\x80\x94" })
        for (size_t at = s.find (dash); at != std::string::npos; at = s.find (dash))
            s.replace (at, 3, "-");
    // RHS ratings by the cold end of their published band (H6: -20 to -15 °C);
    // H7 is "below -20" and gets -20, the conservative reading.
    if (s.rfind ("rhs", 0) == 0) {
        static const struct {
            const char* key;
            double coldEndC;
        } kRhs[] = { { "h1a", 15 }, { "h1b", 10 }, { "h1c", 5 },  { "h2", 1 },  { "h3", -5 },
                     { "h4", -10 }, { "h5", -15 }, { "h6", -20 }, { "h7", -20 } };
        std::string key = s.substr (3);
        key.erase (std::remove_if (key.begin (), key.end (), [] (unsigned char c) { return std::isspace (c); }),
                   key.end ());
        for (const auto& r : kRhs)
            if (key == r.key) {
                lo = UsdaHalfZoneOf (r.coldEndC);
                hi = kUsdaHalfZones - 1;
                return true;
            }
        return false;
    }
    for (const char* word : { "usda", "zone", "zones", "z" })
        if (s.rfind (word, 0) == 0) {
            s.erase (0, std::string (word).size ());
            break;
        }
    s.erase (std::remove_if (s.begin (), s.end (), [] (unsigned char c) { return std::isspace (c); }), s.end ());
    size_t i = 0;
    int a = -1, b = -1;
    if (!Zone (s, i, false, a))
        return false;
    if (i == s.size ())
        b = kUsdaHalfZones - 1; // "USDA 4": hardy down to zone 4, no warm end stated
    else {
        if (s[i] != '-')
            return false;
        ++i;
        if (!Zone (s, i, true, b) || i != s.size () || b < a)
            return false;
    }
    lo = a;
    hi = b;
    return true;
}

bool ColdestHourC (const evp::sunstudy::WeatherData& weather, double& coldestC)
{
    bool any = false;
    for (const auto& m : weather.months)
        if (m.records > 0 && std::isfinite (m.minDryBulbC) && (!any || m.minDryBulbC < coldestC)) {
            coldestC = m.minDryBulbC;
            any = true;
        }
    return any;
}

Hardiness HardinessAt (const std::string& range, int designHalfZone)
{
    int lo = -1, hi = -1;
    if (designHalfZone < 0 || !ParseUsdaRange (range, lo, hi))
        return Hardiness::Unknown;
    if (designHalfZone < lo)
        return Hardiness::TooCold;
    return designHalfZone > hi ? Hardiness::Warmer : Hardiness::Hardy;
}

} // namespace geomsrv::archviz::planting
