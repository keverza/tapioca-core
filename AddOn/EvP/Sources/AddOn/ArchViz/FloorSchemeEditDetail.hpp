#ifndef EVP_ARCHVIZ_FLOORSCHEMEEDITDETAIL_HPP
#define EVP_ARCHVIZ_FLOORSCHEMEEDITDETAIL_HPP

// Shared by the floor edit's translation units (FloorSchemeEdit.cpp: the edits; FloorSchemeEditCount.cpp:
// the flat count): a band's flats frozen as the user sees them, and the helpers every edit uses.
// Private to them: nothing outside floorscheme::edit includes it.
#include "ArchViz/FloorSchemeDetail.hpp"
#include "ArchViz/FloorSchemeEdit.hpp"
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace geomsrv::archviz::floorscheme::edit::shared {
// A flat no narrower than the smallest flat type's rooms (a 1.5R: living room and alcove).
extern const double kNarrowest;
// A door's width: every flat reaches its band's circulation by at least this much.
inline constexpr double kDoor = 1.3;

// A band's flats as the user sees them, along the band's centre line: where each flat starts and
// ends, its room count (0: the generator's choice), and the band's pins written back from them.
struct Frozen {
    int band = -1;
    Vec origin, dir;                   // the centre line: origin + dir * u
    std::vector<double> lo, hi, rooms; // per flat, in order along dir
    std::vector<int> flats;            // scheme indices, in the same order
    Vec At (double u) const
    {
        return { origin.x + dir.x * u, origin.y + dir.y * u };
    }
    double U (Vec p) const
    {
        return detail::Dot ({ p.x - origin.x, p.y - origin.y }, dir);
    }
};
std::optional<Frozen> Freeze (const Scheme& s, int band);
// The band's pins from a frozen band: a wall at every boundary, one flat per span, its rooms.
Design Write (const Scheme& s, Design d, const Frozen& f);
// A frozen band's spans laid out again from its first wall, `widths` in order.
void Relay (Frozen& f, const std::vector<double>& widths);
size_t Index (const Frozen& f, int flat);
// Where a frozen band meets its circulation (corridor, stair, lobby), along it; lo > hi when it
// meets none.
std::pair<double, double> Reach (const Scheme& s, const Frozen& f);
// Where a wall may split span `k` of a frozen band: both parts at least `least` wide and each
// reaching the band's circulation by a door's width. Empty (lo > hi) when the span cannot split.
std::pair<double, double> SplitRange (const Scheme& s, const Frozen& f, size_t k, double least);

// `d` unchanged, and why it could not change in `why` (printf style).
Design Refuse (Design d, std::string* why, const char* format, ...);
// An edit made, with a word on it in `why` (printf style).
void Remark (std::string* why, const char* format, ...);
bool In (const Ring& ring, Vec p);
double Distance (Vec p, Vec a, Vec b);
// A point well inside `r`: of a grid over its bounds, the one farthest from its outline.
Vec Within (const Ring& r);
// The unlocked flat that shares the longest wall with `flat`, of its own section first, and not one
// of `taken`; -1 when none does.
int Beside (const Scheme& s, const Design& d, int flat, const std::vector<int>& taken);
} // namespace geomsrv::archviz::floorscheme::edit::shared
#endif
