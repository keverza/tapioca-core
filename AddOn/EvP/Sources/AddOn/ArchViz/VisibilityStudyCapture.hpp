#ifndef EVP_ARCHVIZ_VISIBILITYSTUDYCAPTURE_HPP
#define EVP_ARCHVIZ_VISIBILITYSTUDYCAPTURE_HPP

#include "Geometry/MeshStore.hpp"

namespace geomsrv::archviz::visibilitystudy {

// Viewer work uses the completed shared pass, never a command's filtered current
// snapshot. CaptureStamp changes on edits even if snapshot identity does not.
inline bool CaptureIsFresh (const std::shared_ptr<const Snapshot>& snapshot, uint64_t admittedStamp)
{
    auto& store = MeshStore::Get ();
    return snapshot != nullptr && store.CaptureStamp () == admittedStamp &&
           (snapshot->captureStamp == admittedStamp || (snapshot->captureStamp == 0 && !store.CaptureActive ()));
}

inline std::shared_ptr<const Snapshot> ViewerCapture ()
{
    const auto snapshot = MeshStore::Get ().Shared ();
    return snapshot != nullptr && snapshot->completeModel && !snapshot->meshes.empty () &&
                   CaptureIsFresh (snapshot, MeshStore::Get ().CaptureStamp ())
               ? snapshot
               : nullptr;
}

} // namespace geomsrv::archviz::visibilitystudy
#endif
