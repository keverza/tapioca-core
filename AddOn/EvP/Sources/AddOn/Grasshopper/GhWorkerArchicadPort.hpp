#ifndef EVP_GRASSHOPPER_GHWORKERARCHICADPORT_HPP
#define EVP_GRASSHOPPER_GHWORKERARCHICADPORT_HPP

#include "UniString.hpp"

#include <cstdint>

namespace evp {
namespace grasshopper {

// Read only on Archicad's main thread. GH1's external connection needs this
// instance's JSON port; GH2 obtains a pre-solve snapshot through the bridge.
uint32_t ReadArchicadJsonPort (GS::UniString& failure);

} // namespace grasshopper
} // namespace evp

#endif
