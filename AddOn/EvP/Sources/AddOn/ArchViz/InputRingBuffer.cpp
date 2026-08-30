#include "ArchViz/InputRingBuffer.hpp"

#include <windows.h>

namespace geomsrv {
namespace archviz {

InputRingBuffer& InputRingBuffer::Get ()
{
    static InputRingBuffer instance;
    return instance;
}

void InputRingBuffer::PushButton (uint8_t button, bool down)
{
    std::lock_guard<std::mutex> lock (mutex_);

    if (down)
        state_.buttons = uint8_t (state_.buttons | button);
    else
        state_.buttons = uint8_t (state_.buttons & ~button);
    ++state_.eventSequence;
    state_.eventTickMs = ::GetTickCount64 ();

    if (state_.transitionCount < InputSnapshot::kMaxTransitions) {
        state_.transitions[state_.transitionCount++] = { button, down };
    }
    else {
        // Full — which means the render thread has not run for 16 transitions,
        // i.e. it is stalled or gone. OVERWRITE THE LAST, never drop the first:
        // the oldest transitions are what got the held state to where it is, and
        // the final entry is what will be replayed last, so this keeps the
        // resulting state correct even though one intermediate click is lost.
        state_.transitions[InputSnapshot::kMaxTransitions - 1] = { button, down };
    }
}

void InputRingBuffer::PushWheel (int32_t delta)
{
    std::lock_guard<std::mutex> lock (mutex_);
    // ACCUMULATE. Assigning here is the sandbox's "camera feels slow and jumpy".
    state_.wheelDelta += delta;
    ++state_.eventSequence;
    state_.eventTickMs = ::GetTickCount64 ();
}

void InputRingBuffer::Reset ()
{
    std::lock_guard<std::mutex> lock (mutex_);
    state_ = InputSnapshot {};
}

InputSnapshot InputRingBuffer::Take ()
{
    std::lock_guard<std::mutex> lock (mutex_);
    const InputSnapshot out = state_;
    // Events are consumed; STATE is not. The held button mask survives the read
    // because it describes where things are, not what happened. The polled
    // fields are never written here at all — the render thread overwrites them
    // in the returned copy, from Win32, immediately after this call.
    state_.wheelDelta = 0;
    state_.transitionCount = 0;
    state_.eventTickMs = 0;
    return out;
}

} // namespace archviz
} // namespace geomsrv
