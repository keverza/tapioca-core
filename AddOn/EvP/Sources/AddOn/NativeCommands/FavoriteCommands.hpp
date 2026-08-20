#ifndef EVP_NATIVECOMMANDS_FAVORITECOMMANDS_HPP
#define EVP_NATIVECOMMANDS_FAVORITECOMMANDS_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// One favourite, as ListFavorites reports it and as the palette's evp.Favourite
// browser shows it.
//
// ⚠️ THE NAME IS THE IDENTITY, and that is the API's decision, not a shortcut
// taken here: ACAPI_Favorite_Get, _Change, _Rename and _Delete all key on the
// name, and neither the legacy struct nor ACAPI::Favorite::Favorite exposes a
// guid or any other stable token. So a duplicate name is genuinely ambiguous —
// CollectFavorites reports the duplicates rather than silently keeping one.
struct FavoriteEntry {
    GS::UniString name;
    GS::UniString elementType;       // "Wall", "Object", … — see ElemTypeName
    GS::Array<GS::UniString> folder; // the Favourites folder path, root == empty
};

// Every favourite in the project, optionally narrowed to one element type by
// NAME ("Wall", "Object", …; empty means every type).
//
// MAIN THREAD ONLY — an ACAPI enumeration, shared with Palette's CatalogBrowser
// for the same reason CollectLibraryParts is.
GSErrCode CollectFavorites (const GS::UniString& elementTypeFilter, GS::Array<FavoriteEntry>& favorites);

// The name an element-type filter is spelled with. Returns "" for the toolbox
// entries that have no agreed spelling, which are then reachable only with no
// filter at all.
GS::UniString ElemTypeName (const API_ElemType& type);

// The project's Favourites: ListFavorites.
//
// Its own domain rather than a room in LibraryObjectCommands, because a
// favourite is not a library part — it is a saved element DEFAULT of any type,
// and the only thing the two share is that a palette picker browses both.
NativeCommandRegistrations GetFavoriteCommandRegistrations ();

} // namespace geomsrv

#endif
