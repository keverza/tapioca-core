// Link stub for the host-occluder feed that SceneCmdQueue drives.
//
// ArchViz/Dxgi/HostOccluders.cpp is Direct3D 11 and can never join the offline
// build, while SceneCmdQueue.cpp -- which the offline gate DOES compile -- feeds
// it at every batch boundary. This file supplies only the calls SceneCmdQueue
// makes, as no-ops. AddVertices answers kNoBase, which is the production
// "no room" answer, so the queue stops feeding exactly as it would in Archicad
// when the occluder store is full: nothing here invents a behaviour of its own.
//
// ⚠️ ADD A FUNCTION HERE ONLY WHEN A LINKED SOURCE CALLS IT. A stub for the
// GPU half (Prepare, GetGeometry, GetStats) would let an offline test assert
// something about occlusion that only a live run can establish.

#include "ArchViz/Dxgi/HostOccluders.hpp"

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace hostocclusion {

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

} // namespace hostocclusion
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
