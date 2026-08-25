#ifndef EVP_PREVIEW_RETAINEDTRACESELECTION_HPP
#define EVP_PREVIEW_RETAINEDTRACESELECTION_HPP

#include "Annotation/DrawList.hpp"
#include "Preview/RetainedPreviewStore.hpp"

namespace evp::preview {

geomsrv::annotation::DrawList ToDrawList (const WatchTrace& trace);

} // namespace evp::preview

#endif
