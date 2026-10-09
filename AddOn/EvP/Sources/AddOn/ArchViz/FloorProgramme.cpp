#include "ArchViz/FloorProgramme.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <regex>

namespace geomsrv::archviz::floorprogramme {
namespace {
constexpr double kTolerance = 0.03; // share deviation per type accepted as is
constexpr double kExcess = 4.0;     // weight per flat beyond the tolerance
bool Finite (double value)
{
    return std::isfinite (value);
}
std::string Text (double value)
{
    char buffer[32];
    const double rounded = std::round (value * 100) / 100;
    if (std::abs (rounded - std::round (rounded)) < 1e-9)
        std::snprintf (buffer, sizeof (buffer), "%.0f", rounded);
    else if (std::abs (rounded * 10 - std::round (rounded * 10)) < 1e-9)
        std::snprintf (buffer, sizeof (buffer), "%.1f", rounded);
    else
        std::snprintf (buffer, sizeof (buffer), "%.2f", rounded);
    return buffer;
}
double Number (std::string text)
{
    std::replace (text.begin (), text.end (), ',', '.');
    return std::strtod (text.c_str (), nullptr);
}
double ShareSum (const Programme& programme)
{
    double sum = 0;
    for (const auto& type : programme.types)
        sum += type.share;
    return sum;
}
} // namespace

Programme Default ()
{
    return { { { 1.5, 30, 36, 0.05 },
               { 2, 40, 45, 0.30 },
               { 3, 55, 65, 0.20 },
               { 3, 65, 75, 0.20 },
               { 4, 75, 80, 0.20 },
               { 5, 80, 90, 0.05 } } };
}

std::string Problem (const Programme& programme)
{
    if (programme.types.empty ())
        return "Add at least one flat type.";
    if (programme.types.size () > kMaxTypes)
        return "At most 12 flat types.";
    for (const auto& type : programme.types) {
        if (!Finite (type.rooms) || type.rooms < kMinRooms || type.rooms > kMaxRooms ||
            std::abs (type.rooms * 2 - std::round (type.rooms * 2)) > 1e-9)
            return "Rooms are 1 to 8 in half steps.";
        if (!Finite (type.minM2) || !Finite (type.maxM2) || type.minM2 < kMinArea || type.maxM2 > kMaxArea ||
            type.minM2 >= type.maxM2)
            return "Each area range rises within 15-300 m2.";
        if (!Finite (type.share) || type.share < 0 || type.share > 1)
            return "Shares are 0-100%.";
    }
    const double sum = ShareSum (programme);
    if (std::abs (sum - 1) > 1e-6)
        return "Shares add to " + Text (sum * 100) + "%, not 100%.";
    return {};
}

bool Valid (const Programme& programme)
{
    return Problem (programme).empty ();
}

double Target (const UnitType& type)
{
    return (type.minM2 + type.maxM2) / 2;
}

double Frontage (const UnitType& type)
{
    return 2.4 + 1.2 * type.rooms;
}

std::string Name (const Programme& programme, size_t type)
{
    if (type >= programme.types.size ())
        return "?";
    const auto& t = programme.types[type];
    const bool repeated = std::count_if (programme.types.begin (), programme.types.end (),
                                         [&] (const UnitType& other) { return other.rooms == t.rooms; }) > 1;
    return Text (t.rooms) + "R" + (repeated ? " " + Text (t.minM2) + "-" + Text (t.maxM2) : "");
}

std::string Key (const Programme& programme)
{
    std::string key = "programme";
    char buffer[96];
    for (const auto& type : programme.types) {
        std::snprintf (buffer, sizeof (buffer), "|%.3f:%.4f-%.4f:%.6f", type.rooms, type.minM2, type.maxM2, type.share);
        key += buffer;
    }
    return key;
}

double MeanArea (const Programme& programme)
{
    double sum = 0, weight = 0;
    for (const auto& type : programme.types) {
        sum += Target (type) * type.share;
        weight += type.share;
    }
    return weight > 1e-12 ? sum / weight : 0;
}

void Normalise (Programme& programme)
{
    const double sum = ShareSum (programme);
    if (std::abs (sum - 1) < 1e-9)
        return; // already whole: keep the written fractions exact
    for (auto& type : programme.types)
        type.share = sum > 1e-12 ? type.share / sum : 1.0 / double (programme.types.size ());
}

bool SetShare (Programme& programme, size_t type, double share)
{
    if (type >= programme.types.size () || !Finite (share))
        return false;
    share = std::clamp (share, 0.0, 1.0);
    if (programme.types.size () == 1)
        share = 1;
    if (std::abs (programme.types[type].share - share) < 1e-12)
        return false;
    double others = 0;
    for (size_t i = 0; i < programme.types.size (); ++i)
        if (i != type)
            others += programme.types[i].share;
    for (size_t i = 0; i < programme.types.size (); ++i)
        if (i != type)
            programme.types[i].share = others > 1e-12 ? programme.types[i].share * (1 - share) / others
                                                      : (1 - share) / double (programme.types.size () - 1);
    programme.types[type].share = share;
    return true;
}

bool SetRooms (Programme& programme, size_t type, double rooms)
{
    if (type >= programme.types.size () || !Finite (rooms))
        return false;
    rooms = std::clamp (std::round (rooms * 2) / 2, kMinRooms, kMaxRooms);
    if (programme.types[type].rooms == rooms)
        return false;
    programme.types[type].rooms = rooms;
    return true;
}

bool SetRange (Programme& programme, size_t type, double minM2, double maxM2)
{
    if (type >= programme.types.size () || !Finite (minM2) || !Finite (maxM2))
        return false;
    minM2 = std::clamp (minM2, kMinArea, kMaxArea);
    maxM2 = std::clamp (maxM2, kMinArea, kMaxArea);
    auto& t = programme.types[type];
    if (minM2 >= maxM2 || (t.minM2 == minM2 && t.maxM2 == maxM2))
        return false;
    t.minM2 = minM2;
    t.maxM2 = maxM2;
    return true;
}

size_t AddType (Programme& programme)
{
    if (programme.types.size () >= kMaxTypes)
        return kMaxTypes;
    if (programme.types.empty ()) {
        programme.types.push_back ({ 2, 40, 45, 1 });
        return 0;
    }
    auto added = programme.types.back ();
    const double span = added.maxM2 - added.minM2;
    added.rooms = (std::min) (kMaxRooms, added.rooms + 1);
    added.minM2 = (std::min) (kMaxArea - span, added.maxM2);
    added.maxM2 = added.minM2 + span;
    added.share = 0;
    programme.types.push_back (added);
    return programme.types.size () - 1;
}

bool RemoveType (Programme& programme, size_t type)
{
    if (type >= programme.types.size () || programme.types.size () <= 1)
        return false;
    programme.types.erase (programme.types.begin () + std::ptrdiff_t (type));
    Normalise (programme);
    return true;
}

size_t Nearest (const Programme& programme, double area)
{
    size_t best = 0;
    for (size_t i = 1; i < programme.types.size (); ++i)
        if (std::abs (Target (programme.types[i]) - area) < std::abs (Target (programme.types[best]) - area))
            best = i;
    return best;
}

size_t Retype (const Programme& programme, size_t current, double area)
{
    if (current < programme.types.size () && programme.types[current].minM2 - 1e-6 <= area &&
        area <= programme.types[current].maxM2 + 1e-6)
        return current;
    size_t best = programme.types.size ();
    for (size_t i = 0; i < programme.types.size (); ++i) {
        const auto& t = programme.types[i];
        if (t.minM2 - 1e-6 <= area && area <= t.maxM2 + 1e-6 &&
            (best == programme.types.size () ||
             std::abs (Target (t) - area) < std::abs (Target (programme.types[best]) - area)))
            best = i;
    }
    return best < programme.types.size () ? best : Nearest (programme, area);
}

std::vector<int> Counts (const Programme& programme, int flats)
{
    std::vector<int> counts (programme.types.size (), 0);
    const double total = ShareSum (programme);
    if (flats <= 0 || total <= 1e-12)
        return counts;
    std::vector<double> exact;
    int assigned = 0;
    for (size_t i = 0; i < counts.size (); ++i) {
        exact.push_back (flats * programme.types[i].share / total);
        counts[i] = int (std::floor (exact[i] + 1e-9));
        assigned += counts[i];
    }
    std::vector<size_t> order (counts.size ());
    for (size_t i = 0; i < order.size (); ++i)
        order[i] = i;
    std::stable_sort (order.begin (), order.end (),
                      [&] (size_t a, size_t b) { return exact[a] - counts[a] > exact[b] - counts[b] + 1e-12; });
    for (size_t i = 0; assigned < flats && i < order.size (); ++i, ++assigned)
        ++counts[order[i]];
    return counts;
}

double MixCost (const Programme& programme, const std::vector<int>& counts)
{
    double n = 0;
    for (int count : counts)
        n += count;
    const double total = ShareSum (programme);
    if (total <= 1e-12)
        return 0;
    double cost = 0;
    for (size_t i = 0; i < programme.types.size (); ++i) {
        const double deviation = std::abs ((i < counts.size () ? counts[i] : 0) - programme.types[i].share / total * n);
        cost += 0.5 * (std::min) (deviation, kTolerance * n) + kExcess * (std::max) (0.0, deviation - kTolerance * n);
    }
    return cost;
}

uint32_t Colour (double rooms)
{
    return rooms < 1.25   ? 0x9AD1E6FFu
           : rooms < 1.75 ? 0xA7DADFFFu
           : rooms < 2.75 ? 0x7FC8A9FFu
           : rooms < 3.75 ? 0xF2D06BFFu
           : rooms < 4.75 ? 0xF0A35EFFu
           : rooms < 5.75 ? 0xE58C9AFFu
                          : 0xB79BD8FFu;
}

namespace {
const std::regex& Line ()
{
    static const std::regex line (
        R"((\d+(?:[.,]\d+)?)\s*%\s*(\d+(?:[.,]\d+)?)\s*-?\s*(?:rooms?|r\b|kamb\w*)[^\d\n]*(\d+(?:[.,]\d+)?)\s*-\s*(\d+(?:[.,]\d+)?))",
        std::regex::icase);
    return line;
}
std::string Dashes (const std::string& text)
{
    std::string normal;
    for (size_t i = 0; i < text.size (); ++i) {
        // En and em dashes read as a range dash.
        if (i + 2 < text.size () && (unsigned char) text[i] == 0xE2 && (unsigned char) text[i + 1] == 0x80 &&
            ((unsigned char) text[i + 2] == 0x93 || (unsigned char) text[i + 2] == 0x94)) {
            normal += '-';
            i += 2;
            continue;
        }
        normal += text[i] == ';' ? '\n' : text[i];
    }
    return normal;
}
bool Row (const std::string& row, UnitType& type)
{
    std::smatch match;
    if (!std::regex_search (row, match, Line ()))
        return false;
    type = { Number (match[2]), Number (match[3]), Number (match[4]), Number (match[1]) / 100.0 };
    type.rooms = std::clamp (std::round (type.rooms * 2) / 2, kMinRooms, kMaxRooms);
    if (type.minM2 > type.maxM2)
        std::swap (type.minM2, type.maxM2);
    return true;
}
} // namespace

bool ParseType (const std::string& text, UnitType& type, std::string& error)
{
    UnitType read;
    if (!Row (Dashes (text), read)) {
        error = "Write the type as \"30% 2 room 40-45m2\".";
        return false;
    }
    if (!std::isfinite (read.share) || read.share < 0 || read.share > 1 || read.minM2 < kMinArea ||
        read.maxM2 > kMaxArea || read.minM2 >= read.maxM2) {
        error = "Share 0-100%, area range rising within 15-300 m2.";
        return false;
    }
    type = read;
    return true;
}

bool Parse (const std::string& text, Programme& programme, std::string& error)
{
    const std::string normal = Dashes (text);
    Programme read;
    size_t start = 0;
    while (start <= normal.size ()) {
        const size_t end = (std::min) (normal.find ('\n', start), normal.size ());
        UnitType type;
        if (Row (normal.substr (start, end - start), type))
            read.types.push_back (type);
        start = end + 1;
    }
    if (read.types.empty ()) {
        error = "No flat type found. Write lines such as \"30% 2 room 40-45m2\".";
        return false;
    }
    if (read.types.size () > kMaxTypes) {
        error = "At most 12 flat types.";
        return false;
    }
    Normalise (read);
    const auto problem = Problem (read);
    if (!problem.empty ()) {
        error = problem;
        return false;
    }
    programme = std::move (read);
    return true;
}

namespace {
constexpr char kHeader[] = "tapioca.programme 1";
}

std::string ToText (const Programme& programme)
{
    std::string text = kHeader;
    char line[128];
    for (const auto& type : programme.types) {
        std::snprintf (line, sizeof (line), "\n%.17g %.17g %.17g %.17g", type.rooms, type.minM2, type.maxM2,
                       type.share);
        text += line;
    }
    return text + "\n";
}

bool FromText (const std::string& text, Programme& programme, std::string& error)
{
    if (text.compare (0, sizeof (kHeader) - 1, kHeader) != 0) {
        error = "The stored programme is not in a known format.";
        return false;
    }
    Programme read;
    size_t start = text.find ('\n');
    while (start != std::string::npos && start + 1 < text.size ()) {
        const size_t end = text.find ('\n', start + 1);
        const std::string row = text.substr (start + 1, end == std::string::npos ? std::string::npos : end - start - 1);
        start = end;
        if (row.find_first_not_of (" \r\t") == std::string::npos)
            continue;
        UnitType type;
        if (std::sscanf (row.c_str (), "%lf %lf %lf %lf", &type.rooms, &type.minM2, &type.maxM2, &type.share) != 4) {
            error = "The stored programme has an unreadable row: " + row;
            return false;
        }
        read.types.push_back (type);
    }
    error = Problem (read);
    if (!error.empty ())
        return false;
    programme = std::move (read);
    return true;
}

std::string Brief (const Programme& programme, const char* separator)
{
    std::string text;
    for (const auto& type : programme.types) {
        if (!text.empty ())
            text += separator;
        text += Text (type.share * 100) + "% " + Text (type.rooms) + " room " + Text (type.minM2) + "-" +
                Text (type.maxM2) + "m2";
    }
    return text;
}
} // namespace geomsrv::archviz::floorprogramme
