#ifndef EVP_NATIVECOMMANDS_LIBRARYPREVIEWCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_LIBRARYPREVIEWCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// A library part's PREVIEW PICTURE: what format it is in, and the bytes
// themselves for a client that cannot reach the library directly.
//
// Split out of LibraryObjectCommands when the graph editor's picker needed the
// bytes on the wire and that file crossed its size ceiling. The seam is real
// rather than arithmetic: enumerating and PLACING a GDL object is a different
// job from transporting a picture of one, and only this half has to care that
// NewDisplay::NativeImage and a WebView2 decode different sets of formats.
NativeCommandRegistrations GetLibraryPreviewCommandRegistrations ();

} // namespace geomsrv

#endif
