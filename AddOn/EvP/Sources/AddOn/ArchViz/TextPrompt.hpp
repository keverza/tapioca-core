#ifndef EVP_ARCHVIZ_TEXTPROMPT_HPP
#define EVP_ARCHVIZ_TEXTPROMPT_HPP

// ArchViz/TextPrompt -- a line of text asked for in a small native dialog ('GDLG' 32580): the
// one value of the HUD's metadata editor ImGui does not edit (HudMetadata.hpp). The HUD is
// drawn into Archicad's own view and has no text input of its own worth the name; a modal
// dialog has the system's -- the IME, the clipboard, the undo of typing.
//
// ⚠️ MAIN THREAD, AND NEVER INSIDE AN UNDO SCOPE OR AN IMGUI FRAME: the dialog runs its own
// message loop. The metadata's writer asks first, then opens its undo step with the answer
// (SelectionMetadata.cpp).

#include <string>

namespace geomsrv {
namespace archviz {
namespace textprompt {

// `prompt` over a field holding `text`; true with `answer` when the user pressed OK.
bool Ask (const std::string& prompt, const std::string& text, std::string& answer);

} // namespace textprompt
} // namespace archviz
} // namespace geomsrv

#endif
