#include "ArchViz/FloorSchemeDetail.hpp"

// The two stages a floor is planned to. `Analyse` stops at the skeleton (T0-T4): wings, access,
// corridors, stairs and bands, to look at and change before any flat is made. `Generate` goes on
// to the flats (T5-T7) and, where a thin wing can be planned two ways, keeps the better floor.
namespace geomsrv::archviz::floorscheme {
namespace {
// Net area less what the stairs cost and the floor left unassigned; a broken scheme loses. A flat
// counts for no more than the largest flat there is (`cap`): two halls of 150 m2 either side of one
// stair are not worth more than four flats on a corridor (user, 2026-11-15: a thin bar with a
// corridor on one edge was passed over for them).
double Economy (const Scheme& s, const Options& o, double cap)
{
    double red = 0, net = 0;
    for (const auto& u : s.unassigned)
        if (u.reason.find ("storage") == std::string::npos)
            red += std::abs (Area (u.shape));
    for (const auto& f : s.flats)
        net += (std::min) (f.net, cap);
    return net - o.stairCost * static_cast<double> (s.cores.size ()) - red - (s.ok ? 0.0 : 1e6);
}
} // namespace

Scheme Analyse (const std::vector<Ring>& outline, const floorprogramme::Programme& programme, const Pins& pins,
                const Options& options)
{
    return detail::PlanFloor (outline, programme, pins, options, true);
}

namespace {
// The floor planned every way it can be: as the options say, and -- when they leave a thin wing's
// access open and the floor has one -- with a corridor on one side too. The first is sections round
// stairs (what an open choice plans first). `chosen` indexes the one that scores better.
struct Weighed {
    std::vector<std::pair<Access, Scheme>> plans;
    std::vector<double> scores;
    size_t chosen = 0;
};
Weighed Weigh (const std::vector<Ring>& outline, const floorprogramme::Programme& programme, const Pins& pins,
               const Options& options)
{
    Weighed w;
    w.plans.emplace_back (options.shallow, detail::PlanFloor (outline, programme, pins, options));
    w.scores.push_back (Economy (w.plans[0].second, options, MaxFlat (programme, options)));
    if (options.shallow != Access::Auto)
        return w;
    // Typology adaptation (user, 2026-10-10): shallow wings planned as sections are weighed
    // against a corridor on one side, the economic plan of a thin bar.
    bool sections = false;
    for (const auto& wing : w.plans[0].second.wings)
        sections = sections || wing.access == Access::CoreOnly;
    if (!sections)
        return w;
    w.plans[0].first = Access::CoreOnly; // what the open choice planned
    Options corridor = options;
    corridor.shallow = Access::OneSide;
    w.plans.emplace_back (Access::OneSide, detail::PlanFloor (outline, programme, pins, corridor));
    w.scores.push_back (Economy (w.plans[1].second, options, MaxFlat (programme, options)));
    w.chosen = w.scores[1] > w.scores[0] ? 1 : 0;
    return w;
}
} // namespace

Scheme Generate (const std::vector<Ring>& outline, const floorprogramme::Programme& programme, const Pins& pins,
                 const Options& options)
{
    auto w = Weigh (outline, programme, pins, options);
    auto& kept = w.plans[w.chosen].second;
    if (w.plans.size () > 1) {
        const double a = w.scores[0], b = w.scores[1];
        kept.diagnostics.push_back (
            { Diagnostic::Info,
              "typology.choice",
              std::string (b > a ? "A corridor on one side" : "Sections round stairs") + " scored " +
                  std::to_string (static_cast<int> (std::round ((std::max) (a, b)))) + " against " +
                  std::to_string (static_cast<int> (std::round ((std::min) (a, b)))) +
                  " (net m2 less stairs and unassigned).",
              {} });
    }
    return std::move (kept);
}

std::vector<Candidate> Candidates (const std::vector<Ring>& outline, const floorprogramme::Programme& programme,
                                   const Pins& pins, const Options& options)
{
    const auto w = Weigh (outline, programme, pins, options);
    std::vector<Candidate> out;
    for (size_t i = 0; i < w.plans.size (); ++i) {
        const auto& [shallow, whole] = w.plans[i];
        Options o = options;
        o.shallow = shallow;
        Candidate c;
        c.shallow = shallow;
        c.skeleton = detail::PlanFloor (outline, programme, pins, o, true);
        c.flats = static_cast<int> (whole.flats.size ());
        c.stairs = static_cast<int> (whole.cores.size ());
        c.net = whole.net;
        c.score = w.scores[i];
        c.chosen = i == w.chosen;
        out.push_back (std::move (c));
    }
    return out;
}
} // namespace geomsrv::archviz::floorscheme
