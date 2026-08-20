#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/LibraryObjectCommands.hpp"
#include "NativeCommands/CommandRegistration.hpp"

#include "Location.hpp" // IO::Location -> the .gsm's path, for the catalogue

// The MODERN library API, for the one thing the legacy struct cannot answer: the
// Library Manager's own tree (embedded vs loaded, library name, folders). This is
// the add-on's first ACAPI:: call anywhere; it links because the CMake already
// globs Support/Modules/*/*/*.lib, which picks up ArchicadAPI/Win/ArchicadAPIImp.LIB.
#include "ACAPI/Library/LibraryManager.hpp"
#include "ACAPI/Library/LibPart.hpp"
#include "ACAPI/Library/LibraryTreePath.hpp"

#include "NativeImage.hpp" // decoding a part's preview picture for the palette's grid

#include <cmath>
#include <cstdlib>
#include <memory>

namespace geomsrv {

namespace {

// Layer NAME -> attribute index. Scripts name layers ("Annotation"), never index
// them: an index is meaningless across projects, and the palette's layer picker
// hands back a name. Returns false if no layer by that name exists — which must
// stay an error, since silently placing on the wrong layer is exactly the kind of
// quiet wrongness that is expensive to notice later.
bool LayerNameToIndex (const GS::UniString& name, API_AttributeIndex& index)
{
    GS::Array<API_Attribute> layers;
    if (ACAPI_Attribute_GetAttributesByType (API_LayerID, layers) != NoError)
        return false;
    for (const API_Attribute& layer : layers) {
        if (GS::UniString (layer.header.name) == name) {
            index = layer.header.index;
            return true;
        }
    }
    return false;
}

// Writes one GDL parameter, converting the incoming TEXT according to the
// parameter's OWN declared type — the only thing that actually knows what it is
// (a `dt` boolean and an `iSlopeSymbolStyle` integer both arrive as text).
// Modelled on Tapir's ChangeParams (MIT), reduced to the types EvP sends.
void ApplyGdlParam (API_AddParType& param, const GS::UniString& value)
{
    switch (param.typeID) {
        case APIParT_CString:
        case APIParT_Title:
            GS::ucscpy (param.value.uStr, value.ToUStr ());
            break;

        case APIParT_Boolean:
        case APIParT_LightSw: {
            // Accept what a human or a JSON dump would plausibly write.
            const GS::UniString lowered = value.ToLowerCase ();
            const bool on = (lowered == "true" || lowered == "1" || lowered == "yes" || lowered == "on");
            param.value.real = on ? 1.0 : 0.0;
            break;
        }

        default:
            // Every remaining GDL type (Integer, PenCol, Length, RealNum, Angle,
            // LineTyp, Mater, FillPat, BuildingMaterial, Profile, ColRGB, Intens)
            // is stored in the same `real` double, so one numeric path covers them.
            param.value.real = std::atof (value.ToCStr ().Get ());
            break;
    }
}

// ---------------------------------------------------------------------------
// Tapioca.PlaceLibraryObject { libraryPartNames:[...], x, y, angle?, floorInd?,
//                              level?, parameters:[{name,value}], inheritFrom? }
//
// Places one GDL library object, FULLY CONFIGURED, in a single call. This
// deliberately replaces the four-call Tapir sequence the slope-symbol script used
// (CreateObjects -> GetDetailsOfElements -> RotateElements ->
// SetGDLParametersOfElements -> SetDetailsOfElements). Doing it natively removes
// two real bugs by construction rather than working around them:
//
//   * ROTATION. Tapir's RotateElements is RELATIVE and an object's angle is not
//     settable through it, so the script had to read each object's inherited angle
//     back and rotate by a computed delta. Here `angle` is ABSOLUTE and applied
//     before creation, so an inherited tool-default rotation cannot leak in.
//   * STORY. Tapir's CreateObjects has no floorIndex, so the script placed on the
//     ACTIVE story then reassigned — which can shift absolute Z, because elevation
//     is stored relative to the home story. Here floorInd is set AT creation.
//
// `libraryPartNames` is a CANDIDATE LIST tried in order: library part names differ
// by Archicad version ("Slope Symbol" / "Slope Symbol 29" / "27") and a script
// cannot know which library is loaded. The response reports which one matched.
// ---------------------------------------------------------------------------
class PlaceLibraryObjectCommand : public WriteCommand {
  public:
    GS::String GetName () const override
    {
        return "PlaceLibraryObject";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Array<GS::UniString> candidates;
        if (!params.Get ("libraryPartNames", candidates) || candidates.IsEmpty ()) {
            return NativeCommandResult::Failure ("need libraryPartNames=[...] (candidates, tried in order)");
        }

        double x = 0.0, y = 0.0;
        if (!params.Get ("x", x) || !params.Get ("y", y)) {
            return NativeCommandResult::Failure ("need x and y");
        }

        // Resolve the library part: the first candidate that exists wins.
        API_LibPart libPart = {};
        GS::UniString usedName, triedList;
        bool resolved = false;
        for (const GS::UniString& name : candidates) {
            API_LibPart candidate = {};
            GS::ucscpy (candidate.docu_UName, name.ToUStr ());
            // createIfMissing=false — never fabricate a virtual reference, which
            // would place Archicad's "missing part" dot and report ok:true while
            // having drawn nothing the user asked for. onlyPlaceable=true.
            const GSErrCode searchErr = ACAPI_LibraryPart_Search (&candidate, false, true);
            delete candidate.location;
            if (!triedList.IsEmpty ())
                triedList += ", ";
            triedList += "\"" + name + "\"";
            if (searchErr == NoError) {
                libPart = candidate;
                usedName = name;
                resolved = true;
                break;
            }
        }
        if (!resolved) {
            // GS::UniString(...) is REQUIRED: `a + b` yields the lazy
            // GS::UniString::Concatenation, which ObjectState::Add cannot store.
            return NativeCommandResult::Failure (
                GS::UniString ("No library part found. Tried: " + triedList +
                               ". Use the name shown in the Object tool's settings dialog "
                               "(the part's name, not the .gsm file name)."));
        }

        API_Element element = {};
        API_ElementMemo memo = {};
        element.header.type = API_ObjectID;

        // TEMPLATE: an existing object, or the tool defaults.
        //
        // Inheriting from a previously placed symbol is what makes a second run
        // match the first: pens, text style, size, iSlopeSymbolStyle and every
        // other GDL value come across, so the user styles ONE symbol by hand and
        // the rest follow. The tool default cannot do that — it is whatever the
        // Object tool happens to be set to, which drifts.
        GS::ObjectState inheritFrom, inheritElementId;
        GS::UniString inheritGuid;
        const bool inherit = params.Get ("inheritFrom", inheritFrom);
        if (params.Contains ("inheritFrom") &&
            (!inherit || !inheritFrom.Get ("elementId", inheritElementId) ||
             !inheritElementId.Get ("guid", inheritGuid) || inheritGuid.IsEmpty ())) {
            return NativeCommandResult::Failure ("inheritFrom needs elementId.guid");
        }

        GSErrCode err = NoError;
        if (inherit) {
            element.header.guid = APIGuidFromString (inheritGuid.ToCStr ().Get ());
            err = ACAPI_Element_Get (&element);
            if (err == NoError)
                err = ACAPI_Element_GetMemo (element.header.guid, &memo, APIMemoMask_AddPars);
            if (err != NoError) {
                return NativeCommandResult::Failure (EVP_ACAPI_FAIL (
                    "ACAPI_Element_Get/GetMemo", err,
                    GS::UniString::Printf ("inheritFrom.elementId.guid %T could not be read. Pass an existing "
                                           "object, or omit it to use the tool defaults.",
                                           inheritGuid.ToPrintf ())));
            }
            // A fresh element, not an edit of the source. Without this the create
            // would collide with the object we are copying FROM.
            element.header.guid = APINULLGuid;
        }
        else {
            err = ACAPI_Element_GetDefaults (&element, &memo);
            if (err != NoError) {
                return NativeCommandResult::Failure (
                    EVP_ACAPI_FAIL ("ACAPI_Element_GetDefaults", err, "API_ObjectID (tool defaults)"));
            }
        }

        element.object.libInd = libPart.index;
        element.object.pos.x = x;
        element.object.pos.y = y;

        // ⚠️ ACAPI_Element_GetDefaults returns the parameter list of whatever library
        // part the Object TOOL currently defaults to — NOT the part we just resolved.
        // Searching those for "txt"/"dt" looks for the slope symbol's parameters in
        // some unrelated object's list and reports them missing. Load the CHOSEN
        // part's own parameters and swap them in. (Same pattern as Tapir's
        // ExtendedElementCommands.cpp marker setup.)
        //
        // NOT when inheriting: the source object's params ARE the styling we were
        // asked to copy, and replacing them with the library defaults would throw
        // away the very thing inheritFrom exists to preserve.
        if (!inherit) {
            double a = 0.0, b = 0.0;
            Int32 addParNum = 0;
            API_AddParType** libParams = nullptr;
            const GSErrCode paramErr = ACAPI_LibraryPart_GetParams (libPart.index, &a, &b, &addParNum, &libParams);
            if (paramErr != NoError) {
                ACAPI_DisposeElemMemoHdls (&memo);
                return NativeCommandResult::Failure (GS::UniString::Printf (
                    "ACAPI_LibraryPart_GetParams failed for \"%T\" (%d).", usedName.ToPrintf (), (int) paramErr));
            }
            ACAPI_DisposeAddParHdl (&memo.params); // the wrong part's list
            memo.params = libParams;               // freed by DisposeElemMemoHdls below

            // a/b are the part's OWN default size. Without these the symbol inherits
            // the previous default part's ratios and is drawn at the wrong scale.
            element.object.xRatio = a;
            element.object.yRatio = b;
        }

        double angle = 0.0;
        params.Get ("angle", angle); // radians, ABSOLUTE
        element.object.angle = angle;

        GS::Int32 floorInd = 0;
        if (params.Get ("floorInd", floorInd))
            element.header.floorInd = (short) floorInd;

        // Layer by NAME. A name that does not exist is an ERROR, never a fallback to
        // the tool default: silently placing on the wrong layer is precisely the kind
        // of quiet wrongness nobody notices until it is buried in a drawing.
        GS::UniString layerName;
        if (params.Get ("layer", layerName) && !layerName.IsEmpty ()) {
            API_AttributeIndex layerIndex;
            if (!LayerNameToIndex (layerName, layerIndex)) {
                ACAPI_DisposeElemMemoHdls (&memo);
                return NativeCommandResult::Failure (
                    GS::UniString ("No layer named \"" + layerName + "\" in this project. NOTHING was placed."));
            }
            element.header.layer = layerIndex;
        }

        double level = 0.0;
        if (params.Get ("level", level))
            element.object.level = level; // elevation relative to its home story

        // ---- GDL parameters -------------------------------------------------
        GS::Array<GS::ObjectState> parameters;
        const bool haveParameters = params.Get ("parameters", parameters);
        if (params.Contains ("parameters") && !haveParameters) {
            ACAPI_DisposeElemMemoHdls (&memo);
            return NativeCommandResult::Failure ("parameters must be an array of {name,value} records");
        }

        GS::Array<GS::UniString> unknownParams;
        if (haveParameters && !parameters.IsEmpty () && memo.params != nullptr) {
            const GSSize paramCount = BMGetHandleSize ((GSHandle) memo.params) / sizeof (API_AddParType);
            for (const GS::ObjectState& requested : parameters) {
                GS::UniString name, value;
                if (!requested.Get ("name", name) || name.IsEmpty () || !requested.Get ("value", value)) {
                    ACAPI_DisposeElemMemoHdls (&memo);
                    return NativeCommandResult::Failure (
                        "every parameters entry needs non-empty name and string value");
                }
                bool matched = false;
                for (GSIndex p = 0; p < paramCount; ++p) {
                    API_AddParType& param = (*memo.params)[p];
                    if (name != GS::UniString (param.name))
                        continue;
                    ApplyGdlParam (param, value);
                    matched = true;
                    break;
                }
                if (!matched)
                    unknownParams.Push (name);
            }
        }

        // A misspelled GDL parameter must NOT pass silently: the object would be
        // placed looking wrong (no slope text, say) under a cheerful ok:true.
        if (!unknownParams.IsEmpty ()) {
            GS::UniString list;
            for (const GS::UniString& name : unknownParams) {
                if (!list.IsEmpty ())
                    list += ", ";
                list += "\"" + name + "\"";
            }

            // Say what DOES exist. "no such parameter" alone sends the reader off to
            // hunt through Archicad's UI; the actual list turns it into a typo fix.
            // Read BEFORE disposing the memo — these names live in that handle.
            GS::UniString available;
            if (memo.params != nullptr) {
                const GSSize total = BMGetHandleSize ((GSHandle) memo.params) / sizeof (API_AddParType);
                for (GSIndex p = 0; p < total && p < 60; ++p) {
                    if (!available.IsEmpty ())
                        available += ", ";
                    available += GS::UniString ((*memo.params)[p].name);
                }
                if (total > 60)
                    available += GS::UniString::Printf (", ... (%d total)", (int) total);
            }

            ACAPI_DisposeElemMemoHdls (&memo);
            return NativeCommandResult::Failure (GS::UniString ("Library part \"" + usedName +
                                                                "\" has no GDL parameter(s): " + list +
                                                                ". NOTHING was placed. It does have: " + available));
        }

        // NO undo scope here — see WriteCommand. The caller has one open.
        err = ACAPI_Element_Create (&element, &memo);
        ACAPI_DisposeElemMemoHdls (&memo);

        if (err != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL (
                "ACAPI_Element_Create", err,
                GS::UniString::Printf ("library object \"%T\" at (%.3f, %.3f)", usedName.ToPrintf (), x, y)));
        }

        GS::ObjectState elementId;
        elementId.Add ("guid", GS::UniString (APIGuidToString (element.header.guid).ToCStr ()));
        os.Add ("elementId", elementId);
        os.Add ("libraryPartName", usedName);
        os.Add ("libInd", (GS::Int32) libPart.index);
        return os;
    }
};

// What FORMAT a library part's preview picture is stored in, and how big it is.
// Reads the header only — the bytes are not returned, because the question this
// answers is whether a thumbnail is DECODABLE AT ALL, and that is settled by the
// mime string alone.
//
// ⚠️ THE OPEN QUESTION THIS EXISTS FOR. The preview lives in the section the
// DevKit calls API_SectInfoGIF, whose payload is a NUL-terminated MIME string
// followed by the raw image bytes (LibPart_Test's SetPreviewPictureToLibPart
// writes exactly that shape). But NewDisplay::NativeImage decodes JPEG and PNG
// ONLY — its Encoding enum has no GIF — so if the stock library really stores
// GIF, a thumbnail needs a decoder the DevKit does not ship, and that changes
// what the picker can offer. The section's NAME says GIF; the example's own
// comment says "prefer gif, but you can change gif to jpeg or png", so the name
// is legacy and the MIME STRING is the authority. Only a real library answers
// it, which is why this is reported rather than assumed.
void ReadPreviewFormat (Int32 libIndex, GS::UniString& mime, GS::Int32& byteCount)
{
    mime.Clear ();
    byteCount = 0;

    API_LibPartSection section = {};
    section.sectType = API_SectInfoGIF;

    GSHandle sectionHdl = nullptr;
    if (ACAPI_LibraryPart_GetSection (libIndex, &section, &sectionHdl, nullptr) != NoError || sectionHdl == nullptr)
        return;

    const GSSize size = BMGetHandleSize (sectionHdl);
    if (size > 0) {
        // The MIME string comes first, NUL-terminated; the image follows it.
        // Bounded by the handle size rather than trusting the terminator — a
        // truncated section would otherwise walk off the end.
        const char* data = *sectionHdl;
        GSSize length = 0;
        while (length < size && data[length] != '\0')
            ++length;
        if (length < size) {
            // A LOCAL COPY, NUL-terminated by construction: the MIME text is a
            // slice of a larger handle, and GS::UniString has no "first N bytes
            // of this buffer" constructor — handing it the raw pointer would run
            // past the slice into the image bytes.
            GS::String text;
            for (GSSize i = 0; i < length; ++i)
                text += data[i];
            mime = GS::UniString (text);
            byteCount = (GS::Int32) (size - length - 1);
        }
    }
    BMKillHandle (&sectionHdl);
}

// ---------------------------------------------------------------------------
// Tapioca.ListLibraryParts { subtype?, nameFilter?, limit? }
//
// What the loaded libraries actually contain THAT CAN BE PLACED. It exists
// because until now every command that wanted a library part had to HARD-CODE a
// candidate name list (PlaceLibraryObject's `libraryPartNames`), guessing at the
// spelling the installed library happens to use — "Slope Symbol" / "Slope Symbol
// 29" / "27". A command can now look instead of guess, and the palette's
// evp.LibraryPart picker is built on the same read.
//
// ⚠️ `subtype` OMITTED MEANS OBJECTS, NOT EVERYTHING. The first cut listed every
// registered library part, which put surfaces, images, lamps, section markers and
// templates in front of a user who had asked "which object do I place" — the
// report was *"data is all over the place"*. This now answers the question
// Archicad's own Object Settings browser answers, and each row carries the
// Library Manager `treePath` so a caller can show the folders the user knows.
//
// A pure READ: no undo step, MainThreadCommand.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Tapioca.GetLibraryPartPreviewInfo { name }
//
// ONE part's preview picture header — the format and the byte count, never the
// bytes. It exists to settle a single question before any thumbnail work is
// built on a guess: CAN a library part's preview be decoded with what the DevKit
// ships? See ReadPreviewFormat for why the answer is not knowable from the
// headers (the section is NAMED for GIF, its payload declares its own MIME, and
// NativeImage decodes only JPEG and PNG).
//
// Deliberately per-part rather than a column on ListLibraryParts: reading a
// section is a file touch, and doing it for every row of a multi-thousand-part
// catalogue would make the picker slow to answer a question that needs one part.
//
// A pure READ: no undo step, MainThreadCommand.
// ---------------------------------------------------------------------------
class GetLibraryPartPreviewInfoCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "GetLibraryPartPreviewInfo";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString name;
        if (!params.Get ("name", name) || name.IsEmpty ())
            return NativeCommandResult::Failure ("need name (the library part's name, as the Object tool shows it)");

        API_LibPart part = {};
        GS::ucscpy (part.docu_UName, name.ToUStr ());
        const GSErrCode searchErr = ACAPI_LibraryPart_Search (&part, false, true);
        delete part.location;
        if (searchErr != NoError) {
            return NativeCommandResult::Failure (
                EVP_ACAPI_FAIL ("ACAPI_LibraryPart_Search", searchErr,
                                GS::UniString::Printf ("no placeable library part named \"%T\"", name.ToPrintf ())));
        }

        GS::UniString mime;
        GS::Int32 byteCount = 0;
        ReadPreviewFormat (part.index, mime, byteCount);

        GS::ObjectState os;
        os.Add ("name", name);
        // Empty mime with zero bytes means the part simply has no preview — a
        // normal state, not a failure, and one a thumbnail grid has to handle.
        os.Add ("previewMime", mime);
        os.Add ("previewBytes", byteCount);
        // ⚠️ THE ANSWER THE THUMBNAIL WORK IS WAITING ON. NewDisplay::NativeImage
        // decodes JPEG and PNG only, so this says whether the picker could render
        // this preview at all without a decoder the DevKit does not ship.
        const GS::UniString lowered = mime.ToLowerCase ();
        os.Add ("decodable", lowered == "image/png" || lowered == "image/jpeg" || lowered == "image/jpg");
        return os;
    }
};

class ListLibraryPartsCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "ListLibraryParts";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString subtype, nameFilter;
        params.Get ("subtype", subtype);
        params.Get ("nameFilter", nameFilter);

        // A full Archicad library is thousands of parts, and the whole list on the
        // wire is a response nobody reads. `limit` caps it and `truncated` SAYS SO —
        // a silently shortened list would read as "the library does not have it".
        GS::Int32 limit = 2000;
        params.Get ("limit", limit);
        if (limit <= 0)
            return NativeCommandResult::Failure ("limit must be a positive integer");

        GS::Array<LibraryPartEntry> parts;
        GS::UniString refusal;
        const GSErrCode err = CollectLibraryParts (subtype, parts, refusal);
        if (!refusal.IsEmpty ()) {
            // A bad or out-of-scope subtype is the CALLER's mistake, not the API's,
            // so it reports as itself rather than as a decoded ACAPI code — the
            // sentence already says what to write instead.
            return NativeCommandResult::Failure (EVP_FAIL (refusal, "ListLibraryParts subtype"));
        }
        if (err != NoError) {
            return NativeCommandResult::Failure (
                EVP_ACAPI_FAIL ("ACAPI_LibraryPart_GetNum/Get", err,
                                GS::UniString::Printf ("enumerating the loaded libraries (subtype filter \"%T\")",
                                                       subtype.ToPrintf ())));
        }

        const GS::UniString wanted = nameFilter.ToLowerCase ();

        GS::Array<GS::ObjectState> rows;
        GS::Int32 matched = 0;
        for (const LibraryPartEntry& part : parts) {
            if (!wanted.IsEmpty () && !part.name.ToLowerCase ().Contains (wanted) &&
                !part.file.ToLowerCase ().Contains (wanted))
                continue;
            ++matched;
            if ((GS::Int32) rows.GetSize () >= limit)
                continue; // still counted, so `total` stays the honest number

            GS::ObjectState row;
            row.Add ("name", part.name);
            row.Add ("file", part.file);
            row.Add ("unID", part.unID);
            row.Add ("type", part.type);
            row.Add ("location", part.location);
            row.Add ("placeable", part.placeable);
            row.Add ("missing", part.missing);
            // The Library Manager's own folders, so a caller can present the tree
            // the user recognises instead of inventing a grouping of its own.
            row.Add ("treePath", part.treePath);
            row.Add ("library", part.library);
            row.Add ("embedded", part.embedded);
            rows.Push (row);
        }

        GS::ObjectState os;
        os.Add ("parts", rows);
        os.Add ("total", matched);
        os.Add ("truncated", matched > (GS::Int32) rows.GetSize ());
        return os;
    }
};

const NativeCommandRegistration LibraryObjectCommandRegistrations[] = {
    { "GetLibraryPartPreviewInfo", &MakeRegisteredNativeCommand<GetLibraryPartPreviewInfoCommand>, false,
      R"json({"type":"object","properties":{"name":{"type":"string","minLength":1}},"additionalProperties":false,"required":["name"]})json",
      R"json({"type":"object","properties":{"name":{"type":"string"},"previewMime":{"type":"string"},"previewBytes":{"type":"integer"},"decodable":{"type":"boolean"}},"additionalProperties":false,"required":["name","previewMime","previewBytes","decodable"]})json" },

    { "ListLibraryParts", &MakeRegisteredNativeCommand<ListLibraryPartsCommand>, false,
      R"json({"type":"object","properties":{"subtype":{"type":"string"},"nameFilter":{"type":"string"},"limit":{"type":"integer","minimum":1}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"parts":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"file":{"type":"string"},"unID":{"type":"string"},"type":{"type":"string"},"location":{"type":"string"},"placeable":{"type":"boolean"},"missing":{"type":"boolean"},"treePath":{"type":"array","items":{"type":"string"}},"library":{"type":"string"},"embedded":{"type":"boolean"}},"additionalProperties":false,"required":["name","file","unID","type","location","placeable","missing","treePath","library","embedded"]}},"total":{"type":"integer"},"truncated":{"type":"boolean"}},"additionalProperties":false,"required":["parts","total","truncated"]})json" },

    { "PlaceLibraryObject", &MakeRegisteredNativeCommand<PlaceLibraryObjectCommand>, false,
      R"json({"type":"object","properties":{"libraryPartNames":{"type":"array","minItems":1,"items":{"type":"string","minLength":1}},"x":{"type":"number"},"y":{"type":"number"},"angle":{"type":"number"},"floorInd":{"type":"integer"},"level":{"type":"number"},"layer":{"type":"string"},"inheritFrom":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]},"parameters":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string","minLength":1},"value":{"type":"string"}},"additionalProperties":false,"required":["name","value"]}}},"additionalProperties":false,"required":["libraryPartNames","x","y"]})json", R"json({"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"libraryPartName":{"type":"string"},"libInd":{"type":"integer"}},"additionalProperties":false,"required":["elementId","libraryPartName","libInd"]})json" },
};

} // namespace

// ---------------------------------------------------------------------------
// The placeable-object catalogue
// ---------------------------------------------------------------------------
//
// ⚠️ TWO APIS, EACH FOR THE ONE THING THE OTHER CANNOT DO. This looked like a
// free choice on the first cut, and it is not.
//
//   * THE LEGACY ENUMERATION carries API_LibPart::typeID — Object / Lamp / Door /
//     ZoneStamp — the ONLY place the "which tool places this" subtype exists.
//     The modern LibraryManager cannot filter by it: FindLibParts has four
//     selectors (All / Object / Text / Image) and LibPartType is the FILE kind
//     (.gsm / .png / .txt), not the tool. The legacy struct also carries
//     isPlaceable and isTemplate, which are what keep a picker from offering
//     things that cannot be placed at all.
//   * THE MODERN LibraryManager carries the LIBRARY TREE — embedded vs loaded,
//     each library's name, the folders inside it. API_LibPart has a filesystem
//     path and nothing else, and a filesystem path is not the tree the user sees
//     in Object Settings.
//
// So the loop enumerates and filters with the legacy calls, and only THEN asks
// the modern manager for the surviving parts' tree paths. The order matters: the
// tree lookup is two calls per part and a full Archicad library is thousands of
// them, so doing it first would pay for every macro and texture in the library
// in order to throw them away.
GS::UniString LibPartTypeName (API_LibTypeID typeID)
{
    switch (typeID) {
        case APILib_SpecID:
            return "Special";
        case APILib_WindowID:
            return "Window";
        case APILib_DoorID:
            return "Door";
        case APILib_ObjectID:
            return "Object";
        case APILib_LampID:
            return "Lamp";
        case APILib_RoomID:
            return "ZoneStamp";
        case APILib_PropertyID:
            return "Property";
        case APILib_PlanSignID:
            return "PlanSign";
        case APILib_LabelID:
            return "Label";
        case APILib_MacroID:
            return "Macro";
        case APILib_PictID:
            return "Picture";
        case APILib_ListSchemeID:
            return "ListScheme";
        case APILib_SkylightID:
            return "Skylight";
        case APILib_OpeningSymbolID:
            return "OpeningSymbol";
        default:
            return "Other";
    }
}

namespace {

// Which API_LibTypeID a subtype spelling means, and whether it is servable here.
// `wantedType` is left at API_ZombieLibID for "all".
//
// ⚠️ REFUSING AN OPENING BY NAME is the point of this function existing. A door
// or a window is not placed on its own — it is cut into a host wall or roof, and
// the settings that decide the result (which host, the reveal, the anchor) have
// nothing to do with choosing a .gsm. A picker that cheerfully listed doors
// would be a control that looks finished and cannot do the job, which is worse
// than one that says plainly what it does not cover.
//
// ⚠️ AND THE ACCEPTED SET IS NOT AN `enum` IN THE INPUT SCHEMA, deliberately.
// It was, for one build, and the schema validator then rejected subtype="Door"
// upstream of this function with "$.subtype: value is not in enum" — replacing a
// sentence that says WHAT TO WRITE INSTEAD with one that says nothing. The
// validator is the right tool for a shape; it is the wrong tool for a boundary
// the user has to understand. One authority, and it is this function.
bool ResolveSubtype (const GS::UniString& spelling, API_LibTypeID& wantedType, GS::UniString& refusal)
{
    const GS::UniString wanted = spelling.ToLowerCase ();

    if (wanted.IsEmpty () || wanted == "object") {
        wantedType = APILib_ObjectID;
        return true;
    }
    if (wanted == "all") {
        wantedType = API_ZombieLibID;
        return true;
    }
    if (wanted == "lamp") {
        wantedType = APILib_LampID;
        return true;
    }
    if (wanted == "zonestamp") {
        wantedType = APILib_RoomID;
        return true;
    }
    if (wanted == "label") {
        wantedType = APILib_LabelID;
        return true;
    }

    if (wanted == "door" || wanted == "window" || wanted == "skylight") {
        refusal = GS::UniString ("subtype \"" + spelling +
                                 "\" is not available from this picker. An opening is cut into a host "
                                 "wall or roof, so choosing one needs that host's context rather than a "
                                 "list of library parts. Use \"Object\" (the default), \"Lamp\", "
                                 "\"ZoneStamp\", \"Label\", or \"all\".");
        return false;
    }

    refusal = GS::UniString ("unknown subtype \"" + spelling +
                             "\". Use \"Object\" (the default), \"Lamp\", \"ZoneStamp\", \"Label\", "
                             "or \"all\".");
    return false;
}

// The tree Archicad's Object Settings shows, for one part: the [Embedded|Loaded]
// root, the library's name, then its folders. Fills entry.library, entry.embedded
// and entry.treePath together, because the three are read from the same lookup.
//
// Everything here degrades rather than fails: a part whose library or path cannot
// be read still appears, at the top level. A tree missing one branch is worth far
// more than a browser that refuses to open.
void ReadTreePlacement (ACAPI::Library::LibraryManager& manager, Int32 libIndex, LibraryPartEntry& entry)
{
    const auto libPart = manager.GetLibPartByLibInd (libIndex);
    if (!libPart.IsOk ())
        return;

    const auto library = manager.GetLibraryOfLibPart (*libPart);
    if (library.IsOk ()) {
        const auto name = library->GetName ();
        if (name.IsOk ())
            entry.library = *name;
        const auto embedded = library->IsEmbedded ();
        if (embedded.IsOk ())
            entry.embedded = *embedded;
    }

    entry.treePath.Push (entry.embedded ? "Embedded Library" : "Loaded Libraries");

    const auto path = manager.GetLibraryTreePathOfLibPart (*libPart);
    if (!path.IsOk ())
        return;

    const std::vector<GS::UniString> raw = path->GetParts ();

    // ⚠️ DROP THE LEADING SOURCE CONTAINER. GetParts is Archicad's "Folder View
    // (with sources)" path and it BEGINS with the physical package the part was
    // shipped in — "Fire Protection.libpack", "Archicad Library 27.lcf",
    // "Grid Tool.apx". Ignoring it is what MERGES the packages: a dozen
    // .libpacks all publish into the one "Object Library", so keeping the
    // container gives a dozen unrelated roots holding one library each, while
    // dropping it lets their folders fall together into the tree Archicad shows.
    //
    // Diagnosed from the live tree with "with sources" turned on, where
    // "Fire Protection.libpack" appeared TWICE — once from this path and once
    // from the level the browser adds — which is what finally identified the
    // leading component for what it is.
    //
    // Matched by extension as well as against the library's own name, because
    // GetName is not guaranteed to be spelled identically to the path node, and
    // only ever at the FRONT: a folder further down that happens to end in
    // ".apx" is a real folder and stays.
    UIndex firstFolder = 0;
    while (firstFolder < raw.size ()) {
        const GS::UniString node = raw[firstFolder].ToLowerCase ();
        const bool isContainer = raw[firstFolder] == entry.library || node.EndsWith (".libpack") ||
                                 node.EndsWith (".lcf") || node.EndsWith (".apx");
        if (!isContainer)
            break;
        ++firstFolder;
    }

    for (UIndex i = firstFolder; i < raw.size (); ++i) {
        // ⚠️ AND DROP THE LEAF FILE AND PART NODES. The path ends
        // "… / Chairs / Armchair 01.gsm" (and GetLastPart is documented as "the
        // last node/libpart"), neither of which is a folder. Left on, the browser
        // grew a folder per object holding exactly one row — reported as "I have
        // open .gsm folder which always has only one element just select it".
        if (raw[i] == entry.file || raw[i] == entry.name)
            continue;
        entry.treePath.Push (raw[i]);
    }
}

} // namespace

bool LoadLibraryPartPreview (const GS::UniString& partName, NewDisplay::NativeImage& image)
{
    if (partName.IsEmpty ())
        return false;

    API_LibPart part = {};
    GS::ucscpy (part.docu_UName, partName.ToUStr ());
    const GSErrCode searchErr = ACAPI_LibraryPart_Search (&part, false, true);
    delete part.location;
    if (searchErr != NoError)
        return false;

    API_LibPartSection section = {};
    section.sectType = API_SectInfoGIF;

    GSHandle sectionHdl = nullptr;
    if (ACAPI_LibraryPart_GetSection (part.index, &section, &sectionHdl, nullptr) != NoError || sectionHdl == nullptr)
        return false;

    bool decoded = false;
    const GSSize size = BMGetHandleSize (sectionHdl);
    if (size > 0) {
        // MIME string first, NUL-terminated, then the image. Bounded by the
        // handle size rather than trusting the terminator — a truncated section
        // would otherwise walk off the end.
        const char* data = *sectionHdl;
        GSSize length = 0;
        while (length < size && data[length] != '\0')
            ++length;

        if (length < size) {
            GS::String mimeText;
            for (GSSize i = 0; i < length; ++i)
                mimeText += data[i];
            const GS::UniString mime = GS::UniString (mimeText).ToLowerCase ();

            // ⚠️ THE MIME STRING DECIDES, NOT THE SECTION NAME. Only PNG and JPEG
            // are attempted because those are the only two NativeImage::Encoding
            // has; a TIFF preview (which the stock library really does ship) is
            // reported as "no preview" rather than handed to a decoder that would
            // fail, or worse, misread it.
            NewDisplay::NativeImage::Encoding encoding = NewDisplay::NativeImage::Unknown;
            if (mime == "image/png")
                encoding = NewDisplay::NativeImage::PNG;
            else if (mime == "image/jpeg" || mime == "image/jpg")
                encoding = NewDisplay::NativeImage::JPEG;

            const UInt32 imageBytes = (UInt32) (size - length - 1);
            if (encoding != NewDisplay::NativeImage::Unknown && imageBytes > 0) {
                try {
                    image = NewDisplay::NativeImage (data + length + 1, imageBytes, encoding);
                    decoded = true;
                }
                catch (...) {
                    // A corrupt preview must cost one blank cell, never the
                    // dialog it is being drawn into.
                    decoded = false;
                }
            }
        }
    }
    BMKillHandle (&sectionHdl);
    return decoded;
}

GSErrCode CollectLibraryParts (const GS::UniString& subtypeFilter, GS::Array<LibraryPartEntry>& parts,
                               GS::UniString& refusal)
{
    parts.Clear ();
    refusal.Clear ();

    API_LibTypeID wantedType = APILib_ObjectID;
    if (!ResolveSubtype (subtypeFilter, wantedType, refusal))
        return APIERR_BADPARS;

    Int32 count = 0;
    const GSErrCode countErr = ACAPI_LibraryPart_GetNum (&count);
    if (countErr != NoError)
        return countErr;

    // The legacy index of each survivor, parallel to `parts`. Needed only to ask
    // the modern manager for the tree path below, so it is a local rather than a
    // field on LibraryPartEntry — an index is "not a stable identifier" (the
    // header says so) and has no business travelling to a script.
    GS::Array<Int32> survivingIndices;

    for (Int32 index = 1; index <= count; ++index) {
        API_LibPart part = {};
        part.index = index;
        if (ACAPI_LibraryPart_Get (&part) != NoError)
            continue; // one unreadable part must not lose the other 2,000

        // ⚠️ ACAPI_LibraryPart_Get ALLOCATES `location` and hands ownership over.
        // Every exit from this iteration has to free it, which is why the path is
        // read out immediately and the delete happens before any `continue`.
        GS::UniString path;
        if (part.location != nullptr) {
            part.location->ToPath (&path);
            delete part.location;
            part.location = nullptr;
        }

        if (wantedType != API_ZombieLibID && part.typeID != wantedType)
            continue;

        // ⚠️ THE TWO FLAGS THAT MADE THE FIRST CUT UNUSABLE, reported live as
        // "surfaces, images, lights, sections, templates" turning up in a picker
        // whose whole job is "choose a thing to place":
        //   * isPlaceable false is a part that exists to be USED BY another part
        //     — a macro, a texture, a list scheme. It cannot be placed at all, so
        //     offering it is offering a dead end;
        //   * isTemplate is a part meant to be DERIVED FROM, not stamped.
        if (!part.isPlaceable || part.isTemplate)
            continue;

        LibraryPartEntry entry;
        entry.name = GS::UniString (part.docu_UName);
        entry.file = GS::UniString (part.file_UName);
        // ownUnID is a plain char[128] C string, not a struct of four indices.
        entry.unID = GS::UniString (part.ownUnID);
        entry.type = LibPartTypeName (part.typeID);
        entry.location = path;
        entry.placeable = part.isPlaceable;
        entry.missing = part.missingDef;
        parts.Push (entry);
        survivingIndices.Push (index);
    }

    // The tree placement, for the SURVIVORS only. A failure here is deliberately
    // not fatal: the parts are already correct and complete, and a flat list
    // beats no list at all.
    auto manager = ACAPI::Library::GetLibraryManager ();
    if (manager.IsOk ()) {
        for (UIndex i = 0; i < parts.GetSize (); ++i)
            ReadTreePlacement (*manager, survivingIndices[i], parts[i]);
    }

    return NoError;
}

NativeCommandRegistrations GetLibraryObjectCommandRegistrations ()
{
    return MakeRegistrationView (LibraryObjectCommandRegistrations);
}

} // namespace geomsrv
