#include "ArchViz/FloorSchemeDetail.hpp"

// The two stages a floor is planned to. `Analyse` stops at the skeleton (T0-T4): wings, access,
// corridors, stairs and bands, to look at and change before any flat is made. `Generate` goes on
// to the flats (T5-T7) and, where a thin wing can be planned two ways, keeps the better floor.
namespace geomsrv::archviz::floorscheme {
namespace {
// Net area less what the stairs cost and the floor left unassigned; a broken scheme loses.
double Economy (const Scheme& s, const Options& o)
{
    double red = 0;
    for (const auto& u : s.unassigned)
        if (u.reason.find ("storage") == std::string::npos)
            red += std::abs (Area (u.shape));
    return s.net - o.stairCost * static_cast<double> (s.cores.size ()) - red - (s.ok ? 0.0 : 1e6);
}
} // namespace

Scheme Analyse (const std::vector<Ring>& outline, const floorprogramme::Programme& programme, const Pins& pins,
                const Options& options)
{
    return detail::PlanFloor (outline, programme, pins, options, true);
}

Scheme Generate (const std::vector<Ring>& outline, const floorprogramme::Programme& programme, const Pins& pins,
                 const Options& options)
{
    auto s = detail::PlanFloor (outline, programme, pins, options);
    if (options.shallow != Access::Auto)
        return s;
    // Typology adaptation (user, 2026-10-10): shallow wings planned as sections are weighed
    // against a corridor on one side, the economic plan of a thin bar.
    bool sections = false;
    for (const auto& w : s.wings)
        sections = sections || w.access == Access::CoreOnly;
    if (!sections)
        return s;
    Options corridor = options;
    corridor.shallow = Access::OneSide;
    auto alt = detail::PlanFloor (outline, programme, pins, corridor);
    const double a = Economy (s, options), b = Economy (alt, options);
    auto& kept = b > a ? alt : s;
    kept.diagnostics.push_back ({ Diagnostic::Info,
                                  "typology.choice",
                                  std::string (b > a ? "A corridor on one side" : "Sections round stairs") +
                                      " scored " + std::to_string (static_cast<int> (std::round ((std::max) (a, b)))) +
                                      " against " + std::to_string (static_cast<int> (std::round ((std::min) (a, b)))) +
                                      " (net m2 less stairs and unassigned).",
                                  {} });
    return std::move (kept);
}
} // namespace geomsrv::archviz::floorscheme
