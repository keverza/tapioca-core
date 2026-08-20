#ifndef GEOMETRYSERVER_PALETTE_PARAMLAYOUT_HPP
#define GEOMETRYSERVER_PALETTE_PARAMLAYOUT_HPP

// F13.A — the generated parameter rows reserve a compact, predictable input
// column.  This is deliberately DevKit-free: changing a resize formula by eye
// is unreliable, so tests/cpp/test_param_layout.cpp asserts its boundaries.

namespace evp {

// Width of a normal generated input widget, excluding a FilePath's Browse
// button. `contentWidth` is the panel width between the usual left/right
// margins. At the 440 px default palette this is one third of the content;
// past that baseline only one tenth of additional width reaches the input.
int InputColumnWidth (int contentWidth);

}   // namespace evp

#endif
