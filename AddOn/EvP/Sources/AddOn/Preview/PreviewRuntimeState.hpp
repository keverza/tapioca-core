#ifndef EVP_PREVIEW_PREVIEWRUNTIMESTATE_HPP
#define EVP_PREVIEW_PREVIEWRUNTIMESTATE_HPP

#include <atomic>

namespace evp::preview {

class PreviewRuntimeState {
  public:
    static PreviewRuntimeState& Get ();

    bool IsEnabled () const
    {
        return enabled.load (std::memory_order_acquire);
    }

    void SetEnabled (bool value)
    {
        enabled.store (value, std::memory_order_release);
    }

    // ⚠️ A WATCH TRACE HAS A SECOND READER: the overlays in Archicad's own views draw
    // its selected frame (ArchViz/OverlayAnnotations.hpp). The palette's Preview
    // switch is about the palette's preview; with it off, SetWatchTrace refused every
    // trace and the overlays -- switched on and waiting -- never had a frame to draw
    // (the first live run, 2026-09-29). A trace is taken when EITHER wants it.
    bool AcceptsWatchTraces () const
    {
        return IsEnabled () || overlayWatch.load (std::memory_order_acquire);
    }

    void SetOverlayWatch (bool value)
    {
        overlayWatch.store (value, std::memory_order_release);
    }

  private:
    PreviewRuntimeState () = default;

    std::atomic<bool> enabled { true };
    std::atomic<bool> overlayWatch { false };
};

} // namespace evp::preview

#endif
