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

  private:
    PreviewRuntimeState () = default;

    std::atomic<bool> enabled { true };
};

} // namespace evp::preview

#endif
