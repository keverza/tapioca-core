#ifndef EVP_ARCHVIZ_INPUTRINGBUFFER_HPP
#define EVP_ARCHVIZ_INPUTRINGBUFFER_HPP

// Native child HWND (main thread) -> render thread input.
//
// ⚠️ DG IS NOT THE SOURCE OF THE CURSOR POSITION ANY MORE, AND THAT IS THE WHOLE
// POINT OF THIS REVISION. `UserItemMouseMoved` arrives while HOVERING and STOPS
// for the duration of a drag: press at A, drag to B, release, and the camera
// sees no motion at all — then on release `UserItemMouseUp` pushes the position
// (B) and the button release as two separate operations, so a render thread that
// reads BETWEEN them gets "button held" with dx = B - A. One large jump, and
// whether it happens is a race. That is exactly the reported "clicks RANDOMLY
// rotate the cube", and it is also what threw the ImGui panel half off screen
// (plan §8.4).
//
// So the position, the Shift key and the NAVIGATION BUTTON are POLLED by the
// render thread (`GetCursorPos` + `ScreenToClient`, `GetAsyncKeyState`), and
// this class carries only discrete native-window events:
//
//   BUTTONS    left/right, LOSSLESS and in order, FOR ImGui. Dropping a mouse-up
//              strands a widget mid-drag (plan §4 risk 2), and a press+release
//              inside one 16 ms frame is an ordinary fast click that collapses to
//              "nothing happened" if only the final mask crosses.
//
//   WHEEL      ACCUMULATE. This is the sandbox's second bug (plan §1.1): the
//              window system delivers several notches per frame and the consumer
//              reads once per frame, so ASSIGNING discards all but the last and
//              the zoom feels slow and jumpy. Deltas add; positions replace.
//
// ⚠️ ONE SOURCE OF TRUTH PER QUANTITY. Do not add a DG path for the position,
// the modifier or the wheel button "as well" — a second source that disagrees
// for one frame is the bug above, back again and harder to see.
//
// ⚠️ THE POLLED FIELDS ARE FILLED IN BY THE RENDER THREAD AFTER `Take ()`, not
// by any producer here. They live in the same snapshot because the consumers
// (ImGui and the camera) want one object, not two.
//
// A mutex, not a lock-free ring. It is taken for a few nanoseconds in the child
// WndProc and once per frame on the render thread; a hand-rolled lock-free
// structure here would buy nothing measurable and cost a class of bug that is
// very hard to see. Revisit only with a measurement.

#include <cstdint>
#include <mutex>

namespace geomsrv {
namespace archviz {

// Bit flags, matching the IMGUI_MBUT_* values the bgfx imgui backend expects.
enum MouseButtonMask : uint8_t {
    kMouseNone = 0x00,
    kMouseLeft = 0x01,
    kMouseRight = 0x02,
    kMouseMiddle = 0x04,
};

struct ButtonTransition {
    uint8_t button = kMouseNone;
    bool down = false;
};

// What the render thread reads once per frame.
struct InputSnapshot {
    // ---- POLLED by the render thread; NOT pushed by window messages --------
    // Viewport-relative PHYSICAL pixels, straight out of ScreenToClient. ⚠️ The
    // HWND's client rect is already physical (measured: 1080x738 client for a
    // 720x492 logical item at 144 dpi), which is the same space as the
    // backbuffer — so the DPI multiply the DG path needed is GONE with it, not
    // moved. Do not reintroduce one.
    int32_t x = 0, y = 0;
    int32_t screenX = 0, screenY = 0;
    int32_t messageX = 0, messageY = 0;
    int32_t clientWidth = 0, clientHeight = 0;
    int32_t hostX = 0, hostY = 0, hostWidth = 0, hostHeight = 0;
    uint32_t dpi = 0;
    uint64_t monitor = 0;
    bool inside = false; // is the cursor within the viewport's client rect
    bool visible = false;
    bool hostMinimized = false;
    bool hostToolWindow = false;
    uint64_t hostStyle = 0;
    uint64_t hostExStyle = 0;
    bool shift = false; // Shift held RIGHT NOW; orbit vs pan reads this live
    // The wheel BUTTON — Archicad's navigation button. Polled, so it keeps
    // reporting for the whole drag and cannot be lost between two messages.
    bool navButton = false;

    // ---- pushed by the child HWND subclass/message hook -------------------
    uint8_t buttons = kMouseNone; // left/right held, for ImGui
    int32_t wheelDelta = 0;       // accumulated since the last snapshot, then cleared
    // QPC timestamps keep the sub-frame intervals measurable. `pointerQpc` is
    // when GetCursorPos/ScreenToClient completed on the render thread;
    // `eventQpc` is when the child WndProc received the newest button/wheel message.
    uint64_t pointerSequence = 0;
    uint64_t pointerQpc = 0;
    uint64_t pointerMessageSequence = 0;
    uint64_t pointerMessageQpc = 0;
    uint64_t eventSequence = 0;
    uint64_t eventQpc = 0;

    // ⚠️ THE TRANSITIONS, NOT JUST THE FINAL STATE. A press and release inside
    // ONE frame — 16 ms, which is an ordinary fast click — collapses to "nothing
    // happened" if only `buttons` is carried across. ImGui takes an event per
    // transition, so they are delivered as events; that is what makes a checkbox
    // in the viewport actually tick.
    static constexpr int kMaxTransitions = 16;
    ButtonTransition transitions[kMaxTransitions] = {};
    int transitionCount = 0;
};

class InputRingBuffer final {
  public:
    static InputRingBuffer& Get ();

    // ---- producer: native child HWND, main thread ----
    // ⚠️ LEFT AND RIGHT ONLY. The wheel button is polled (see the header note);
    // pushing it here as well would give the camera two disagreeing sources.
    void PushButton (uint8_t button, bool down);
    void PushWheel (int32_t delta);
    void PushPointerMessage (int32_t x, int32_t y);
    // The viewer is closing, or the child lost the mouse in a way Windows did not
    // report. Clears held buttons so nothing is left mid-drag.
    void Reset ();

    // ---- consumer: the render thread, once per frame ----
    // Drains the wheel accumulator and the transition list; the held-button state
    // persists, because it is STATE and not an event. The polled fields come back
    // zeroed — the caller fills them.
    InputSnapshot Take ();

  private:
    InputRingBuffer () = default;
    InputRingBuffer (const InputRingBuffer&) = delete;
    InputRingBuffer& operator= (const InputRingBuffer&) = delete;

    mutable std::mutex mutex_;
    InputSnapshot state_;
};

} // namespace archviz
} // namespace geomsrv

#endif
