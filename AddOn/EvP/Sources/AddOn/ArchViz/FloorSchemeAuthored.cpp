#include "ArchViz/FloorSchemeDetail.hpp"
#include <cstdarg>
#include <cstdio>

// The skeleton as the user has it: a wing drawn instead of searched for (Pins::wings) and a
// corridor moved off its wing's middle or made another width (Pins::Corridor). One rule says
// whether a wing can be a wing of the floor, for the generator (which says it and leaves the wing
// out) and for the edits (which refuse the gesture), so the two never disagree.
namespace geomsrv::archviz::floorscheme::detail {
namespace {
// A wing may lean this much past the floor, or over another wing: the generator's own wings are
// grown to within a few hundredths of a m2 of both, and a saved wing is kept to the millimetre.
constexpr double kWingSlack = 0.25; // m2

std::string Text (const char* format, ...)
{
    char text[256];
    va_list args;
    va_start (args, format);
    std::vsnprintf (text, sizeof (text), format, args);
    va_end (args);
    return text;
}
} // namespace

Wing AuthoredWing (const Pins::Wing& pin, Frame& f)
{
    Wing w;
    w.length = std::hypot (pin.b.x - pin.a.x, pin.b.y - pin.a.y);
    w.depth = pin.depth;
    w.id = pin.id, w.mirror = pin.mirror;
    f.u = Unit ({ pin.b.x - pin.a.x, pin.b.y - pin.a.y });
    f.v = pin.mirror ? Vec { f.u.y, -f.u.x } : Vec { -f.u.y, f.u.x };
    f.o = { pin.a.x - f.v.x * w.depth / 2, pin.a.y - f.v.y * w.depth / 2 };
    w.rect = Counter ({ f.World (0, 0), f.World (w.length, 0), f.World (w.length, w.depth), f.World (0, w.depth) });
    w.a = f.World (0, w.depth / 2), w.b = f.World (w.length, w.depth / 2);
    return w;
}

std::string WingFault (const cp::PathsD& floor, const std::vector<Ring>& others, const Pins::Wing& pin,
                       const Options& o, const char** code)
{
    auto fault = [&] (const char* name, std::string text) {
        if (code)
            *code = name;
        return text;
    };
    const double length = std::hypot (pin.b.x - pin.a.x, pin.b.y - pin.a.y);
    if (!std::isfinite (length) || !std::isfinite (pin.depth) || length < 0.1 || pin.depth < 0.1)
        return fault ("skeleton.wing_malformed", Text ("Wing %u has no length or no depth.", pin.id));
    // A wing that grows out of another's side is as long as that side is deep: its shorter side is
    // its depth whichever way it was drawn.
    const double across = (std::min) (length, pin.depth), along = (std::max) (length, pin.depth);
    if (across < o.minWingDepth - 1e-6)
        return fault ("skeleton.wing_shallow",
                      Text ("Wing %u is %.1f m deep: a wing is at least %.1f m deep.", pin.id, across, o.minWingDepth));
    if (across > o.maxWingDepth + 1e-6)
        return fault ("skeleton.wing_deep", Text ("Wing %u is %.1f m deep: a wing is at most %.1f m deep, or its "
                                                  "flats have a dark inner band.",
                                                  pin.id, across, o.maxWingDepth));
    if (along < o.minWingLength - 1e-6)
        return fault ("skeleton.wing_short",
                      Text ("Wing %u is %.1f m long: a wing is at least %.1f m long.", pin.id, along, o.minWingLength));
    Frame frame;
    const cp::PathsD rect { ToPath (AuthoredWing (pin, frame).rect) };
    if (const double out = Area (cp::Difference (rect, floor, cp::FillRule::NonZero, kPrecision)); out > kWingSlack)
        return fault ("skeleton.wing_outside",
                      Text ("%.1f m2 of wing %u lies outside the floor (at most %.2f m2).", out, pin.id, kWingSlack));
    for (const auto& other : others)
        if (const double over = Area (cp::Intersect (rect, { ToPath (other) }, cp::FillRule::NonZero, kPrecision));
            over > kWingSlack)
            return fault ("skeleton.wing_overlap", Text ("Wing %u lies over another wing by %.1f m2 (at most %.2f m2).",
                                                         pin.id, over, kWingSlack));
    return {};
}

Lane LaneFor (const Pins& pins, const Options& o, const Frame& f, double s0, double s1, double depth, bool centre,
              std::vector<char>& used, std::vector<Diagnostic>& notes)
{
    Lane lane { (depth - o.corridor) / 2, 0 };
    lane.c1 = lane.c0 + o.corridor;
    if (used.size () < pins.corridors.size ())
        used.resize (pins.corridors.size (), 0);
    for (size_t i = 0; i < pins.corridors.size (); ++i) {
        const auto& pin = pins.corridors[i];
        const auto q = f.Local (pin.at);
        if (q.x < s0 || q.x > s1 || q.y < -0.01 || q.y > depth + 0.01)
            continue;
        used[i] = 1;
        const double asked = pin.width > 0 ? pin.width : o.corridor;
        const double width = Clip (asked, kCorridorMin, kCorridorMax);
        if (std::abs (width - asked) > 1e-6)
            notes.push_back ({ Diagnostic::Info, "pin.corridor_width",
                               Text ("A corridor asked to be %.2f m wide is %.2f m wide (%.1f to %.1f m).", asked,
                                     width, kCorridorMin, kCorridorMax),
                               pin.at });
        if (!centre)
            return { 0, width }; // on one side: it keeps its facade and takes the width
        // A row of flats either side; a wing too shallow for that keeps its corridor in the middle.
        const double lo = kRowDepthMin, hi = depth - width - kRowDepthMin, wanted = q.y - width / 2;
        const double c0 = hi < lo ? (depth - width) / 2 : Clip (wanted, lo, hi);
        if (std::abs (c0 - wanted) > 0.05)
            notes.push_back ({ Diagnostic::Info, "pin.corridor_moved",
                               Text ("A moved corridor stands %.2f m from where it was put: a row of flats beside it "
                                     "is at least %.1f m deep (this wing is %.1f m deep).",
                                     std::abs (c0 - wanted), kRowDepthMin, depth),
                               pin.at });
        return { c0, c0 + width };
    }
    return lane;
}
} // namespace geomsrv::archviz::floorscheme::detail
