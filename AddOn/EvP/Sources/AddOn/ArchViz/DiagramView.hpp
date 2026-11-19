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
// Detail (DiagramPlans.hpp): Units and Rooms draw the Massing workflow's planned floors;
// a storey without one keeps its shell. Walls are not drawn yet.
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
// Detail levels the diagram can draw now.
constexpr bool DetailAvailable (Detail detail)
{
    return detail != Detail::Walls;
}

struct DiagramView {
    std::shared_ptr<const Publication> publication; // null: nothing to draw
    Spacing spacing;                                // the target; the frame eases toward it
    Colouring colouring = Colouring::Function;
    Detail detail = Detail::Topology;
    EntityId selected;
    bool outlines = true;        // the shells' edges
    bool connectors = true;      // dashed corner lines from each storey down to the one below
    bool hiddenLines = false;    // edges behind a body: dashed and faint
    bool shellWireframe = false; // shells as edges only; around the units/rooms when those show
    bool animate = true;         // false: show `spacing` at once (offline pictures)
};
} // namespace geomsrv::archviz::diagram
#endif
