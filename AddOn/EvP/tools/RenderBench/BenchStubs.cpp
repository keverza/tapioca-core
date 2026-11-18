// RenderBench/BenchStubs -- the add-on services DiligentScene asks every frame whose real source
// reaches the Archicad DevKit, answered as "nothing here".
//
// ⚠️ ONLY SERVICES WHOSE EMPTY ANSWER IS A REAL STATE OF THE VIEWER. The diagram model with no
// publication is exactly the viewer before Diagram Mode is ever opened, so the scene takes the same
// path it takes in Archicad on an ordinary project. A stub that changed what the renderer draws
// would make the bench test something the user never sees.

#include "ArchViz/DiagramModel.hpp"

namespace geomsrv::archviz::diagrammodel {

Controls ReadControls ()
{
    return {};
}
void SetControls (const Controls&)
{
}
diagram::DiagramView View ()
{
    return {};
}
std::shared_ptr<const diagram::Publication> Read ()
{
    return nullptr;
}
std::shared_ptr<const std::vector<std::string>> HiddenSource ()
{
    return nullptr;
}
diagram::EntityId Selected ()
{
    return {};
}
void Select (const diagram::EntityId&)
{
}
Status ReadStatus ()
{
    return {};
}
void RequestSave ()
{
}
void ReportDraw (uint64_t, size_t, size_t, double)
{
}
void Follow ()
{
}
void Forget ()
{
}

} // namespace geomsrv::archviz::diagrammodel
