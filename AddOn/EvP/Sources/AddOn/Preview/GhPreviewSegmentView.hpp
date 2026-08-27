#ifndef EVP_PREVIEW_GHPREVIEWSEGMENTVIEW_HPP
#define EVP_PREVIEW_GHPREVIEWSEGMENTVIEW_HPP

// The Win32 half of the preview segment: the batch's shared memory, mapped into
// Archicad READ-ONLY for as long as one batch is being copied out of it.
//
// This is the only file on the preview path that names a Win32 type, and it is
// deliberately the smallest thing that can be true: open by name, map, report a
// size, close. Every rule about WHEN it is opened, what may be read out of it
// and what happens when it cannot be opened lives in GhPreviewIngest, which is
// offline-tested against a buffer instead of against a kernel object.
//
// ⚠️ READ-ONLY, AND THAT IS NOT A PRECAUTION -- IT IS THE TRUST BOUNDARY. The
// producer is a separate process running third-party Grasshopper components.
// FILE_MAP_READ means a bug on this side cannot corrupt the worker's memory, and
// the copy-out in GhPreviewIngest means a malicious or broken worker cannot
// rewrite bytes the render thread is already reading.
//
// ⚠️ THE MAPPED SIZE IS VERIFIED, NOT ASSUMED. A batch declares how many bytes
// its segment holds and the primitive headers index into it; a section object
// that is SHORTER than declared would turn every one of those validated offsets
// into an over-read, because they were checked against the declaration rather
// than against the memory. VirtualQuery is what closes that gap.

#include "Preview/GhPreviewIngest.hpp"

#include <cstdint>
#include <string>

namespace evp::preview {

class GhPreviewSegmentView final : public GhPreviewSegmentSource {
  public:
    GhPreviewSegmentView () = default;
    ~GhPreviewSegmentView () override;

    GhPreviewSegmentView (const GhPreviewSegmentView&) = delete;
    GhPreviewSegmentView& operator= (const GhPreviewSegmentView&) = delete;

    bool Open (const std::string& name, uint32_t declaredBytes, std::string& error) override;
    const uint8_t* Data () const override;
    std::size_t Size () const override;
    void Close () override;

  private:
    void* mapping = nullptr; // HANDLE, kept opaque so <windows.h> stays out of this header
    const uint8_t* view = nullptr;
    std::size_t bytes = 0;
};

} // namespace evp::preview

#endif
