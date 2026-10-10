#include "ArchViz/FloorScheme.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <algorithm>
#include <numbers>
#include <sstream>

namespace fs = geomsrv::archviz::floorscheme;
namespace fp = geomsrv::archviz::floorprogramme;

namespace {
fs::Ring Rect (double x0, double y0, double x1, double y1)
{
    return { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
}
fs::Ring Turned (fs::Ring ring, double degrees, fs::Vec origin = {})
{
    const double a = degrees * std::numbers::pi / 180, c = std::cos (a), s = std::sin (a);
    for (auto& p : ring)
        p = { origin.x + p.x * c - p.y * s, origin.y + p.x * s + p.y * c };
    return ring;
}
fs::Ring Reversed (fs::Ring ring)
{
    std::reverse (ring.begin (), ring.end ());
    return ring;
}
struct Case {
    const char* name;
    std::vector<fs::Ring> outline;
};
std::vector<Case> Cases ()
{
    return {
        { "I 60 x 16", { Rect (0, 0, 60, 16) } },
        { "I 36 x 16", { Rect (0, 0, 36, 16) } },
        { "I 60 x 10, sections", { Rect (0, 0, 60, 10) } },
        { "I 48 x 12, sections", { Rect (0, 0, 48, 12) } },
        { "I 40 x 7.5, shallow sections", { Rect (0, 0, 40, 7.5) } },
        { "L 48 x 16 and 16 x 32", { Rect (0, 0, 48, 16), Rect (0, 16, 16, 48) } },
        { "U 60 x 16 with two 16 x 24 arms", { Rect (0, 0, 60, 16), Rect (0, 16, 16, 40), Rect (44, 16, 60, 40) } },
        { "O 50 x 50, 14 deep", { Rect (0, 0, 50, 50), Reversed (Rect (14, 14, 36, 36)) } },
        { "T 60 x 16 with a 16 x 24 stem", { Rect (0, 0, 60, 16), Rect (22, 16, 38, 40) } },
        { "+ 64 x 16 crossed by 16 x 64", { Rect (0, 24, 64, 40), Rect (24, 0, 40, 24), Rect (24, 40, 40, 64) } },
        { "L at 60 degrees", { Rect (0, 0, 44, 16), Turned (Rect (0, 0, 40, 16), 60, { 40, 0 }) } },
        { "I 50 x 16, slanted end", { { { 0, 0 }, { 50, 0 }, { 56, 16 }, { 0, 16 } } } },
        { "I 60 x 16 turned 23 degrees", { Turned (Rect (0, 0, 60, 16), 23, { 1000, 2000 }) } },
        { "I 60 x 14, facade skewed to 15.5", { { { 0, 0 }, { 60, 0 }, { 60, 15.5 }, { 0, 14 } } } },
    };
}
size_t Errors (const fs::Scheme& s)
{
    size_t n = 0;
    for (const auto& d : s.diagnostics)
        n += d.level == fs::Diagnostic::Error;
    return n;
}
std::string Report (const fs::Scheme& s)
{
    std::ostringstream out;
    out << s.typology << ": " << s.flats.size () << " flats, " << s.cores.size () << " stairs\n";
    for (const auto& d : s.diagnostics)
        out << "  [" << static_cast<int> (d.level) << "] " << d.code << " " << d.text << " at " << d.at.x << ","
            << d.at.y << "\n";
    return out.str ();
}
} // namespace

TEST (FloorScheme, RoomWidthsGiveTheUsersFrontages)
{
    // 1R 3.5 + 0.2, 2R 3.7 + 0.1 + 2.7 + 0.2, 3R 4 + 0.1 + 2.7 + 0.1 + 2.7 + 0.2.
    EXPECT_NEAR (fs::RoomFrontage (1).pref, 3.7, 1e-9);
    EXPECT_NEAR (fs::RoomFrontage (2).pref, 6.7, 1e-9);
    EXPECT_NEAR (fs::RoomFrontage (3).pref, 9.8, 1e-9);
    EXPECT_NEAR (fs::RoomFrontage (1).min, 3.5, 1e-9);
    EXPECT_NEAR (fs::RoomFrontage (3).min, 3.3 + 2 * 2.7 + 0.2, 1e-9);
    EXPECT_LT (fs::RoomFrontage (1.5).min, fs::RoomFrontage (2).min);
}

TEST (FloorScheme, SkeletonNamesTheTypology)
{
    const auto p = fp::Default ();
    std::map<std::string, std::string> expected {
        { "I 60 x 16", "I" },
        { "L 48 x 16 and 16 x 32", "L" },
        { "U 60 x 16 with two 16 x 24 arms", "U" },
        { "O 50 x 50, 14 deep", "O" },
        { "T 60 x 16 with a 16 x 24 stem", "T" },
        { "+ 64 x 16 crossed by 16 x 64", "+" },
    };
    for (const auto& c : Cases ()) {
        const auto it = expected.find (c.name);
        if (it == expected.end ())
            continue;
        const auto s = fs::Generate (c.outline, p);
        EXPECT_EQ (s.typology, it->second) << c.name;
    }
    const auto angled = fs::Generate (Cases ()[10].outline, p);
    EXPECT_EQ (angled.typology.substr (0, 1), "V") << angled.typology;
}

TEST (FloorScheme, StandardShapesKeepEveryHardRule)
{
    const auto p = fp::Default ();
    for (const auto& c : Cases ()) {
        const auto s = fs::Generate (c.outline, p);
        EXPECT_EQ (Errors (s), 0u) << c.name << "\n" << Report (s);
        EXPECT_FALSE (s.flats.empty ()) << c.name;
        for (const auto& corridor : s.corridors)
            EXPECT_LE (corridor.axis.size (), 3u) << c.name; // straight or one L
        for (const auto& f : s.flats)
            EXPECT_GE (f.frontage,
                       (std::min) (fs::RoomFrontage (f.rooms).min, fs::ThroughFrontage (f.rooms).min) - 1e-6)
                << c.name;
    }
}

TEST (FloorScheme, StraightBarHasCentreCorridorCapsAndCornerFlats)
{
    const auto s = fs::Generate ({ Rect (0, 0, 36, 16) }, fp::Default ());
    ASSERT_EQ (s.cores.size (), 1u);
    ASSERT_EQ (s.corridors.size (), 1u);
    EXPECT_EQ (s.corridors.front ().axis.size (), 2u);
    int caps = 0, corners = 0;
    for (const auto& f : s.flats)
        caps += f.cap, corners += f.corner;
    EXPECT_EQ (caps, 2);
    EXPECT_GE (corners, 4); // both caps and both ends of the running band
    // The living room of a corner flat is in the corner.
    for (const auto& f : s.flats) {
        if (!f.corner || f.cap)
            continue;
        const auto living = std::find_if (f.roomList.begin (), f.roomList.end (),
                                          [] (const fs::Room& r) { return r.kind == fs::RoomKind::Living; });
        ASSERT_NE (living, f.roomList.end ());
        double x0 = 1e9, x1 = -1e9;
        for (const auto& q : living->shape)
            x0 = (std::min) (x0, q.x), x1 = (std::max) (x1, q.x);
        EXPECT_TRUE (x0 < 0.2 || x1 > 35.8) << x0 << " " << x1;
    }
}

TEST (FloorScheme, LargeBarSplitsIntoSectionsOfAtMostTheStairArea)
{
    const auto s = fs::Generate ({ Rect (0, 0, 60, 16) }, fp::Default ());
    EXPECT_EQ (s.sections.size (), 2u);
    for (const auto& section : s.sections)
        EXPECT_LE (section.gross * 0.78, 500.0 + 1e-6);
    EXPECT_EQ (s.cores.size (), 2u);
}

TEST (FloorScheme, SectionsHoldThreeOrFourFlatsPerStair)
{
    const auto s = fs::Generate ({ Rect (0, 0, 48, 12) }, fp::Default ());
    ASSERT_FALSE (s.cores.empty ());
    EXPECT_TRUE (s.corridors.empty ());
    const double perStair = static_cast<double> (s.flats.size ()) / static_cast<double> (s.cores.size ());
    EXPECT_GE (perStair, 3.0);
    EXPECT_LE (perStair, 4.0);
    EXPECT_EQ (Errors (s), 0u) << Report (s);
}

TEST (FloorScheme, CorridorOnOneSideWhenPinned)
{
    fs::Pins pins;
    pins.access.push_back ({ { 30, 5 }, fs::Access::OneSide });
    const auto s = fs::Generate ({ Rect (0, 0, 60, 10) }, fp::Default (), pins);
    EXPECT_FALSE (s.corridors.empty ());
    EXPECT_EQ (Errors (s), 0u) << Report (s);
}

TEST (FloorScheme, CornerFlatsPutTheLivingRoomInTheCornerWithABedroomOnEachFacade)
{
    const auto s = fs::Generate ({ Rect (0, 0, 36, 16) }, fp::Default ());
    int checked = 0;
    for (const auto& f : s.flats) {
        if (!f.corner || f.rooms < 3)
            continue;
        int beds = 0;
        for (const auto& r : f.roomList)
            beds += r.kind == fs::RoomKind::Bedroom;
        EXPECT_EQ (beds, static_cast<int> (f.rooms) - 1);
        EXPECT_GE (f.windows.size (), static_cast<size_t> (f.rooms) + 1); // the living room has two
        ++checked;
    }
    EXPECT_GT (checked, 0);
}

TEST (FloorScheme, PinnedStairMovesAtMostThreeMetres)
{
    fs::Pins pins;
    pins.cores.push_back ({ { 14.0, 14.0 } });
    const auto s = fs::Generate ({ Rect (0, 0, 36, 16) }, fp::Default (), pins);
    ASSERT_EQ (s.cores.size (), 1u);
    EXPECT_LE (std::abs (s.cores.front ().centre.x - 14.0), fs::kPinTolerance + 1e-6);
    EXPECT_TRUE (s.cores.front ().pinned);
    EXPECT_EQ (Errors (s), 0u) << Report (s);
}

TEST (FloorScheme, PinnedWallAndRoomCountAreKept)
{
    const auto base = fs::Generate ({ Rect (0, 0, 36, 16) }, fp::Default ());
    fs::Pins pins;
    pins.walls.push_back ({ { 18.0, 3.0 } });
    pins.rooms.push_back ({ { 10.0, 3.0 }, 2 });
    const auto s = fs::Generate ({ Rect (0, 0, 36, 16) }, fp::Default (), pins);
    bool wall = false, rooms = false;
    for (const auto& f : s.flats) {
        double x0 = 1e9, x1 = -1e9, y0 = 1e9;
        for (const auto& q : f.shape)
            x0 = (std::min) (x0, q.x), x1 = (std::max) (x1, q.x), y0 = (std::min) (y0, q.y);
        if (y0 > 1)
            continue;
        wall = wall || std::abs (x0 - 18.0) <= fs::kPinTolerance || std::abs (x1 - 18.0) <= fs::kPinTolerance;
        if (x0 <= 10 && x1 >= 10)
            rooms = std::abs (f.rooms - 2) < 1e-9;
    }
    EXPECT_TRUE (wall);
    EXPECT_TRUE (rooms) << Report (s);
    EXPECT_EQ (Errors (s), 0u) << Report (s);
    EXPECT_NE (base.flats.size (), 0u);
}

TEST (FloorScheme, SkewedFacadeExtendsThePartyWallsStraight)
{
    // The far facade drifts 1.5 m over the bar: what the inscribed wing leaves joins each flat
    // between its own walls, so every flat edge is square to its band or lies on the outline.
    const fs::Ring outline = Cases ().back ().outline.front ();
    const auto s = fs::Generate ({ outline }, fp::Default ());
    EXPECT_EQ (Errors (s), 0u) << Report (s);
    auto onOutline = [&] (fs::Vec p) {
        for (size_t i = 0; i < outline.size (); ++i) {
            const auto a = outline[i], b = outline[(i + 1) % outline.size ()];
            const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
            const double t = std::clamp (((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.0, 1.0);
            if (std::hypot (a.x + t * dx - p.x, a.y + t * dy - p.y) < 0.01)
                return true;
        }
        return false;
    };
    for (const auto& f : s.flats)
        for (size_t i = 0; i < f.shape.size (); ++i) {
            const auto a = f.shape[i], b = f.shape[(i + 1) % f.shape.size ()];
            const double len = std::hypot (b.x - a.x, b.y - a.y);
            const double along = std::abs ((b.x - a.x) * f.axis.x + (b.y - a.y) * f.axis.y) / len;
            const bool square = along < 1e-3 || along > 1 - 1e-6;
            EXPECT_TRUE (square || (onOutline (a) && onOutline (b)))
                << "slanted inner edge " << a.x << "," << a.y << " -> " << b.x << "," << b.y;
        }
    double red = 0;
    for (const auto& u : s.unassigned)
        red += std::abs (fs::Area (u.shape));
    EXPECT_LT (red, 1.0);
}

// Offline review: FLOORSCHEME_REVIEW=<file.json> writes every case (and the floors of
// FLOORSCHEME_FIXTURES, "floor <name>" then "ring x y x y ..." lines) for the review page.
TEST (FloorScheme, ReviewDump)
{
    const char* path = std::getenv ("FLOORSCHEME_REVIEW");
    if (!path)
        GTEST_SKIP () << "FLOORSCHEME_REVIEW not set";
    auto cases = Cases ();
    std::vector<std::string> names;
    if (const char* extra = std::getenv ("FLOORSCHEME_FIXTURES")) {
        std::ifstream in (extra);
        std::string line;
        while (std::getline (in, line)) {
            std::istringstream words (line);
            std::string head;
            words >> head;
            if (head == "floor") {
                std::string name;
                std::getline (words, name);
                names.push_back (name.substr (name.find_first_not_of (' ')));
                cases.push_back ({ names.back ().c_str (), {} });
            }
            else if (head == "ring" && !cases.empty ()) {
                fs::Ring ring;
                double x, y;
                while (words >> x >> y)
                    ring.push_back ({ x, y });
                cases.back ().outline.push_back (ring);
            }
        }
        for (size_t i = 0; i < names.size (); ++i)
            cases[cases.size () - names.size () + i].name = names[i].c_str ();
    }
    std::ofstream out (path);
    out << std::setprecision (10) << "[";
    auto ring = [&] (const fs::Ring& r) {
        out << "[";
        for (size_t i = 0; i < r.size (); ++i)
            out << (i ? "," : "") << "[" << r[i].x << "," << r[i].y << "]";
        out << "]";
    };
    auto rings = [&] (const char* key, const std::vector<fs::Ring>& list) {
        out << ",\"" << key << "\":[";
        for (size_t i = 0; i < list.size (); ++i)
            out << (i ? "," : ""), ring (list[i]);
        out << "]";
    };
    auto text = [&] (const std::string& s) {
        out << "\"";
        for (char ch : s)
            out << (ch == '"' || ch == '\\' ? "\\" : "") << ch;
        out << "\"";
    };
    const auto p = fp::Default ();
    for (size_t k = 0; k < cases.size (); ++k) {
        const auto s = fs::Generate (cases[k].outline, p);
        out << (k ? "," : "") << "{\"name\":";
        text (cases[k].name);
        out << ",\"typology\":";
        text (s.typology);
        out << ",\"gross\":" << s.gross << ",\"net\":" << s.net << ",\"circulation\":" << s.circulation;
        rings ("outline", s.outline);
        std::vector<fs::Ring> list;
        for (const auto& w : s.wings)
            list.push_back (w.rect);
        rings ("wings", list);
        out << ",\"axes\":[";
        for (size_t i = 0; i < s.wings.size (); ++i)
            out << (i ? "," : "") << "[[" << s.wings[i].a.x << "," << s.wings[i].a.y << "],[" << s.wings[i].b.x << ","
                << s.wings[i].b.y << "],\"" << static_cast<int> (s.wings[i].access) << "\"]";
        out << "]";
        list.clear ();
        for (const auto& c : s.corridors)
            list.push_back (c.shape);
        rings ("corridors", list);
        rings ("lobbies", s.lobbies);
        list.clear ();
        for (const auto& c : s.cores)
            list.push_back (c.shape);
        rings ("cores", list);
        list.clear ();
        for (const auto& b : s.bands)
            list.push_back (b.shape);
        rings ("bands", list);
        out << ",\"unassigned\":[";
        for (size_t i = 0; i < s.unassigned.size (); ++i) {
            out << (i ? "," : "") << "{\"reason\":";
            text (s.unassigned[i].reason);
            out << ",\"ring\":";
            ring (s.unassigned[i].shape);
            out << "}";
        }
        out << "],\"flats\":[";
        for (size_t i = 0; i < s.flats.size (); ++i) {
            const auto& f = s.flats[i];
            out << (i ? "," : "") << "{\"type\":";
            text (fp::Name (p, f.type));
            out << ",\"rooms\":" << f.rooms << ",\"net\":" << f.net << ",\"frontage\":" << f.frontage
                << ",\"depth\":" << f.depth << ",\"cap\":" << f.cap << ",\"corner\":" << f.corner
                << ",\"inRange\":" << f.inRange << ",\"ring\":";
            ring (f.shape);
            out << ",\"rooms_\":[";
            for (size_t r = 0; r < f.roomList.size (); ++r) {
                out << (r ? "," : "") << "{\"kind\":" << static_cast<int> (f.roomList[r].kind)
                    << ",\"width\":" << f.roomList[r].width << ",\"ring\":";
                ring (f.roomList[r].shape);
                out << "}";
            }
            out << "],\"windows\":[";
            for (size_t w = 0; w < f.windows.size (); ++w)
                out << (w ? "," : "") << "[[" << f.windows[w].a.x << "," << f.windows[w].a.y << "],["
                    << f.windows[w].b.x << "," << f.windows[w].b.y << "]]";
            out << "],\"door\":[[" << f.door.a.x << "," << f.door.a.y << "],[" << f.door.b.x << "," << f.door.b.y
                << "]]}";
        }
        out << "],\"targets\":[";
        for (size_t i = 0; i < s.targets.size (); ++i)
            out << (i ? "," : "") << s.targets[i];
        out << "],\"counts\":[";
        for (size_t i = 0; i < s.counts.size (); ++i)
            out << (i ? "," : "") << s.counts[i];
        out << "],\"diagnostics\":[";
        for (size_t i = 0; i < s.diagnostics.size (); ++i) {
            const auto& d = s.diagnostics[i];
            out << (i ? "," : "") << "{\"level\":" << static_cast<int> (d.level) << ",\"code\":";
            text (d.code);
            out << ",\"text\":";
            text (d.text);
            out << ",\"at\":[" << d.at.x << "," << d.at.y << "]}";
        }
        out << "]}";
    }
    out << "]";
}
