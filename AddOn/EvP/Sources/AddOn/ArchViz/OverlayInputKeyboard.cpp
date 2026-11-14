#include "ArchViz/OverlayInputKeyboard.hpp"

namespace geomsrv::archviz::overlayinput::keyboard {
namespace {
HHOOK s_hook = nullptr;
Sink s_sink = nullptr;
bool s_ctrl = false, s_shift = false, s_alt = false;
// A key left to Archicad (`PassesToHost`) stays in the queue, so BOTH hooks see its message --
// in an order Windows does not promise, and a posted one reaches the message hook alone. The
// first to see it queues it for the HUD and notes it here; the other, seeing the same key next,
// lets it go by. Never the same hook twice: an auto-repeat is the same key again.
enum class Seen : uint8_t { None, ByKeyboardHook, ByMessageHook };
struct Passed {
    Seen by = Seen::None;
    uint32_t code = 0;
    LPARAM info = 0;
} s_passed;
// True when `by` is the first of the two hooks to see this key message.
bool FirstToSee (Seen by, uint32_t code, LPARAM info)
{
    if (s_passed.by != Seen::None && s_passed.by != by && s_passed.code == code && s_passed.info == info) {
        s_passed = Passed {};
        return false;
    }
    s_passed = { by, code, info };
    return true;
}
overlayhud::Input::Key Event (uint32_t code, LPARAM info)
{
    overlayhud::Input::Key key;
    key.code = code, key.down = (info & (1ll << 31)) == 0, key.repeat = (info & (1ll << 30)) != 0;
    key.ctrl = s_ctrl;
    key.shift = s_shift;
    key.alt = s_alt;
    if (code == VK_CONTROL || code == VK_LCONTROL || code == VK_RCONTROL)
        key.ctrl = s_ctrl = key.down;
    if (code == VK_SHIFT || code == VK_LSHIFT || code == VK_RSHIFT)
        key.shift = s_shift = key.down;
    if (code == VK_MENU || code == VK_LMENU || code == VK_RMENU)
        key.alt = s_alt = key.down;
    return key;
}
std::u16string Text (const overlayhud::Input::Key& key, LPARAM info)
{
    if (!key.down || key.ctrl || key.alt)
        return {};
    BYTE state[256] = {};
    ::GetKeyboardState (state);
    state[VK_SHIFT] = key.shift ? 0x80 : 0;
    state[VK_CONTROL] = key.ctrl ? 0x80 : 0;
    state[VK_MENU] = key.alt ? 0x80 : 0;
    state[key.code & 255u] |= 0x80;
    wchar_t out[8] = {};
    // Flag 4 does not change the OS dead-key state. Numeric roof fields need no
    // IME/dead-key composition, but must honour the keyboard's decimal layout.
    const int count = ::ToUnicodeEx (key.code, uint32_t (info >> 16) & 255u, state, out, 8, 4, ::GetKeyboardLayout (0));
    return count > 0 ? std::u16string (reinterpret_cast<const char16_t*> (out), size_t (count)) : std::u16string ();
}
LRESULT CALLBACK Hook (int code, WPARAM value, LPARAM info)
{
    if (code == HC_ACTION && s_sink) {
        const auto key = Event (uint32_t (value), info);
        if (PassesToHost (key) && !FirstToSee (Seen::ByKeyboardHook, key.code, info))
            return ::CallNextHookEx (s_hook, code, value, info);
        if (!(key.alt && key.code == VK_F4) && s_sink (::GetFocus (), key, Text (key, info), true))
            return 1;
    }
    return ::CallNextHookEx (s_hook, code, value, info);
}
} // namespace
bool Install (Sink sink)
{
    s_sink = sink;
    if (!s_hook) {
        s_ctrl = ::GetAsyncKeyState (VK_CONTROL) < 0;
        s_shift = ::GetAsyncKeyState (VK_SHIFT) < 0;
        s_alt = ::GetAsyncKeyState (VK_MENU) < 0;
        s_hook = ::SetWindowsHookExW (WH_KEYBOARD, &Hook, nullptr, ::GetCurrentThreadId ());
    }
    return s_hook != nullptr;
}
void Stop ()
{
    if (s_hook)
        ::UnhookWindowsHookEx (s_hook);
    s_hook = nullptr, s_sink = nullptr;
    s_ctrl = s_shift = s_alt = false;
    s_passed = Passed {};
}
bool Installed ()
{
    return s_hook != nullptr;
}
void Message (MSG& message, bool take)
{
    if (!s_sink)
        return;
    if (message.message == WM_KEYDOWN || message.message == WM_KEYUP || message.message == WM_SYSKEYDOWN ||
        message.message == WM_SYSKEYUP) {
        auto key = Event (uint32_t (message.wParam), message.lParam);
        key.down = message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN;
        if (take && PassesToHost (key) && !FirstToSee (Seen::ByMessageHook, key.code, message.lParam))
            return;
        if (!(key.alt && key.code == VK_F4) && s_sink (message.hwnd, key, Text (key, message.lParam), take))
            message.message = WM_NULL;
    }
    else if (message.message == WM_CHAR &&
             s_sink (message.hwnd, {}, std::u16string (1, char16_t (message.wParam)), take))
        message.message = WM_NULL;
}
} // namespace geomsrv::archviz::overlayinput::keyboard
