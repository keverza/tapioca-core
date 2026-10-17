#ifndef EVP_SURFACETAKEOFFOVERLAY_HPP
#define EVP_SURFACETAKEOFFOVERLAY_HPP
// OVERLAY-INVARIANTS.md: main-thread publication and capture-bound lifetime only.
#include "SurfaceTakeoffLayer.hpp"
namespace geomsrv::archviz::surfacetakeoff {
// Worker-safe intent generation; newer requests/Clear supersede late completions.
uint64_t BeginRequest ();
bool Publish (const Presentation& presentation, uint64_t snapshotId, uint64_t captureStamp, uint64_t request,
              std::string& error);
void Clear ();
void OnProjectClosed ();
} // namespace geomsrv::archviz::surfacetakeoff
#endif
