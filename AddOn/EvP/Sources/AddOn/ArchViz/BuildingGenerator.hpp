#ifndef EVP_ARCHVIZ_BUILDINGGENERATOR_HPP
#define EVP_ARCHVIZ_BUILDINGGENERATOR_HPP

// The floor plan generator from any of three starting points, each giving one cell complex
// (BuildingTopology.hpp) planned by the same typology generator (FloorScheme.hpp):
//
//   mass   every level of a building: the stair stack planned once on the floor every level
//          has, then held exactly on each level (user, 2026-10-10: "stair core must be same
//          position from ground to top floor"); the flats may differ per level
//   floor  one floor outline: its own stairs, or a building's stack held
//   unit   one flat on its own: its rooms from what lies around it -- the corridor its door
//          opens onto, the flats beside it (Level::context); the rest of its outline is facade
//
// A part of a planned building is planned again the same way (`ReplanFloor`, `ReplanUnit`): a
// floor receives the building's stack, never decides it; a flat reads what lies around it off
// the complex. The complex is rebuilt from the schemes after every change, so the two never
// disagree. Pure C++/Clipper2: no ImGui, no Archicad; callers run it off the UI thread on copies.
#include "ArchViz/BuildingTopology.hpp"

namespace geomsrv::archviz::buildinggenerator {
using floorscheme::Ring;

// m2: less floor than this common to every level, and the stack is planned on the lead level.
constexpr double kMinCommon = 50.0;

// One level of what is planned.
struct Level {
    int story = 0;
    double z = 0, height = 3;
    std::vector<Ring> outline;                      // world rings: outer counter-clockwise, holes clockwise
    std::vector<Ring> party;                        // other buildings' floor at this elevation: blind walls
    std::vector<buildingtopology::Context> context; // around a unit planned on its own
    // This level's guidance. Its stairs are used only while the building has no stack: a level
    // receives the stack, it never carries stairs of its own.
    floorscheme::Pins pins;
};
struct Generated {
    std::vector<Level> levels;                             // lowest first
    std::vector<floorscheme::Scheme> schemes;              // one per level: complex floor i is level i
    std::vector<floorscheme::Pins::Core> stack;            // the building's stairs, held on every level
    buildingtopology::Complex complex;                     // from the schemes (`Rebuild`)
    std::vector<buildingtopology::Diagnostic> diagnostics; // the planning's own; the complex keeps its
    uint64_t nextId = 1; // the next new unit or cell id: ids outlive every rebuild (Reconcile)
};

// The floor every one of `levels` has, within `within` (the lead level's outline): where the
// stack's stairs are planned. `within` itself when less than `minArea` is common.
std::vector<Ring> Common (const std::vector<std::vector<Ring>>& levels, const std::vector<Ring>& within,
                          double minArea = kMinCommon);

// A whole mass, given as its levels in any order. The stack is planned on the levels' common floor
// with `stairs` (saved positions) as ordinary pins, as on the lead (largest) level; every level is
// then planned holding it.
Generated PlanMass (std::vector<Level> levels, const floorprogramme::Programme& programme,
                    const std::vector<floorscheme::Pins::Core>& stairs = {}, const floorscheme::Options& options = {});
// One floor outline, holding `stack` when given; without one its own stairs are its stack.
Generated PlanFloor (Level level, const floorprogramme::Programme& programme,
                     const std::vector<floorscheme::Pins::Core>& stack = {}, const floorscheme::Options& options = {});
// One flat of programme type `type`, outlined by the level's largest ring on wall centre lines.
// `axis`: along the facade its rooms should face, when known (a flat taken from a band); zero, the
// side with the most facade.
Generated PlanUnit (Level level, const floorprogramme::Programme& programme, size_t type,
                    floorscheme::Vec axis = { 0, 0 });

// Plans level `level` again (after its outline or pins changed), holding the building's stack.
// False when the level's scheme has an error.
bool ReplanFloor (Generated& generated, int level, const floorprogramme::Programme& programme,
                  const floorscheme::Options& options = {});
// Lays unit `unit`'s rooms again from what lies around it. False when it is not a rectangle with
// facade (its rooms are left to the user). Unit indices hold; cell and face indices do not.
bool ReplanUnit (Generated& generated, int unit);
// Unit `unit` becomes programme type `type`, and its rooms are laid for that type's room count.
bool RetypeUnit (Generated& generated, int unit, const floorprogramme::Programme& programme, size_t type);
// The complex again from the levels and schemes, after a caller changed a scheme itself. Units
// and cells found again keep their ids (buildingtopology::Reconcile).
void Rebuild (Generated& generated);
} // namespace geomsrv::archviz::buildinggenerator
#endif
