#ifndef EVP_ARCHVIZ_OVERLAYINPUTKEYBOARD_HPP
#define EVP_ARCHVIZ_OVERLAYINPUTKEYBOARD_HPP
#include "ArchViz/OverlayHud.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace geomsrv::archviz::overlayinput::keyboard {
using Sink = bool (*) (HWND window, const overlayhud::Input::Key& key, const std::u16string& text, bool take);
// Thread-local and scoped by Sink. Runs before host accelerator translation,
// unlike a canvas-only WM_KEYDOWN rewrite. No focus steal, ACAPI or ImGui here.
bool Install (Sink sink);
void Stop ();
void Message (MSG& message, bool take);
} // namespace geomsrv::archviz::overlayinput::keyboard
#endif
