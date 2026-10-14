#include "ArchViz/BuildingGenerator.hpp"
#include <clipper2/clipper.h>
#include <algorithm>
#include <cmath>

// Each starting point plans its levels with floorscheme::Generate (a unit: its rooms with
// floorscheme::PlanRooms) and builds the complex from what it planned; a part planned again
// changes its scheme and the complex is built again.
namespace geomsrv::archviz::buildinggenerator {
namespace bt = buildingtopology;
namespace fs = floorscheme;
namespace fp = floorprogramme;

namespace {
namespace cp = Clipper2Lib;
constexpr int kPrecision = 6;

cp::PathsD Paths (const std::vector<Ring>& rings)
{
    cp::PathsD out;
    for (const auto& r : rings) {
        cp::PathD path;
        for (const auto& p : r)
            path.emplace_back (p.x, p.y);
        out.push_back (std::move (path));
    }
    return out;
}
double AreaOf (const std::vector<Ring>& rings)
{
    return std::abs (cp::Area (cp::Union (Paths (rings), cp::FillRule::NonZero, kPrecision)));
}
std::vector<fs::Pins::Core> Cores (const fs::Scheme& scheme)
{
    std::vector<fs::Pins::Core> out;
    for (const auto& core : scheme.cores)
        out.push_back ({ core.centre, core.width, core.depth });
    return out;
}
// One level planned: holding `stack` when the building has one, its own stairs (as pins) when not.
fs::Scheme Plan (const Level& level, const fp::Programme& programme, const std::vector<fs::Pins::Core>& stack,
                 const fs::Options& options)
{
    fs::Pins pins = level.pins;
    fs::Options o = options;
    o.party = level.party;
    o.holdCores = !stack.empty ();
    if (!stack.empty ())
        pins.cores = stack;
    return fs::Generate (level.outline, programme, pins, o);
}
bool InRange (const fp::UnitType& type, double net)
{
    return net >= type.minM2 - 0.5 && net <= type.maxM2 + 0.5;
}
} // namespace

std::vector<Ring> Common (const std::vector<std::vector<Ring>>& levels, const std::vector<Ring>& within, double minArea)
{
    auto common = cp::Union (Paths (within), cp::FillRule::NonZero, kPrecision);
    for (const auto& level : levels)
        common = cp::Intersect (common, cp::Union (Paths (level), cp::FillRule::NonZero, kPrecision),
                                cp::FillRule::NonZero, kPrecision);
    if (std::abs (cp::Area (common)) < minArea)
        return within;
    std::vector<Ring> out;
    for (const auto& path : common) {
        Ring ring;
        for (const auto& p : path)
            ring.push_back ({ p.x, p.y });
        out.push_back (std::move (ring));
    }
    return out;
}

void Rebuild (Generated& generated)
{
    std::vector<bt::FloorInput> floors;
    for (size_t i = 0; i < generated.levels.size () && i < generated.schemes.size (); ++i) {
        const auto& level = generated.levels[i];
        floors.push_back ({ level.story, level.z, level.height, &generated.schemes[i], level.party, level.context });
    }
    const auto before = std::move (generated.complex);
    generated.complex = bt::Build (floors, generated.stack);
    bt::Reconcile (before, generated.complex, generated.nextId);
}

Generated PlanMass (std::vector<Level> levels, const fp::Programme& programme,
                    const std::vector<fs::Pins::Core>& stairs, const fs::Options& options)
{
    std::stable_sort (levels.begin (), levels.end (), [] (const Level& a, const Level& b) { return a.z < b.z; });
    Generated out;
    out.levels = std::move (levels);
    if (out.levels.empty ())
        return out;
    size_t lead = 0;
    double largest = -1;
    std::vector<std::vector<Ring>> outlines;
    for (size_t i = 0; i < out.levels.size (); ++i) {
        outlines.push_back (out.levels[i].outline);
        if (const double area = AreaOf (out.levels[i].outline); area > largest + 1e-6)
            largest = area, lead = i;
    }
    // The stack: the lead level's plan of the floor every level shares, the saved stairs as
    // ordinary pins (they may move to fit, here only).
    Level shared = out.levels[lead];
    shared.outline = Common (outlines, shared.outline);
    shared.pins.cores = stairs;
    out.stack = Cores (Plan (shared, programme, {}, options));
    if (out.stack.empty ())
        out.diagnostics.push_back ({ "stack.none",
                                     "The floor every level has got no stair: each level plans its own.",
                                     static_cast<int> (lead),
                                     {} });
    for (const auto& level : out.levels)
        out.schemes.push_back (Plan (level, programme, out.stack, options));
    Rebuild (out);
    return out;
}

Generated PlanFloor (Level level, const fp::Programme& programme, const std::vector<fs::Pins::Core>& stack,
                     const fs::Options& options)
{
    Generated out;
    out.levels.push_back (std::move (level));
    out.schemes.push_back (Plan (out.levels.front (), programme, stack, options));
    out.stack = stack.empty () ? Cores (out.schemes.front ()) : stack;
    Rebuild (out);
    return out;
}

Generated PlanUnit (Level level, const fp::Programme& programme, size_t type, fs::Vec axis)
{
    Generated out;
    Ring ring;
    for (const auto& r : level.outline)
        if (std::abs (fs::Area (r)) > std::abs (fs::Area (ring)))
            ring = r;
    if (fs::Area (ring) < 0)
        std::reverse (ring.begin (), ring.end ());
    fs::Scheme s;
    if (ring.size () >= 3 && type < programme.types.size ()) {
        fs::Flat flat;
        flat.shape = ring;
        flat.type = type;
        flat.rooms = programme.types[type].rooms;
        flat.gross = flat.net = fs::Area (ring);
        flat.axis = axis;
        flat.manual = true; // one cell until its rooms are laid: what lies around it is read off its walls
        s.outline = { ring };
        s.gross = flat.gross;
        s.flats.push_back (std::move (flat));
        s.targets.assign (programme.types.size (), 0);
        s.targets[type] = 1;
        s.counts = s.targets;
        s.ok = true;
    }
    if (level.outline.size () > 1)
        out.diagnostics.push_back ({ "unit.outline", "A unit is one ring: its largest is planned.", 0, {} });
    level.outline = s.outline;
    out.levels.push_back (std::move (level));
    out.schemes.push_back (std::move (s));
    Rebuild (out);
    if (out.complex.units.empty ()) {
        out.diagnostics.push_back ({ "input.empty", "No unit outline, or no such programme type.", 0, {} });
        return out;
    }
    // Its net area is inside its walls, as the complex measures every flat.
    auto& flat = out.schemes.front ().flats.front ();
    flat.net = bt::Figures (out.complex, 0).clearArea;
    flat.inRange = InRange (programme.types[type], flat.net);
    out.schemes.front ().net = flat.net;
    ReplanUnit (out, 0);
    return out;
}

bool ReplanFloor (Generated& generated, int level, const fp::Programme& programme, const fs::Options& options)
{
    if (level < 0 || level >= static_cast<int> ((std::min) (generated.levels.size (), generated.schemes.size ())))
        return false;
    generated.schemes[level] = Plan (generated.levels[level], programme, generated.stack, options);
    Rebuild (generated);
    return generated.schemes[level].ok;
}

bool ReplanUnit (Generated& generated, int unit)
{
    if (unit < 0 || unit >= static_cast<int> (generated.complex.units.size ()))
        return false;
    const int floor = generated.complex.units[unit].floor, index = generated.complex.units[unit].flat;
    if (floor < 0 || floor >= static_cast<int> (generated.schemes.size ()) || index < 0 ||
        index >= static_cast<int> (generated.schemes[floor].flats.size ()))
        return false;
    auto& flat = generated.schemes[floor].flats[index];
    const bool laid = fs::PlanRooms (flat, bt::Sides (generated.complex, unit));
    const fs::Vec at = flat.shape.empty () ? fs::Vec {} : flat.shape.front ();
    std::erase_if (generated.diagnostics, [&] (const bt::Diagnostic& d) {
        return d.code == "unit.rooms_manual" && d.floor == floor && d.at == at;
    });
    if (!laid)
        generated.diagnostics.push_back (
            { "unit.rooms_manual", "Not a rectangle with facade: its rooms are left to the user.", floor, at });
    Rebuild (generated);
    return laid;
}

bool RetypeUnit (Generated& generated, int unit, const fp::Programme& programme, size_t type)
{
    if (unit < 0 || unit >= static_cast<int> (generated.complex.units.size ()) || type >= programme.types.size ())
        return false;
    const auto& u = generated.complex.units[unit];
    if (u.floor < 0 || u.floor >= static_cast<int> (generated.schemes.size ()))
        return false;
    auto& scheme = generated.schemes[u.floor];
    if (u.flat < 0 || u.flat >= static_cast<int> (scheme.flats.size ()))
        return false;
    auto& flat = scheme.flats[u.flat];
    if (flat.type != type && flat.type < scheme.counts.size () && type < scheme.counts.size ())
        --scheme.counts[flat.type], ++scheme.counts[type];
    flat.type = type;
    flat.rooms = programme.types[type].rooms;
    flat.inRange = InRange (programme.types[type], flat.net);
    return ReplanUnit (generated, unit);
}
} // namespace geomsrv::archviz::buildinggenerator
