#include "ArchViz/FloorSchemeEdit.hpp"
#include "ArchViz/FloorSchemeDetail.hpp"

// The silhouette a floor's design plans, a building's share of a floor among its neighbours, and
// the session's undo (FloorSchemeEdit.hpp). The edits themselves are FloorSchemeEdit.cpp.
namespace geomsrv::archviz::floorscheme::edit {
namespace {
using namespace detail;
constexpr size_t kUndo = 200;

cp::PathsD Clean (const std::vector<Ring>& rings)
{
    return cp::Union (ToPaths (rings), cp::FillRule::NonZero, kPrecision);
}
} // namespace

std::vector<Ring> Outline (const std::vector<Ring>& floor, const Design& design)
{
    cp::PathsD all = ToPaths (floor);
    for (const auto& r : design.added)
        all.push_back (ToPath (Counter (r)));
    auto joined = cp::Union (all, cp::FillRule::NonZero, kPrecision);
    if (!design.cut.empty ()) {
        cp::PathsD cut;
        for (const auto& r : design.cut)
            cut.push_back (ToPath (Counter (r)));
        joined = cp::Difference (joined, cut, cp::FillRule::NonZero, kPrecision);
    }
    return FromPaths (joined);
}

std::vector<std::vector<Ring>> Owned (const std::vector<std::vector<Ring>>& floors,
                                      const std::vector<const Design*>& designs)
{
    const size_t n = floors.size ();
    std::vector<cp::PathsD> base (n), added (n);
    cp::PathsD before;
    for (size_t i = 0; i < n; ++i) {
        const auto floor = Clean (floors[i]);
        base[i] = before.empty () ? floor : cp::Difference (floor, before, cp::FillRule::NonZero, kPrecision);
        before = cp::Union (before, floor, cp::FillRule::NonZero, kPrecision);
        if (i < designs.size () && designs[i])
            added[i] = Clean (designs[i]->added);
    }
    std::vector<std::vector<Ring>> out (n);
    for (size_t i = 0; i < n; ++i) {
        auto own = added[i].empty () ? base[i] : cp::Union (base[i], added[i], cp::FillRule::NonZero, kPrecision);
        if (i < designs.size () && designs[i] && !designs[i]->cut.empty ())
            own = cp::Difference (own, Clean (designs[i]->cut), cp::FillRule::NonZero, kPrecision);
        cp::PathsD others;
        for (size_t j = 0; j < n; ++j)
            if (j != i)
                others.insert (others.end (), added[j].begin (), added[j].end ());
        if (!others.empty ())
            own = cp::Difference (own, cp::Union (others, cp::FillRule::NonZero, kPrecision), cp::FillRule::NonZero,
                                  kPrecision);
        out[i] = FromPaths (own);
    }
    return out;
}
Scheme RunOwned (const std::vector<Ring>& owned, const std::vector<Ring>& party,
                 const floorprogramme::Programme& programme, const Design& design, const Options& options)
{
    Options o = options;
    o.shallow = design.shallow;
    o.party = party;
    return Generate (owned, programme, design.pins, o);
}
std::vector<Ring> PartyStrip (const std::array<Vec, 2>& wall, double distance, const std::vector<Ring>& from)
{
    if (std::abs (distance) < 0.05 || from.empty ())
        return {};
    const Vec a = wall[0], b = wall[1];
    const Vec d = Unit ({ b.x - a.x, b.y - a.y }), n { d.y, -d.x }; // outward: right of a counter-clockwise outline
    const Ring strip = Counter (
        { a, b, { b.x + n.x * distance, b.y + n.y * distance }, { a.x + n.x * distance, a.y + n.y * distance } });
    std::vector<Ring> out;
    for (auto& r : FromPaths (cp::Intersect ({ ToPath (strip) }, Clean (from), cp::FillRule::NonZero, kPrecision)))
        if (std::abs (Area (r)) > 0.05)
            out.push_back (std::move (r));
    return out;
}
void Take (Design& gainer, Design& loser, const std::vector<Ring>& strip)
{
    if (strip.empty ())
        return;
    auto added = ToPaths (gainer.added);
    for (const auto& r : strip)
        added.push_back (ToPath (Counter (r)));
    gainer.added = FromPaths (cp::Union (added, cp::FillRule::NonZero, kPrecision));
    const auto paths = Clean (strip);
    if (!gainer.cut.empty ())
        gainer.cut = FromPaths (cp::Difference (Clean (gainer.cut), paths, cp::FillRule::NonZero, kPrecision));
    if (!loser.added.empty ())
        loser.added = FromPaths (cp::Difference (Clean (loser.added), paths, cp::FillRule::NonZero, kPrecision));
}

Scheme Run (const std::vector<Ring>& floor, const floorprogramme::Programme& programme, const Design& design,
            const Options& options)
{
    Options o = options;
    o.shallow = design.shallow;
    return Generate (Outline (floor, design), programme, design.pins, o);
}

Access Chosen (const Scheme& scheme, const Options& options)
{
    Access chosen = Access::Auto;
    for (const auto& w : scheme.wings)
        if (w.depth < options.centreDepth - 1e-6 && (w.access == Access::OneSide || w.access == Access::CoreOnly))
            chosen = chosen == Access::Auto || chosen == w.access ? w.access : chosen;
    return chosen;
}

bool Session::Commit (Design next)
{
    if (next == design)
        return false;
    undo.push_back (std::move (design));
    if (undo.size () > kUndo)
        undo.erase (undo.begin ());
    design = std::move (next);
    redo.clear ();
    return true;
}
bool Session::Undo ()
{
    if (undo.empty ())
        return false;
    redo.push_back (std::move (design));
    design = std::move (undo.back ());
    undo.pop_back ();
    return true;
}
bool Session::Redo ()
{
    if (redo.empty ())
        return false;
    undo.push_back (std::move (design));
    design = std::move (redo.back ());
    redo.pop_back ();
    return true;
}
void Session::Reset ()
{
    Commit ({});
}
} // namespace geomsrv::archviz::floorscheme::edit
