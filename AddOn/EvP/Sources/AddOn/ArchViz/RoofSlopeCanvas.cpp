#include "ArchViz/RoofSlopeCanvas.hpp"
#include "ArchViz/OverlayHud.hpp"
#include <imgui.h>
#include <algorithm>
#include <limits>

namespace geomsrv::archviz::roofcanvas {
Scope::Scope (Frame& frame, std::initializer_list<Anchor> anchors) : frame (frame)
{
    batch.first = frame.list->IdxBuffer.Size;
    batch.anchors = anchors;
}
Scope::~Scope ()
{
    batch.last = frame.list->IdxBuffer.Size;
    if (batch.last > batch.first)
        frame.batches.push_back (std::move (batch));
}
bool Bind (const Frame& frame, const ImDrawList* list, int index, overlayhud::Vertex& vertex)
{
    if (frame.list != list)
        return false;
    const auto found = std::upper_bound (frame.batches.begin (), frame.batches.end (), index,
                                         [] (int value, const Batch& batch) { return value < batch.last; });
    if (found == frame.batches.end () || index < found->first)
        return false;
    const Anchor* closest = nullptr;
    double distance = std::numeric_limits<double>::max ();
    for (const auto& at : found->anchors) {
        const double dx = vertex.x - at.screen[0], dy = vertex.y - at.screen[1];
        const double d = dx * dx + dy * dy;
        if (d < distance)
            closest = &at, distance = d;
    }
    if (!closest)
        return false;
    vertex.modelAnchored = true;
    vertex.model = closest->model;
    vertex.direction = closest->direction;
    vertex.turn = closest->turn;
    const float x = vertex.x - closest->screen[0], y = vertex.y - closest->screen[1];
    if (closest->turn) {
        const auto& u = closest->screenDirection;
        vertex.modelOffset = { x * u[0] + y * u[1], -x * u[1] + y * u[0] };
    }
    else
        vertex.modelOffset = { x, y };
    return true;
}
} // namespace geomsrv::archviz::roofcanvas
