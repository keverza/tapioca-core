#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/AttributeCommands.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/CommandUtils.hpp" // AttributeNameToIndex

// ACAPI does not expose a profile attribute's vector image directly
// (ACAPI_Attribute_GetDefExt is documented for lines/fills/composites/layers/zones
// only — see ACAPinc.h:1855). The profile read therefore creates a TEMP wall with the
// picked profile, reads its memo.customOrigProfile, and computes the building
// dimensions via ProfileVectorImageOperations — all wrapped in an undoable session so
// the wall is created+deleted invisibly. This include is the reason this command is
// worth its own file: nothing else in the read domain needs it.
#include "ProfileVectorImageOperations.hpp"

#include <memory>
#include <utility>

#include "HashTable.hpp"

namespace geomsrv {

namespace {

// ---------------------------------------------------------------------------
// EvP.GetAttributeInfo { name, kind } -> composite thickness / profile bbox.
//
// Reads a NAMED attribute's geometry-backed dimensions for the cases where
// ShaftShell-style commands need to OFFSET geometry by the attribute's size:
//   composite  : totalThick [m] — for the future roof contour offset by the
//                wall composite width (read straight from API_CompWallType).
//   profile    : building-bbox width + height [m] — for the grate wall height,
//                which must come from the profile geometry, not an extra input.
//                ACAPI does not expose the profile attribute's vector image
//                (ACAPI_Attribute_GetDefExt is documented composite-only), so
//                the read creates a TEMP wall with the picked profile, reads
//                memo.customOrigProfile, calls
//                ProfileVectorImageOperations::CalculateBuildingDimensions,
//                then deletes the temp wall — all wrapped in an undoable
//                session so the project is unchanged when the call returns.
//   buildingMaterial : nothing to read beyond existence/index (returned always).
//
// LOGICALLY a read (no open caller transaction expected). It does, however,
// open its OWN undoable scope for the temp-wall probe — so it MUST be called
// OUTSIDE any open evp.transaction. IsWrite=false because the dispatcher must
// NOT wrap it (that would nest scopes and ACAPI_CallUndoableCommand refuses to
// nest — APIERR_REFUSEDCMD).
// ---------------------------------------------------------------------------
class GetAttributeInfoCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "GetAttributeInfo";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString name;
        if (!params.Get ("name", name) || name.IsEmpty ())
            return NativeCommandResult::Failure ("need name");
        GS::UniString kind;
        if (!params.Get ("kind", kind) || kind.IsEmpty ())
            return NativeCommandResult::Failure ("need kind = composite|profile|buildingMaterial");

        API_AttrTypeID typeID;
        if (kind == "composite")
            typeID = API_CompWallID;
        else if (kind == "profile")
            typeID = API_ProfileID;
        else if (kind == "buildingMaterial")
            typeID = API_BuildingMaterialID;
        else
            return NativeCommandResult::Failure ("kind must be composite|profile|buildingMaterial");

        API_AttributeIndex idx;
        if (!AttributeNameToIndex (typeID, name, idx))
            return NativeCommandResult::Failure (
                GS::UniString::Printf ("%s not found: %T", kind.ToCStr ().Get (), name.ToPrintf ()));

        GS::ObjectState os;
        os.Add ("name", name);
        os.Add ("kind", kind);
        os.Add ("index", idx.ToInt32_Deprecated ());

        // COMPOSITE — totalThick is a documented member of API_CompWallType, and
        // ACAPI_Attribute_Get fetches it directly. No memo / extended data needed.
        if (kind == "composite") {
            API_Attribute attribute = {};
            attribute.header.typeID = API_CompWallID;
            attribute.header.index = idx;
            const GSErrCode err = ACAPI_Attribute_Get (&attribute);
            if (err != NoError)
                return NativeCommandResult::Failure (
                    EVP_ACAPI_FAIL ("ACAPI_Attribute_Get", err, "API_CompWallID (composite thickness)"));
            os.Add ("thickness", attribute.compWall.totalThick); // meters
            return os;
        }

        // PROFILE — building bbox is read via a temp wall + ProfileVectorImageOperations.
        // The temp wall creation+deletion all happens inside one undoable command so
        // the project is byte-for-byte unchanged once the scope closes.
        if (kind == "profile") {
            double outHeight = 0.0, outWidth = 0.0;
            bool gotIt = false;
            GS::UniString diagError;

            const GSErrCode scopeErr =
                ACAPI_CallUndoableCommand (GS::UniString ("EvP.GetAttributeInfo: profile probe"), [&] () -> GSErrCode {
                    API_Element element = {};
                    element.header.type = API_WallID;
                    GSErrCode err = ACAPI_Element_GetDefaults (&element, nullptr);
                    if (err != NoError) {
                        diagError = EVP_ACAPI_FAIL ("ACAPI_Element_GetDefaults", err, "temp wall");
                        return err;
                    }

                    element.wall.modelElemStructureType = API_ProfileStructure;
                    element.wall.profileType = APISect_Poly;
                    element.wall.profileAttr = idx;
                    element.wall.begC.x = 0.0;
                    element.wall.begC.y = 0.0;
                    element.wall.endC.x = 0.001;
                    element.wall.endC.y = 0.0;
                    // GetDefaults gives a real height; don't zero it — a 0-height
                    // wall could be rejected or change the memo's customOrigProfile.
                    err = ACAPI_Element_Create (&element, nullptr);
                    if (err != NoError) {
                        diagError = EVP_ACAPI_FAIL ("ACAPI_Element_Create", err, "temp profiled wall");
                        return err;
                    }
                    const API_Guid tempGuid = element.header.guid;

                    API_ElementMemo memo = {};
                    err = ACAPI_Element_GetMemo (tempGuid, &memo, APIMemoMask_All);
                    if (err != NoError) {
                        diagError = EVP_ACAPI_FAIL ("ACAPI_Element_GetMemo", err, "APIMemoMask_All on the temp wall");
                        GS::Array<API_Guid> dead = { tempGuid };
                        ACAPI_Element_Delete (dead);
                        return err;
                    }

                    if (memo.customOrigProfile != nullptr) {
                        try {
                            const ProfileVectorImageOperations::Dimensions dims =
                                ProfileVectorImageOperations::CalculateBuildingDimensions (*memo.customOrigProfile);
                            outHeight = dims.height;
                            outWidth = dims.width;
                            gotIt = true;
                        }
                        catch (const GS::Exception& ex) {
                            diagError = GS::UniString ("CalculateBuildingDimensions threw: ") + ex.GetMessage ();
                        }
                        catch (...) {
                            diagError = "CalculateBuildingDimensions threw an unknown exception";
                        }
                    }
                    else {
                        diagError = "memo.customOrigProfile was null";
                    }

                    ACAPI_DisposeElemMemoHdls (&memo);

                    // Explicit delete: with the scope still open, the wall is gone
                    // BEFORE the command returns, so the user sees nothing new even
                    // if they never undo. The undo step records create-then-delete;
                    // undoing it also results in "no wall" (delete reverted then
                    // create reverted — net state unchanged either way).
                    GS::Array<API_Guid> dead = { tempGuid };
                    ACAPI_Element_Delete (dead);

                    return NoError;
                });

            if (scopeErr != NoError)
                return NativeCommandResult::Failure (
                    EVP_ACAPI_FAIL ("ACAPI_CallUndoableCommand", scopeErr,
                                    GS::UniString ("profile probe; inner failure: ") + diagError));
            if (!gotIt)
                return NativeCommandResult::Failure (
                    GS::UniString::Printf ("profile geometry unreadable: %T", diagError.ToPrintf ()));
            os.Add ("height", outHeight); // meters
            os.Add ("width", outWidth);   // meters
            return os;
        }

        // BUILDING MATERIAL — nothing to read past the index; the caller mainly
        // wants the name resolution verified.
        return os;
    }
};

// ---------------------------------------------------------------------------
// Preview data for an attribute picker.
//
// WHY THE NATIVE SIDE DRAWS NOTHING AND SENDS DATA INSTEAD. A picker row is a
// swatch plus a name, and the swatch is the half that makes a fill list usable -
// "25 %" and "50 %" are indistinguishable as words. But Archicad's own preview
// is a vector image the API does not hand out, so the honest arrangement is to
// send the DEFINITION - the 8x8 bit pattern, the dash lengths, the skin
// thicknesses and their colours - and let the client draw it. That keeps the
// bridge free of images, keeps the payload small enough to send the whole list
// at once, and means a client can draw the swatch at any size it likes.
//
// Everything here is MAIN THREAD ONLY and reads only. Handles from GetDefExt are
// disposed on every path, including the early ones.
// ---------------------------------------------------------------------------

GS::UniString HexColor (const API_RGBColor& rgb)
{
    return GS::UniString::Printf ("#%02X%02X%02X", static_cast<int> (rgb.f_red * 255.0 + 0.5),
                                  static_cast<int> (rgb.f_green * 255.0 + 0.5),
                                  static_cast<int> (rgb.f_blue * 255.0 + 0.5));
}

// A pen's colour. Pens are read one at a time from the active pen table rather
// than from the attribute table - they are not attributes and have no GUID.
bool PenColor (short penIndex, GS::UniString& hex)
{
    if (penIndex <= 0)
        return false;
    API_Pen pen = {};
    pen.index = penIndex;
    if (ACAPI_Attribute_GetPen (pen) != NoError)
        return false;
    hex = HexColor (pen.rgb);
    return true;
}

const char* FillKindName (API_FillSubtype subType)
{
    switch (subType) {
        case APIFill_Vector:
            return "vector";
        case APIFill_Symbol:
            return "symbol";
        case APIFill_Solid:
            return "solid";
        case APIFill_Empty:
            return "empty";
        case APIFill_LinearGradient:
            return "linearGradient";
        case APIFill_RadialGradient:
            return "radialGradient";
        case APIFill_Image:
            return "image";
    }
    return "vector";
}

const char* LineKindName (API_LtypTypeID type)
{
    switch (type) {
        case APILine_SolidLine:
            return "solid";
        case APILine_DashedLine:
            return "dashed";
        case APILine_SymbolLine:
            return "symbol";
    }
    return "solid";
}

// The building material's cut-fill pen colour, which is what a composite skin
// reads as at swatch size. Cached by the caller: a composite list re-asks for
// the same handful of materials on nearly every row.
GS::UniString BuildingMaterialColor (const API_AttributeIndex& index)
{
    API_Attribute material = {};
    material.header.typeID = API_BuildingMaterialID;
    material.header.index = index;
    if (ACAPI_Attribute_Get (&material) != NoError)
        return GS::UniString ();
    GS::UniString hex;
    if (PenColor (material.buildingMaterial.cutFillPen, hex))
        return hex;
    return GS::UniString ();
}

// A fill's 8x8 bit pattern, by index.
//
// A surface names a fill, and the surface picker draws it beside the colour -
// which is what tells one "Tinkas - GRUBUS" from the next when their colours are
// nearly the same. Read here rather than by the caller so a surface row costs
// one lookup and the fill listing keeps its own path.
bool FillPattern (const API_AttributeIndex& index, GS::Array<GS::Int32>& rows)
{
    if (!index.IsPositive ())
        return false;
    API_Attribute fill = {};
    fill.header.typeID = API_FilltypeID;
    fill.header.index = index;
    if (ACAPI_Attribute_Get (&fill) != NoError)
        return false;
    for (short row = 0; row < 8; ++row)
        rows.Push (static_cast<GS::Int32> (fill.filltype.bitPat[row]));
    return true;
}

// One attribute's swatch definition. Absent for kinds that have nothing to
// draw - a layer is a name and two flags, and inventing a swatch for it would
// be decoration rather than information.
bool AttributePreview (API_AttrTypeID typeID, const API_Attribute& attribute, GS::ObjectState& preview)
{
    if (typeID == API_MaterialID) {
        // A surface is a colour AND a fill AND, sometimes, a texture. Archicad's
        // own surface list shows all three, and it needs to: a project's
        // renders and plasters run to dozens of near-identical colours, and the
        // hatch beside them is what separates a roof tile from a render.
        preview.Add ("kind", GS::UniString ("surface"));
        preview.Add ("color", HexColor (attribute.material.surfaceRGB));
        GS::Array<GS::Int32> rows;
        if (FillPattern (attribute.material.ifill, rows))
            preview.Add ("pattern", rows);
        // The texture NAME is the signal, not the status bits: those describe
        // how a texture is mapped and are meaningless when there is none.
        const GS::UniString textureName (attribute.material.texture.texName);
        if (!textureName.IsEmpty ())
            preview.Add ("hasTexture", true);
        return true;
    }

    if (typeID == API_BuildingMaterialID) {
        preview.Add ("kind", GS::UniString ("color"));
        const GS::UniString hex = BuildingMaterialColor (attribute.header.index);
        if (!hex.IsEmpty ())
            preview.Add ("color", hex);
        return true;
    }

    if (typeID == API_FilltypeID) {
        preview.Add ("kind", GS::UniString ("pattern"));
        preview.Add ("fillKind", GS::UniString (FillKindName (attribute.filltype.subType)));
        // The 8x8 bit pattern, one byte per row. This is what makes 25 %, 50 %
        // and 75 % tell themselves apart in a list, and it is eight numbers
        // rather than an image.
        GS::Array<GS::Int32> rows;
        for (short row = 0; row < 8; ++row)
            rows.Push (static_cast<GS::Int32> (attribute.filltype.bitPat[row]));
        preview.Add ("pattern", rows);
        return true;
    }

    if (typeID == API_LinetypeID) {
        preview.Add ("kind", GS::UniString ("line"));
        preview.Add ("lineKind", GS::UniString (LineKindName (attribute.linetype.type)));
        if (attribute.linetype.type == APILine_DashedLine && attribute.linetype.nItems > 0) {
            API_AttributeDefExt defs = {};
            if (ACAPI_Attribute_GetDefExt (API_LinetypeID, attribute.header.index, &defs) == NoError) {
                GS::Array<double> dashes;
                if (defs.ltype_dashItems != nullptr) {
                    for (Int32 i = 0; i < attribute.linetype.nItems; ++i) {
                        dashes.Push ((*defs.ltype_dashItems)[i].dash);
                        dashes.Push ((*defs.ltype_dashItems)[i].gap);
                    }
                }
                ACAPI_DisposeAttrDefsHdlsExt (&defs);
                if (!dashes.IsEmpty ())
                    preview.Add ("dashes", dashes);
            }
        }
        return true;
    }

    if (typeID == API_CompWallID) {
        preview.Add ("kind", GS::UniString ("composite"));
        preview.Add ("thickness", attribute.compWall.totalThick);
        API_AttributeDefExt defs = {};
        if (ACAPI_Attribute_GetDefExt (API_CompWallID, attribute.header.index, &defs) == NoError) {
            GS::Array<GS::ObjectState> skins;
            if (defs.cwall_compItems != nullptr) {
                for (short i = 0; i < attribute.compWall.nComps; ++i) {
                    const API_CWallComponent& component = (*defs.cwall_compItems)[i];
                    GS::ObjectState skin;
                    skin.Add ("thickness", component.fillThick);
                    const GS::UniString hex = BuildingMaterialColor (component.buildingMaterial);
                    if (!hex.IsEmpty ())
                        skin.Add ("color", hex);
                    skins.Push (std::move (skin));
                }
            }
            ACAPI_DisposeAttrDefsHdlsExt (&defs);
            if (!skins.IsEmpty ())
                preview.Add ("skins", skins);
        }
        return true;
    }

    return false;
}

// The project's named pen tables.
//
// Listed alongside the pens themselves rather than behind a second verb: a pen
// picker needs both to draw one dropdown, and two round trips to fill one panel
// is two chances for them to disagree about which set is showing.
void CollectPenSets (GS::Array<GS::UniString>& names)
{
    GS::Array<API_Attribute> tables;
    if (ACAPI_Attribute_GetAttributesByType (API_PenTableID, tables) != NoError)
        return;
    for (const API_Attribute& table : tables) {
        const GS::UniString name (table.header.name);
        if (!name.IsEmpty ())
            names.Push (name);
    }
}

// The 255 pens of a NAMED pen table.
//
// WARNING: THIS DOES NOT CHANGE WHICH PEN SET THE PROJECT USES, and must not.
// Picking a pen to feed a graph is a read; switching the active pen table
// restyles every drawing in the project, which is a document write nobody asked
// for. Reading the table's own definition keeps the two apart - the picker can
// show any set without the act of looking changing anything.
bool PensOfSet (const GS::UniString& setName, GS::Array<API_Pen>& pens)
{
    API_AttributeIndex index;
    if (!AttributeNameToIndex (API_PenTableID, setName, index))
        return false;
    API_AttributeDefExt defs = {};
    if (ACAPI_Attribute_GetDefExt (API_PenTableID, index, &defs) != NoError)
        return false;
    if (defs.penTable_Items != nullptr)
        pens = *defs.penTable_Items;
    ACAPI_DisposeAttrDefsHdlsExt (&defs);
    return true;
}

// ---------------------------------------------------------------------------
// Attribute folders -> a slash-joined path per attribute.
//
// Folders are addressed by GUID and their content lists attribute GUIDs, so the
// walk builds guid -> path once and the listing looks each row up. Pens are
// excluded by construction: they are not attributes, have no GUID, and are not
// foldered.
//
// Depth-capped and visited-guarded. A malformed or cyclic folder tree must not
// hang Archicad on the main thread - the same rule the navigator walk follows.
// ---------------------------------------------------------------------------
constexpr Int32 kMaxFolderDepth = 12;

void CollectFolderPaths (const API_AttributeFolder& folder, const GS::UniString& path, Int32 depth,
                         GS::HashTable<GS::Guid, GS::UniString>& paths)
{
    if (depth > kMaxFolderDepth)
        return;
    API_AttributeFolderContent content = {};
    if (ACAPI_Attribute_GetFolderContent (folder, content) != NoError)
        return;
    for (const GS::Guid& attributeId : content.attributeIds)
        if (!paths.ContainsKey (attributeId))
            paths.Add (attributeId, path);
    for (const API_AttributeFolder& child : content.subFolders) {
        // The folder's own name is the last element of its path; the struct's
        // name field is documented as unused.
        const GS::UniString name = child.path.IsEmpty () ? GS::UniString () : child.path.GetLast ();
        const GS::UniString childPath = path.IsEmpty () ? name : path + "/" + name;
        CollectFolderPaths (child, childPath, depth + 1, paths);
    }
}

bool CollectAttributeFolders (API_AttrTypeID typeID, GS::HashTable<GS::Guid, GS::UniString>& paths)
{
    API_AttributeFolder root = {};
    root.typeID = typeID;
    if (ACAPI_Attribute_GetFolder (root) != NoError)
        return false;
    CollectFolderPaths (root, GS::UniString (), 0, paths);
    return true;
}

// ---------------------------------------------------------------------------
// EvP.ListAttributes { kind } -> every attribute of that kind in the open
// project, as a picker's rows.
//
// WHY THIS EXISTS. Inside Archicad's own dialogs an attribute picker is a
// native control (APIUserControlType_Layer and friends), and a command palette
// parameter declared `evp.Layer` gets one for free. A picker drawn in a WEBVIEW
// - the node graph editor - has no such control, and the browser must not
// enumerate a model domain itself: the list depends on the open project, so it
// cannot live in a static catalog either. So the domain is named by the node
// catalog and the MEMBERS are asked for here, which keeps the names-not-indices
// policy intact and gives both surfaces one answer rather than two.
//
// `label` is what a person picks; exactly one of `name` or `number` is what
// gets stored. Pens are the only kind keyed by number - a pen IS its number -
// and the only kind that carries a colour, because a pen picker without the
// colours is not a pen picker.
//
// A read, and cheap: one ACAPI_Attribute_GetAttributesByType per call.
// MAIN THREAD ONLY, which the base class is what guarantees.
// ---------------------------------------------------------------------------
class ListAttributesCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "ListAttributes";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString kind;
        if (!params.Get ("kind", kind) || kind.IsEmpty ())
            return NativeCommandResult::Failure ("need kind");

        GS::ObjectState os;
        os.Add ("kind", kind);
        GS::Array<GS::ObjectState> rows;

        // PENS are not in the attribute table; they live in the active pen
        // table and are read one at a time by index.
        if (kind == "pen") {
            // Every named pen table, so the picker can offer the set dropdown
            // from the same answer that carried the pens.
            GS::Array<GS::UniString> penSets;
            CollectPenSets (penSets);
            os.Add ("penSets", penSets);

            GS::UniString requestedSet;
            params.Get ("penSet", requestedSet);

            GS::Array<API_Pen> pens;
            if (!requestedSet.IsEmpty ()) {
                if (!PensOfSet (requestedSet, pens))
                    return NativeCommandResult::Failure (
                        GS::UniString::Printf ("no pen set named \"%T\"", requestedSet.ToPrintf ()));
                os.Add ("penSet", requestedSet);
            }
            else {
                // No set named: the project's CURRENT pens, which is what a
                // picker should open on.
                UInt32 count = 0;
                if (const GSErrCode err = ACAPI_Attribute_GetPenNum (count); err != NoError)
                    return NativeCommandResult::Failure (
                        EVP_ACAPI_FAIL ("ACAPI_Attribute_GetPenNum", err, "listing pens"));
                for (UInt32 i = 1; i <= count; ++i) {
                    API_Pen pen = {};
                    pen.index = static_cast<short> (i);
                    if (ACAPI_Attribute_GetPen (pen) == NoError)
                        pens.Push (pen);
                }
            }

            for (const API_Pen& pen : pens) {
                if (pen.index <= 0)
                    continue; // a gap in the table is not a failure of the listing
                const GS::UniString description (pen.description);
                GS::ObjectState row;
                row.Add ("label",
                         description.IsEmpty ()
                             ? GS::UniString::Printf ("Pen %d", static_cast<int> (pen.index))
                             : GS::UniString::Printf ("%d  %T", static_cast<int> (pen.index), description.ToPrintf ()));
                row.Add ("number", static_cast<Int32> (pen.index));
                row.Add ("index", static_cast<Int32> (pen.index));
                row.Add ("color", HexColor (pen.rgb));
                // Also as a preview, so a client has ONE swatch renderer rather
                // than a pen-shaped exception beside it.
                GS::ObjectState preview;
                preview.Add ("kind", GS::UniString ("color"));
                preview.Add ("color", HexColor (pen.rgb));
                row.Add ("preview", preview);
                rows.Push (std::move (row));
            }
            os.Add ("count", static_cast<Int32> (rows.GetSize ()));
            os.Add ("attributes", rows);
            return os;
        }

        API_AttrTypeID typeID;
        if (kind == "layer")
            typeID = API_LayerID;
        else if (kind == "fill")
            typeID = API_FilltypeID;
        else if (kind == "lineType")
            typeID = API_LinetypeID;
        else if (kind == "surface")
            typeID = API_MaterialID;
        else if (kind == "buildingMaterial")
            typeID = API_BuildingMaterialID;
        else if (kind == "composite")
            typeID = API_CompWallID;
        else if (kind == "profile")
            typeID = API_ProfileID;
        else
            return NativeCommandResult::Failure (
                "kind must be layer|pen|fill|lineType|surface|buildingMaterial|composite|profile");

        GS::Array<API_Attribute> attributes;
        if (const GSErrCode err = ACAPI_Attribute_GetAttributesByType (typeID, attributes); err != NoError)
            return NativeCommandResult::Failure (
                EVP_ACAPI_FAIL ("ACAPI_Attribute_GetAttributesByType", err, GS::UniString ("listing ") + kind));

        // One folder walk for the whole listing, not one per row.
        GS::HashTable<GS::Guid, GS::UniString> folders;
        CollectAttributeFolders (typeID, folders);

        for (const API_Attribute& attribute : attributes) {
            const GS::UniString name (attribute.header.name);
            if (name.IsEmpty ())
                continue; // an unnamed attribute cannot be picked by name
            GS::ObjectState row;
            row.Add ("label", name);
            row.Add ("name", name);
            row.Add ("index", attribute.header.index.ToInt32_Deprecated ());
            const GS::Guid attributeGuid = APIGuid2GSGuid (attribute.header.guid);
            if (folders.ContainsKey (attributeGuid)) {
                const GS::UniString& path = folders[attributeGuid];
                // The root folder is an empty path and is NOT sent as a folder:
                // a client would then draw every ungrouped attribute inside a
                // nameless group, which is worse than no grouping at all.
                if (!path.IsEmpty ())
                    row.Add ("folder", path);
            }
            GS::ObjectState preview;
            if (AttributePreview (typeID, attribute, preview))
                row.Add ("preview", preview);
            // Reported, not filtered: a hidden layer is still a legal choice,
            // and a picker that quietly dropped it would look like the layer
            // had been deleted. The caller decides how to draw it.
            if (typeID == API_LayerID) {
                row.Add ("hidden", (attribute.header.flags & APILay_Hidden) != 0);
                row.Add ("locked", (attribute.header.flags & APILay_Locked) != 0);
            }
            rows.Push (std::move (row));
        }
        os.Add ("count", static_cast<Int32> (rows.GetSize ()));
        os.Add ("attributes", rows);
        return os;
    }
};

const NativeCommandRegistration kAttributeCommandRegistrations[] = {
    { "GetAttributeInfo", &MakeRegisteredNativeCommand<GetAttributeInfoCommand>, false,
      R"json({
            "type":"object",
            "properties":{
                "name":{"type":"string","minLength":1},
                "kind":{"type":"string","enum":["composite","profile","buildingMaterial"]}
            },
            "additionalProperties":false,
            "required":["name","kind"]
        })json",
      R"json({
            "type":"object",
            "properties":{
                "name":{"type":"string"},
                "kind":{"type":"string","enum":["composite","profile","buildingMaterial"]},
                "index":{"type":"integer"},
                "thickness":{"type":"number"},
                "height":{"type":"number"},
                "width":{"type":"number"}
            },
            "additionalProperties":false,
            "required":["name","kind","index"]
        })json" },
    { "ListAttributes", &MakeRegisteredNativeCommand<ListAttributesCommand>, false,
      R"json({
            "type":"object",
            "properties":{
                "kind":{"type":"string","enum":["layer","pen","fill","lineType","surface","buildingMaterial","composite","profile"]},
                "penSet":{"type":"string","minLength":1}
            },
            "additionalProperties":false,
            "required":["kind"]
        })json",
      R"json({
            "type":"object",
            "properties":{
                "kind":{"type":"string","enum":["layer","pen","fill","lineType","surface","buildingMaterial","composite","profile"]},
                "count":{"type":"integer","minimum":0},
                "penSets":{"type":"array","items":{"type":"string"}},
                "penSet":{"type":"string"},
                "attributes":{
                    "type":"array",
                    "items":{
                        "type":"object",
                        "properties":{
                            "label":{"type":"string"},
                            "name":{"type":"string"},
                            "number":{"type":"integer"},
                            "index":{"type":"integer"},
                            "color":{"type":"string"},
                            "hidden":{"type":"boolean"},
                            "locked":{"type":"boolean"},
                            "folder":{"type":"string"},
                            "preview":{
                                "type":"object",
                                "properties":{
                                    "kind":{"type":"string","enum":["color","pattern","line","composite","surface"]},
                                    "color":{"type":"string"},
                                    "fillKind":{"type":"string","enum":["vector","symbol","solid","empty","linearGradient","radialGradient","image"]},
                                    "pattern":{"type":"array","minItems":8,"maxItems":8,"items":{"type":"integer","minimum":0,"maximum":255}},
                                    "hasTexture":{"type":"boolean"},
                                    "lineKind":{"type":"string","enum":["solid","dashed","symbol"]},
                                    "dashes":{"type":"array","items":{"type":"number"}},
                                    "thickness":{"type":"number"},
                                    "skins":{
                                        "type":"array",
                                        "items":{
                                            "type":"object",
                                            "properties":{
                                                "thickness":{"type":"number"},
                                                "color":{"type":"string"}
                                            },
                                            "additionalProperties":false,
                                            "required":["thickness"]
                                        }
                                    }
                                },
                                "additionalProperties":false,
                                "required":["kind"]
                            }
                        },
                        "additionalProperties":false,
                        "required":["label","index"]
                    }
                }
            },
            "additionalProperties":false,
            "required":["kind","count","attributes"]
        })json" }
};

} // namespace

NativeCommandRegistrations GetAttributeCommandRegistrations ()
{
    return MakeRegistrationView (kAttributeCommandRegistrations);
}

} // namespace geomsrv
