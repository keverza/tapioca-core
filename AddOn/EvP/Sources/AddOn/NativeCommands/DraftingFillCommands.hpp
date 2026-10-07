#ifndef EVP_NATIVECOMMANDS_DRAFTINGFILLCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_DRAFTINGFILLCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

NativeCommandRegistrations GetDraftingFillCommandRegistrations ();
// Shared straight outer+hole polygon memo for fill creation and baked slab finalization.
bool BuildStraightPolygonMemo (const GS::Array<GS::ObjectState>& outline, const GS::Array<GS::ObjectState>& holes,
                               API_Polygon& polygon, API_ElementMemo& memo, GS::UniString& error);

} // namespace geomsrv

#endif
