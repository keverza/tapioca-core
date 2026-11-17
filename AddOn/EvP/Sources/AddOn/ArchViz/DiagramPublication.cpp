#include "ArchViz/DiagramPublication.hpp"
#include <chrono>

namespace geomsrv::archviz::diagram {
LayoutRequest DefaultRequest ()
{
    LayoutRequest request;
    request.rules = DefaultLayoutRules ();
    return request;
}

Prepared Prepare (const DiagramInputSnapshot& snapshot, StoreyBindings bindings, const LayoutRequest& request,
                  uint64_t generation)
{
    const auto started = std::chrono::steady_clock::now ();
    Prepared out;
    out.status = snapshot.status;
    BuiltScene built = BuildScene (snapshot, bindings);
    out.notes = built.notes;
    if (!built.scene)
        return out; // refused: the caller keeps its last coherent publication
    auto publication = std::make_shared<Publication> ();
    publication->generation = generation;
    publication->revisions = snapshot.revisions;
    publication->domain = snapshot.domain;
    publication->plan = Plan (*built.scene, request);
    publication->geometry = BuildGeometry (built);
    publication->representedGuids = built.representedGuids;
    publication->notes = built.scene->notes;
    publication->notes.insert (publication->notes.end (), publication->plan.notes.begin (),
                               publication->plan.notes.end ());
    for (const auto& entity : built.scene->entities) {
        publication->buildings += entity.id.level == Level::Building;
        publication->storeys += entity.id.level == Level::Storey;
    }
    publication->shells = std::move (built.shells);
    publication->scene = std::move (built.scene);
    publication->buildMs =
        std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count ();
    out.publication = std::move (publication);
    out.bindings = std::move (bindings);
    return out;
}
} // namespace geomsrv::archviz::diagram
