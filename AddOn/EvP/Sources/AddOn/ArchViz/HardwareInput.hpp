#ifndef EVP_ARCHVIZ_HARDWAREINPUT_HPP
#define EVP_ARCHVIZ_HARDWAREINPUT_HPP

// ArchViz/HardwareInput — the cursor position and the wheel button, polled.
//
// Lifted verbatim out of `RenderThread.cpp`'s anonymous namespace so the
// Diligent viewport can navigate with the SAME code rather than a second
// version of it. It is pure Win32: no bgfx, no Diligent, no DG, no DevKit, so
// it sits on the .apx side and both renderers link one copy.
//
// ⚠️ POLLED, NOT QUEUED, and that is the whole design. A latch driven by a
// queued down/up pair can be read between the two events and yields one huge
// delta; the polled state is continuous for the entire drag.
//
// ⚠️⚠️ BUT GetAsyncKeyState IS GLOBAL, AND THAT ONCE COST US ARCHICAD'S OWN 3D
// WINDOW. Measured 2026-08-06: orbiting Archicad's 3D window also drove the
// viewer's camera, because the middle button was being read from the whole
// desktop while the wheel came from DG and was already filtered to our item —
// one gesture, two cameras. `inside` is what prevents it, and A RECTANGLE TEST
// IS NOT ENOUGH: our viewport keeps its screen rect while another window sits
// on top of it. The test asks Windows which window is actually at that point
// and compares top-level ancestors. Camera only LATCHES a drag whose press
// satisfies it; a drag already latched keeps running wherever the cursor goes,
// which is what makes panning across the HUD work.
//
// ⚠️ PHYSICAL PIXELS. The position comes from the OS in the same space as the
// back buffer, so the DPI multiply the DG path needed is not moved here, it is
// GONE. Do not add one back.

#include "ArchViz/InputRingBuffer.hpp"

namespace geomsrv {
namespace archviz {

struct HardwarePointerPosition {
    int32_t x = 0;
    int32_t y = 0;
    bool inside = false;
};

// Reads canvas-local physical coordinates directly from the HWND. The palette
// uses the same measurement to gate hover/wheel routing that the renderer uses.
bool ReadHardwarePointer (void* nwh, HardwarePointerPosition& position);

// Embedded hosts disable polling on every explicit input-release path. Other
// viewer hosts have no gate and retain the existing hover behaviour.
void SetHardwareInputEnabled (void* nwh, bool enabled);
void ForgetHardwareInputWindow (void* nwh);

// Fills the polled fields of `io` (x, y, inside, shift, navButton) for the
// window `nwh`. The queued fields (wheel, button transitions) are DG's and are
// left alone -- call this immediately after InputRingBuffer::Take, before
// anything consumes the snapshot, or the consumer sees the cursor at 0,0.
void PollHardwareInput (void* nwh, InputSnapshot& io);

} // namespace archviz
} // namespace geomsrv

#endif
