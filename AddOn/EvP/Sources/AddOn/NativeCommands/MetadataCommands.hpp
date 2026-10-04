#ifndef EVP_NATIVECOMMANDS_METADATACOMMANDS_HPP
#define EVP_NATIVECOMMANDS_METADATACOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// Tapioca's own metadata (Metadata/TapiocaMetadata.hpp) for scripts: an element's, read and
// written on the element (GetElementMetadata, SetElementMetadata), and the project's schema
// (GetMetadataSchema, SetMetadataSchema). Metadata and schemas travel as JSON text, the
// format the model reads and writes.
NativeCommandRegistrations GetMetadataCommandRegistrations ();

} // namespace geomsrv

#endif
