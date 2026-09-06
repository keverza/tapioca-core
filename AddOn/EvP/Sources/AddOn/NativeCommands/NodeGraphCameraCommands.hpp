#ifndef EVP_NATIVECOMMANDS_NODEGRAPHCAMERACOMMANDS_HPP
#define EVP_NATIVECOMMANDS_NODEGRAPHCAMERACOMMANDS_HPP

// The camera-set node's action verb. Split out from the selection commands
// rather than added to them: they share a shape and nothing else, and one file
// holding both would be the place a copied `update` case survives a rename.

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

NativeCommandRegistrations GetNodeGraphCameraCommandRegistrations ();

} // namespace geomsrv

#endif // EVP_NATIVECOMMANDS_NODEGRAPHCAMERACOMMANDS_HPP
