#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/FavoriteCommands.hpp"
#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

namespace {

// ---------------------------------------------------------------------------
// Tapioca.ListFavorites { elementType?, nameFilter? }
//
// The project's saved element defaults. Nothing in the JSON API or in Tapir
// exposes favourites at all, so before this a command could neither discover
// nor name one — which is why the palette's evp.Favourite picker had to wait
// for a native command.
//
// A pure READ: no undo step, MainThreadCommand.
// ---------------------------------------------------------------------------
class ListFavoritesCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "ListFavorites";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString elementType, nameFilter;
        params.Get ("elementType", elementType);
        params.Get ("nameFilter", nameFilter);

        GS::Array<FavoriteEntry> favorites;
        const GSErrCode err = CollectFavorites (elementType, favorites);
        if (err != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL (
                "ACAPI_Toolbox_GetToolBoxInfo/ACAPI_Favorite_GetNum", err,
                GS::UniString::Printf ("enumerating the project's favourites (element-type filter \"%T\")",
                                       elementType.ToPrintf ())));
        }

        const GS::UniString wanted = nameFilter.ToLowerCase ();

        GS::Array<GS::ObjectState> rows;
        for (const FavoriteEntry& favorite : favorites) {
            if (!wanted.IsEmpty () && !favorite.name.ToLowerCase ().Contains (wanted))
                continue;
            GS::ObjectState row;
            row.Add ("name", favorite.name);
            row.Add ("elementType", favorite.elementType);
            row.Add ("folder", favorite.folder);
            rows.Push (row);
        }

        GS::ObjectState os;
        os.Add ("favorites", rows);
        os.Add ("total", (GS::Int32) rows.GetSize ());
        return os;
    }
};

const NativeCommandRegistration FavoriteCommandRegistrations[] = {
    { "ListFavorites", &MakeRegisteredNativeCommand<ListFavoritesCommand>, false,
      R"json({"type":"object","properties":{"elementType":{"type":"string"},"nameFilter":{"type":"string"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"favorites":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"elementType":{"type":"string"},"folder":{"type":"array","items":{"type":"string"}}},"additionalProperties":false,"required":["name","elementType","folder"]}},"total":{"type":"integer"}},"additionalProperties":false,"required":["favorites","total"]})json" },
};

} // namespace

// ---------------------------------------------------------------------------
GS::UniString ElemTypeName (const API_ElemType& type)
{
    switch (type.typeID) {
        case API_WallID:
            return "Wall";
        case API_ColumnID:
            return "Column";
        case API_BeamID:
            return "Beam";
        case API_WindowID:
            return "Window";
        case API_DoorID:
            return "Door";
        case API_ObjectID:
            return "Object";
        case API_LampID:
            return "Lamp";
        case API_SlabID:
            return "Slab";
        case API_RoofID:
            return "Roof";
        case API_MeshID:
            return "Mesh";
        case API_DimensionID:
            return "Dimension";
        case API_RadialDimensionID:
            return "RadialDimension";
        case API_LevelDimensionID:
            return "LevelDimension";
        case API_AngleDimensionID:
            return "AngleDimension";
        case API_TextID:
            return "Text";
        case API_LabelID:
            return "Label";
        case API_ZoneID:
            return "Zone";
        // ⚠️ The FILL tool's element is API_HatchID — there is no API_FillID.
        case API_HatchID:
            return "Fill";
        case API_LineID:
            return "Line";
        case API_PolyLineID:
            return "Polyline";
        case API_ArcID:
            return "Arc";
        case API_CircleID:
            return "Circle";
        case API_SplineID:
            return "Spline";
        case API_HotspotID:
            return "Hotspot";
        case API_CutPlaneID:
            return "Section";
        case API_CameraID:
            return "Camera";
        case API_DrawingID:
            return "Drawing";
        case API_PictureID:
            return "Picture";
        case API_DetailID:
            return "Detail";
        case API_ElevationID:
            return "Elevation";
        case API_InteriorElevationID:
            return "InteriorElevation";
        case API_WorksheetID:
            return "Worksheet";
        case API_CurtainWallID:
            return "CurtainWall";
        case API_ShellID:
            return "Shell";
        case API_SkylightID:
            return "Skylight";
        case API_MorphID:
            return "Morph";
        case API_ChangeMarkerID:
            return "ChangeMarker";
        case API_StairID:
            return "Stair";
        case API_RailingID:
            return "Railing";
        case API_BeamSegmentID:
            return "BeamSegment";
        case API_ColumnSegmentID:
            return "ColumnSegment";
        case API_OpeningID:
            return "Opening";
        default:
            return GS::UniString ();
    }
}

GSErrCode CollectFavorites (const GS::UniString& elementTypeFilter, GS::Array<FavoriteEntry>& favorites)
{
    favorites.Clear ();

    // ⚠️ THERE IS NO "GIVE ME EVERY FAVOURITE" IN THE LEGACY API.
    // ACAPI_Favorite_GetNum answers for ONE API_ElemType, and an API_ElemType is
    // typeID + variationID — a favourite saved against a GDL subtype lives under
    // that subtype's variation, not under the generic one. Iterating the TOOLBOX
    // is what enumerates the real (type, variation) pairs, and it is what the
    // DevKit's own Favorite_Test does for exactly this reason. Asking for
    // API_ObjectID alone would quietly miss every subtyped object favourite.
    //
    // enableHidden=true: a favourite may be saved against a tool the current
    // Archicad configuration does not show, and it still exists.
    API_ToolBoxInfo toolbox = {};
    const GSErrCode toolboxErr = ACAPI_Toolbox_GetToolBoxInfo (&toolbox, true);
    if (toolboxErr != NoError)
        return toolboxErr;

    const GS::UniString wantedType = elementTypeFilter.ToLowerCase ();

    GSErrCode result = NoError;
    for (Int32 i = 0; i < toolbox.nTools; ++i) {
        const API_ElemType& type = (*toolbox.data)[i].type;
        const GS::UniString typeName = ElemTypeName (type);

        if (!wantedType.IsEmpty () && typeName.ToLowerCase () != wantedType)
            continue;

        short count = 0;
        GS::Array<API_FavoriteFolderHierarchy> folders;
        GS::Array<GS::UniString> names;
        const GSErrCode err = ACAPI_Favorite_GetNum (type, &count, &folders, &names);
        if (err != NoError) {
            // One refusing tool must not lose the other forty. Remember the first
            // error so a total failure is still reported as one.
            if (result == NoError)
                result = err;
            continue;
        }

        for (UIndex n = 0; n < names.GetSize (); ++n) {
            FavoriteEntry entry;
            entry.name = names[n];
            entry.elementType = typeName;
            if (n < folders.GetSize ())
                entry.folder = folders[n];
            favorites.Push (entry);
        }
    }

    // The SDK allocated this handle for us; free it.
    BMKillHandle (reinterpret_cast<GSHandle*> (&toolbox.data));

    // A project with no favourites at all is a legitimate answer, not an error —
    // so a per-tool failure only surfaces when nothing was collected either.
    return favorites.IsEmpty () ? result : NoError;
}

NativeCommandRegistrations GetFavoriteCommandRegistrations ()
{
    return MakeRegistrationView (FavoriteCommandRegistrations);
}

} // namespace geomsrv
