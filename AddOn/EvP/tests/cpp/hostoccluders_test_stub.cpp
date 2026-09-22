#include "ArchViz/Dxgi/HostOccluders.hpp"

namespace geomsrv::archviz::dxgi::hostocclusion {

void BeginBatch (bool)
{
}

uint32_t AddVertices (const float*, uint32_t)
{
    return kNoBase;
}

void AddOpaqueIndices (uint32_t, const uint32_t*, uint32_t)
{
}

void AddTransparentIndices (uint32_t, const uint32_t*, uint32_t)
{
}

void NoteTransparent (uint32_t)
{
}

void EndBatch ()
{
}

} // namespace geomsrv::archviz::dxgi::hostocclusion
