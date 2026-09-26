#ifndef EVP_NATIVECOMMANDS_VIEWERCAMERAAGREEMENTOUTPUT_HPP
#define EVP_NATIVECOMMANDS_VIEWERCAMERAAGREEMENTOUTPUT_HPP

// The `cameraAgreement` object of Tapioca.ViewerPassProvenance: the per-draw camera
// pose table and the raw windows of a few consecutive images. Split out of
// ViewerPassProvenanceCommands.cpp when Stage 74 pushed that file past the size
// cap; the response schema stays with the command's registration.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ObjectState.hpp"

namespace geomsrv {

// MAIN THREAD, after the PassProvenance drain -- the same boundary as the rest of
// the verb's response.
GS::ObjectState BuildCameraAgreementOutput ();

} // namespace geomsrv

#endif
