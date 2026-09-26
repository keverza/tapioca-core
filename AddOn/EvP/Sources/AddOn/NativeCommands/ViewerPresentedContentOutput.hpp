#ifndef EVP_NATIVECOMMANDS_VIEWERPRESENTEDCONTENTOUTPUT_HPP
#define EVP_NATIVECOMMANDS_VIEWERPRESENTEDCONTENTOUTPUT_HPP

// The `presentedContent` object of Tapioca.ViewerPassProvenance: which composited
// image each Present showed, read from pixels, against the overlay camera drawn on
// it, plus the frames kept as BMP files. Its own file for the reason
// ViewerCameraAgreementOutput has one: the verb's file sits at the size cap. The
// response schema stays with the command's registration.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ObjectState.hpp"

#include <cstdint>

namespace geomsrv {

// MAIN THREAD, after the PassProvenance drain. Writes any kept frame not yet
// written, named for `epoch`, and reports its path.
GS::ObjectState BuildPresentedContentOutput (uint64_t epoch);

} // namespace geomsrv

#endif
