#ifndef EVP_ARCHVIZ_DIAGRAMVIEW_HPP
#define EVP_ARCHVIZ_DIAGRAMVIEW_HPP
// What one exploded-diagram frame shows (#201): the publication and how to present it.
//
// The live viewer fills it from DiagramModel each frame; RenderBench fills it from a replayed
// capture, so a style or spacing can be checked without Archicad. The scene's DrawDiagram
// reads nothing else -- no model, no ACAPI, no HUD.
#include "ArchViz/DiagramPublication.hpp"
#include <memory>

namespace geomsrv::archviz::diagram {
// How a storey is coloured: by its massing function, or one plain white study-model tone.
enum class Colouring : uint8_t { Function, White };
// How much of the building a storey shows. Topology (the storey shells) is drawn today; the
// others need the floor plans in the publication (M2) and fall back to Topology until then.
enum class Detail : uint8_t { Topology, Units, Rooms, Walls };
constexpr const char* DetailName (Detail detail)
{
    switch (detail) {
        case Detail::Topology:
            return "Topology";
        case Detail::Units:
            return "Units";
        case Detail::Rooms:
            return "Rooms";
        case Detail::Walls:
            return "Walls";
    }
    return "Topology";
}
// Detail levels the publication can draw now.
constexpr bool DetailAvailable (Detail detail)
{
    return detail == Detail::Topology;
}

struct DiagramView {
    std::shared_ptr<const Publication> publication; // null: nothing to draw
    Spacing spacing;                                // the target; the frame eases toward it
    Colouring colouring = Colouring::Function;
    Detail detail = Detail::Topology;
    EntityId selected;
    bool animate = true; // false: show `spacing` at once (offline pictures)
};
} // namespace geomsrv::archviz::diagram
#endif
