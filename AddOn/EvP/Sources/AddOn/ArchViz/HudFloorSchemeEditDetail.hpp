#ifndef EVP_ARCHVIZ_HUDFLOORSCHEMEEDITDETAIL_HPP
#define EVP_ARCHVIZ_HUDFLOORSCHEMEEDITDETAIL_HPP

// Shared by the Plan view's two translation units: HudFloorSchemeEdit.cpp (designs, undo, the
// right-click menu, the tools) and HudFloorSchemeCanvas.cpp (the canvas: what is under the
// pointer, the selection's arrows, gestures, drawing, the palette). MAIN THREAD, under ImGui's lock.
#include "ArchViz/HudFloorSchemeEdit.hpp"
#include "imgui.h"
#include <climits>

namespace geomsrv::archviz::hudfloorscheme {
namespace bp = buildingplan;
namespace fs = floorscheme;
namespace fe = floorscheme::edit;

enum class Kind : uint8_t { None, Flat, Stair, Wall, End, Party };
// What is under the pointer, selected, or held: indices into the scheme it was found in.
struct Target {
    Kind kind = Kind::None;
    int index = -1; // a flat, a stair, a party wall
    std::optional<fe::Wall> wall;
    std::optional<fe::End> end;
};
enum class Tool : uint8_t { Select, Draw, Cut };
enum class Drag : uint8_t { None, Body, Arrow, Rect };
// One building's design as undo keeps it.
struct Snapshot {
    fe::Designs designs;
    std::vector<bp::Core> cores;
    bool operator== (const Snapshot&) const = default;
};
struct Step {
    std::map<std::string, Snapshot> before, after;
};
// World triangles of one filled shape, built once per scheme.
struct Fill {
    std::vector<fs::Vec> triangles;
    ImU32 colour = 0;
};
struct Arrow {
    fs::Vec at, dir; // world: where it sits and where it points
};
struct Editor {
    std::string key;
    int story = INT_MIN;
    bp::FloorInput input; // the shown floor's, as last asked
    fs::Scheme scheme;    // the newest planned
    uint64_t revision = 0;
    bool have = false;
    std::vector<Fill> fills, around;
    std::vector<fs::Ring> real; // the slab's own contour, dashed
    Tool tool = Tool::Select;
    Target selected, menu;
    fs::Vec selectedAt, menuAt; // where they were picked: a new scheme finds them there again
    // A gesture: the scheme and design it started from (indices stay valid), where, what it holds.
    Drag drag = Drag::None;
    Target held;
    fs::Scheme base;
    fe::Design baseDesign;
    fs::Vec from, at, axis;
    bool moved = false;
    ImGuiContext* owner = nullptr;
    uint64_t liveSince = 0; // planner revision when the gesture began: newer live schemes are its
    std::vector<Step> undo, redo;
};
struct Context {
    bp::Planner& planner;
    const std::map<std::string, bp::Plan>& plans;
    std::map<std::string, bp::Draft>& drafts;
    const floorprogramme::Programme& programme;
    const fs::Options& options;
};
namespace detail {
bool Inside (const std::vector<fs::Ring>& rings, fs::Vec p);
fs::Vec Mid (const fs::Ring& r);
fs::Vec Outward (const std::array<fs::Vec, 2>& wall); // out of the building, across a party wall
Fill Triangles (const std::vector<fs::Ring>& rings, ImU32 colour);
std::vector<Fill> Fills (const fs::Scheme& s);
// The selection found again in a new scheme, where it was picked.
Target Again (const fs::Scheme& s, const Target& t, fs::Vec at);
void Palette (float scale);
void Canvas (Editor& e, Context& c, float scale);
// HudFloorSchemeEdit.cpp: the shown floor's design becomes `next` (one undo step); a party wall
// pushed `distance` out of the building, or pulled in.
void Commit (Editor& e, Context& c, fe::Design next);
void PushParty (Editor& e, Context& c, const std::array<fs::Vec, 2>& wall, double distance);
} // namespace detail
} // namespace geomsrv::archviz::hudfloorscheme
#endif
