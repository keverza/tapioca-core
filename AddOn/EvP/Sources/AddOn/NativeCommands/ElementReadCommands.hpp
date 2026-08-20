#ifndef EVP_NATIVECOMMANDS_ELEMENTREADCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_ELEMENTREADCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// Element / library-part reads, all by guid or by part name: GetElementInfo,
// GetElementDetails, GetLibraryPartInfo, FindPlacedObjects.
//
// Two neighbours were split out as this file hit its size ceiling, both along seams
// that were already there: project-level reads taking no guid (GetStories,
// GetProjectInfo) plus the facade's ProjectInfoField live in ProjectCommands, and the
// named-attribute read (GetAttributeInfo, with its ProfileVectorImageOperations
// include) lives in AttributeCommands.
//
NativeCommandRegistrations GetElementReadCommandRegistrations ();

} // namespace geomsrv

#endif
