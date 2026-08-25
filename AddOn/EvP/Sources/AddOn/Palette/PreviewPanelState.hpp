#ifndef EVP_PALETTE_PREVIEWPANELSTATE_HPP
#define EVP_PALETTE_PREVIEWPANELSTATE_HPP

namespace evp::previewpanel {

enum class Host { None, Band, Overlay, PopOut };

struct HostState {
    Host current = Host::None;
    Host target = Host::None;
    bool transition = false;

    bool ExternalStartingOrActive () const;
    bool CanvasCollapsed () const;
};

// Pure state for the DG/Win32 input adapter. Coordinates stay out of this
// class: the render thread polls them from the canvas HWND in physical pixels.
class CanvasInputState {
  public:
    void SetAvailable (bool available);
    void SetPointerInside (bool inside);
    bool Press (unsigned buttonMask);
    bool Release (unsigned buttonMask);
    bool ReleaseAll ();

    bool CanRoutePointer () const;
    bool IsDragging () const;

  private:
    bool available = false;
    bool pointerInside = false;
    unsigned heldButtons = 0;
};

struct Rect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    int Width () const
    {
        return right - left;
    }
    int Height () const
    {
        return bottom - top;
    }
};

struct Layout {
    int height = 0;
    Rect canvas;
    Rect nodeSelector;
    Rect scrubber;
    Rect frameLabel;
    Rect enableControl;
    Rect overlayButton;
    Rect popOutButton;
    Rect returnButton;
    Rect hideButton;
    bool showCanvas = false;
    bool showPreviewControls = false;
};

Layout BuildLayout (int left, int right, int bottom, bool enabled, bool canvasCollapsed);

} // namespace evp::previewpanel

#endif
