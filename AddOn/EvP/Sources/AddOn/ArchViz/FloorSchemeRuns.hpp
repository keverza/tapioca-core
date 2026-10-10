#ifndef EVP_ARCHVIZ_FLOORSCHEMERUNS_HPP
#define EVP_ARCHVIZ_FLOORSCHEMERUNS_HPP

// Shared by the two circulation translation units: FloorSchemeRuns.cpp (straight runs, their
// sections, access) and FloorSchemeCorners.cpp (L runs and bends). Boxes are drawn in a run
// frame as in FloorSchemeDetail.hpp.
#include "ArchViz/FloorSchemeDetail.hpp"

namespace geomsrv::archviz::floorscheme::detail::runs {
constexpr double kRowMin = 5.5; // a single-aspect row: rooms and a strip of services behind

struct Ctx {
    cp::PathsD outline; // less what the typology culls, as the culling happens
    const floorprogramme::Programme& programme;
    const Pins& pins;
    const Options& o;
    std::vector<Diagnostic>& notes;
    Layout out;
    std::vector<char> fill; // Fillable by 0.1 m
    cp::PathsD blind;       // neighbouring buildings' floor (Options::party): party walls, no facade
    bool Fits (double length) const
    {
        if (length < 0.05)
            return true;
        // Rounded down: a band a little short of a flat's minimum must not read as fillable
        // (a little long only makes the flats wider).
        const size_t i = static_cast<size_t> (std::floor (length * 10 + 1e-6));
        return i < fill.size () ? fill[i] != 0 : Fillable (programme, length);
    }
};
inline Frame Mirror (const Frame& f)
{
    Frame m = f;
    m.v = { -f.v.x, -f.v.y };
    return m;
}
inline Frame FlipU (const Frame& f, double length)
{
    Frame m = f;
    m.o = f.World (length, 0);
    m.u = { -f.u.x, -f.u.y };
    return m;
}

// Facade length on one side of a box: 0 at v0, 1 at u1, 2 at v1, 3 at u0.
double SideFacade (const Ctx& c, const Frame& f, Box b, int side);
// A rectangle to divide, in frame f; `facadeHigh` says its facade is at v1 of f (else at v0,
// and the slot is mirrored so its facade is always v1). The door interval is in f too.
void AddSlot (Ctx& c, const Frame& f, Box b, bool facadeHigh, int section, int door = 0, int count = 0,
              bool darkCheck = true, double doorLo = -1e18, double doorHi = 1e18, Box extra = {});
// Facade parts of a band piece, split where the facade is covered for 2 m or more (as
// AddSlot splits them); each must take whole flats or be empty.
bool PieceFits (const Ctx& c, const Frame& f, Box b, bool facadeHigh);
// A flat that owns a corridor end; its door is on the corridor end, where the corridor
// [cv0, cv1] (v in f) meets it.
void AddCap (Ctx& c, const Frame& f, Box b, bool doorHigh, int section, double cv0, double cv1);
void AddCore (Ctx& c, const Frame& f, Box b, int section, bool pinned);
void AddCorridor (Ctx& c, const Frame& f, const std::vector<Box>& boxes, const std::vector<Vec>& axis, int section);
int AddSection (Ctx& c, int run, double gross, char access);
Vec North (const Options& o);
// A pinned core in this box of the frame, nearest first; null when none.
const Pins::Core* PinIn (const Ctx& c, const Frame& f, Box b, double margin);
// How far a pinned stair may move: none when the pins are the building's stack.
inline double CoreSlack (const Options& o)
{
    return o.holdCores ? 0.0 : kPinTolerance;
}
// Cap lengths from corridor-end pins near this section's ends.
void EndPins (const Ctx& c, const Frame& f, Box section, double c0, double c1, double& capLo, double& capHi);

struct Portion {
    int wing = -1;
    double s0 = 0, s1 = 0;
};
// One wing span: split into sections by stair area and dead ends, then laid out.
void Straight (Ctx& c, const Skeleton& sk, Portion p, Access access, int run);

// FloorSchemeCorners.cpp
struct Corner {
    int parent = -1, child = -1;
    int parentEnd = 0, childEnd = 0; // 0: u = 0, 1: u = length
    double score = 0;
    bool single = false; // the arm is a short shallow stub: its corridor runs along its inner side
};
// Frame of an L: u along the parent from the corner, the child on the +v side at u = 0.
Frame CornerFrame (const Skeleton& sk, const Corner& k);
// One L run; what is left of either arm goes to `rest` as straight portions.
void LRun (Ctx& c, const Skeleton& sk, const Corner& k, Portion pa, Portion pb, int run, std::vector<Portion>& rest);
std::vector<Corner> Corners (const Skeleton& sk, const std::vector<Access>& access, double corridor);
// The parent `ia` and its angled arm `ib`: the bend section and what is left of each wing as
// straight runs. False when the pair is no clean bend (the caller plans them straight).
bool Bend (Ctx& c, const Skeleton& sk, int ia, int ib, int& run);
} // namespace geomsrv::archviz::floorscheme::detail::runs
#endif
