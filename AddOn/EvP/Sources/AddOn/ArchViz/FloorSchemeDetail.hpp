#ifndef EVP_ARCHVIZ_FLOORSCHEMEDETAIL_HPP
#define EVP_ARCHVIZ_FLOORSCHEMEDETAIL_HPP

// Shared by the FloorScheme translation units only. Every layout is drawn in a run frame
// (u along a wing, v across it) as axis-aligned boxes; Clipper works in world XY.
#include "ArchViz/FloorScheme.hpp"
#include <clipper2/clipper.h>
#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::floorscheme::detail {
namespace cp = Clipper2Lib;
constexpr double kEps = 1e-6;
constexpr int kPrecision = 6;
// How far beyond a side the facade test looks: real outlines are a little skewed, so an
// inscribed rectangle can sit some decimetres inside its facade.
constexpr double kFacadeOffset = 0.8;

struct Frame {
    Vec o, u { 1, 0 }, v { 0, 1 }; // v may be either perpendicular (a mirrored frame)
    Vec World (double s, double t) const
    {
        return { o.x + s * u.x + t * v.x, o.y + s * u.y + t * v.y };
    }
    Vec Local (Vec p) const
    {
        const double x = p.x - o.x, y = p.y - o.y;
        return { x * u.x + y * u.y, x * v.x + y * v.y };
    }
};
struct Box {
    double u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    double W () const
    {
        return u1 - u0;
    }
    double H () const
    {
        return v1 - v0;
    }
    bool Empty () const
    {
        return W () < 0.05 || H () < 0.05;
    }
};
// std::clamp with the lower bound winning when the range is empty (short spans).
inline double Clip (double x, double lo, double hi)
{
    return hi < lo ? lo : (std::min) ((std::max) (x, lo), hi);
}
inline Ring Counter (Ring ring)
{
    if (Area (ring) < 0)
        std::reverse (ring.begin (), ring.end ());
    return ring;
}
inline Ring ToRing (const Frame& f, Box b)
{
    return Counter ({ f.World (b.u0, b.v0), f.World (b.u1, b.v0), f.World (b.u1, b.v1), f.World (b.u0, b.v1) });
}
inline cp::PathD ToPath (const Ring& ring)
{
    cp::PathD path;
    path.reserve (ring.size ());
    for (const auto& p : ring)
        path.emplace_back (p.x, p.y);
    return path;
}
inline cp::PathsD ToPaths (const std::vector<Ring>& rings)
{
    cp::PathsD out;
    for (const auto& ring : rings)
        out.push_back (ToPath (ring));
    return out;
}
inline Ring FromPath (const cp::PathD& path)
{
    Ring ring;
    for (const auto& p : path)
        ring.push_back ({ p.x, p.y });
    return ring;
}
inline std::vector<Ring> FromPaths (const cp::PathsD& paths)
{
    std::vector<Ring> out;
    for (const auto& path : paths)
        out.push_back (FromPath (path));
    return out;
}
inline double Dot (Vec a, Vec b)
{
    return a.x * b.x + a.y * b.y;
}
inline Vec Unit (Vec a)
{
    const double l = std::hypot (a.x, a.y);
    return l > kEps ? Vec { a.x / l, a.y / l } : Vec { 1, 0 };
}
// Points on a boundary count as inside (ray cast; see HudFloorPlanFrame for why not Clipper's).
bool Inside (const cp::PathsD& paths, Vec p);
// Outline length along the segment a-b that faces outside: samples just beyond the side.
double FacadeAlong (const cp::PathsD& outline, Vec a, Vec b, Vec outward);
double Area (const cp::PathsD& paths);

// Shared by Generate and Check (FloorScheme.cpp, FloorSchemeCheck.cpp).
double Overlap (const Ring& a, const Ring& b);
// Boundary shared by two touching polygons, from the overlap of a thin band around `a`.
double Touch (const Ring& a, const Ring& b, double band = 0.05);
// Facade length of a ring: its edges that face outside the outline.
double FacadeOf (const cp::PathsD& outline, const Ring& ring);
void Note (Scheme& s, Diagnostic::Level level, const char* code, const std::string& text, Vec at);
Vec Centroid (const Ring& r);
// Bounding-box overlap test before the Clipper work.
bool Near (const Ring& a, const Ring& b, double gap);

// T1: skeleton.
struct Skeleton {
    std::vector<Wing> wings;
    std::vector<Frame> frames; // per wing: u along its axis, box [0, length] x [0, depth]
    std::vector<Ring> rest;    // outline the wings leave out (mould pieces)
    std::string typology;
};
Skeleton Decompose (const cp::PathsD& outline, const Options& options, std::vector<Diagnostic>& notes);
// Largest rectangle inside `area` whose sides run along `theta` (frame u) and across it.
bool Inscribed (const cp::PathsD& area, double theta, double minDepth, double minLength, Frame& frame, Box& box);

// A rectangle to divide into flats along u; its facade is the v1 side, its door side is
// v0 (a corridor), or a u end (a stair landing) for sectional flats.
struct Slot {
    Frame frame;
    Box box;
    int section = -1, band = -1;
    bool cap = false;
    bool through = false;                    // v0 is a facade too
    bool cornerLo = false, cornerHi = false; // u0 / u1 on a facade
    int door = 0;                            // 0: v0, 1: the u0 end, 2: the u1 end
    double doorLo = -1e18, doorHi = 1e18;    // where circulation meets that side: u on v0, v on an end
    int count = 0;                           // 0 free, else exactly this many flats
    Box extra;                               // a hall reaching past the box to the stair (sectional)
};
// T2-T4: circulation and the slots between it and the facade.
struct Layout {
    std::vector<Section> sections;
    std::vector<Corridor> corridors;
    std::vector<Ring> lobbies;
    std::vector<Core> cores;
    std::vector<Slot> slots;
    std::vector<Piece> storage; // band without facade
    std::vector<Piece> culled;  // left out of the massing (an L's outer corner bay)
};
Layout Circulate (const cp::PathsD& outline, Skeleton& skeleton, const floorprogramme::Programme& programme,
                  const Pins& pins, const Options& options, std::vector<Diagnostic>& notes);
// True when `length` of facade can be divided into whole flats of the programme's types.
bool Fillable (const floorprogramme::Programme& programme, double length);
// Where a flat's door meets circulation: side 0 is v0 (lo/hi along u), 1 the u0 end and 2 the
// u1 end (lo/hi along v), all in the slot frame.
struct DoorAt {
    int side = 0;
    double lo = -1e18, hi = 1e18;
};
// T6: rooms, windows and the entrance of one flat; `corner` -1 none, 0 at u0, 1 at u1.
void LayRooms (Flat& flat, const Slot& slot, Box b, int corner, DoorAt door);
// T5-T6: flats and rooms in every slot, counted against the programme.
// `resume` continues the counts already in `scheme` (a second pass on leftover floor).
void Divide (const floorprogramme::Programme& programme, const std::vector<Slot>& slots, Scheme& scheme,
             const Pins& pins, const Options& options, bool resume = false);
} // namespace geomsrv::archviz::floorscheme::detail
#endif
