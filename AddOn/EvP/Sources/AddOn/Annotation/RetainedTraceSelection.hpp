#ifndef EVP_ANNOTATION_RETAINEDTRACESELECTION_HPP
#define EVP_ANNOTATION_RETAINEDTRACESELECTION_HPP

#include "Annotation/DrawList.hpp"

#include <cstddef>
#include <memory>
#include <optional>

namespace geomsrv::annotation {

struct RetainedFrameSnapshot {
    std::shared_ptr<const DrawList> drawList;
    std::size_t nodeIndex = 0;
    std::size_t frameIndex = 0;

    const Frame& SelectedFrame () const
    {
        return drawList->nodes[nodeIndex].frames[frameIndex];
    }
};

// Implemented by Preview's retained-store adapter. The converted draw list is
// immutable and cached by trace generation, so render hosts only copy a pointer.
std::optional<RetainedFrameSnapshot> SelectedRetainedFrameSnapshotCopy ();

} // namespace geomsrv::annotation

#endif
