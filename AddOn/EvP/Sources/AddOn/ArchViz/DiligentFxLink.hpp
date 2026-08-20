#ifndef EVP_ARCHVIZ_DILIGENTFXLINK_HPP
#define EVP_ARCHVIZ_DILIGENTFXLINK_HPP

// ArchViz/DiligentFxLink — proof that DiligentFX is REACHABLE, not merely named
// in `target_link_libraries` (PLAT-RE127).
//
// WHY A PROOF AND NOT A FEATURE. `cmake/Diligent.cmake` has listed `DiligentFX`
// among the link libraries for some time and the library has been building, yet
// no translation unit in the add-on had ever included a DiligentFX header. A
// link line for a library nobody references proves nothing: the linker has no
// symbol to resolve, so the configuration would look identical whether the
// headers were reachable or not. The first real use would have discovered that
// at the worst moment -- in the middle of the G-Buffer work, where a build
// failure reads as a mistake in the new code.
//
// So this file does the smallest thing that CANNOT pass by accident: it includes
// DiligentFX's own headers and constructs one of its classes, which forces the
// compiler to resolve the include tree (including the .fxh shader headers that
// only the DiligentFX root include directory makes reachable) and forces the
// linker to resolve a real symbol out of DiligentFX.lib.
//
// It is the same reason the port has a probe per unknown rather than one big
// first integration, and the same shape: prove the mechanism in isolation, leave
// the probe in the tree as the regression fixture.
//
// ⚠️ THIS IS A COMPILE-AND-LINK PROOF, NOT A RUNTIME ONE. It says the headers
// resolve and the symbols link. It says NOTHING about whether a ShadowMapManager
// initialized against our device produces a correct cascade -- that needs a
// device, a frame, and a look, and belongs to the shadow work itself.

#include <string>

namespace geomsrv {
namespace archviz {

// True when DiligentFX compiled into this binary and its symbols linked. The
// string is for the log and the probe verb: it names what was reached.
bool DiligentFxLinked (std::string& report);

}   // namespace archviz
}   // namespace geomsrv

#endif
