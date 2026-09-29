#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/DraftingDimensionCommands.hpp"
#include "NativeCommands/CommandUtils.hpp"
#include "NativeCommands/DraftingDatabaseTarget.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace geomsrv {
namespace {

bool ReadPoint (const GS::ObjectState& owner, const char* key, API_Coord& point)
{
    GS::ObjectState nested;
    return owner.Get (key, nested) && ReadFiniteNumber (nested, "x", point.x) &&
           ReadFiniteNumber (nested, "y", point.y);
}

GS::ObjectState Point (const API_Coord& point)
{
    GS::ObjectState result;
    result.Add ("x", point.x);
    result.Add ("y", point.y);
    return result;
}

const char* DimensionKind (API_ElemTypeID type)
{
    switch (type) {
        case API_DimensionID: return "linear";
        case API_RadialDimensionID: return "radial";
        case API_AngleDimensionID: return "angular";
        case API_LevelDimensionID: return "level";
        default: return nullptr;
    }
}

bool BuildStaticLinear (const GS::ObjectState& item, API_Element& element,
                        API_ElementMemo& memo, GS::UniString& error)
{
    API_Coord line = {}, direction = {};
    GS::Array<GS::ObjectState> requested;
    if (!ReadPoint (item, "line", line) || !ReadPoint (item, "direction", direction) ||
        !item.Get ("points", requested) || requested.GetSize () < 2 || requested.GetSize () > 64) {
        error = "linear needs line, nonzero direction and 2..64 witness points";
        return false;
    }
    const double length = std::hypot (direction.x, direction.y);
    if (!std::isfinite (length) || length <= 0.0) {
        error = "linear direction must be nonzero";
        return false;
    }
    direction.x /= length;
    direction.y /= length;
    struct Witness { API_Coord point; double projection; };
    std::vector<Witness> witnesses;
    witnesses.reserve (requested.GetSize ());
    for (const GS::ObjectState& node : requested) {
        API_Coord point = {};
        if (!ReadFiniteNumber (node, "x", point.x) || !ReadFiniteNumber (node, "y", point.y)) {
            error = "every linear witness point needs finite x and y";
            return false;
        }
        const double projection = (point.x - line.x) * direction.x + (point.y - line.y) * direction.y;
        if (!std::isfinite (projection)) {
            error = "linear witness projection is not finite";
            return false;
        }
        witnesses.push_back ({ point, projection });
    }
    std::sort (witnesses.begin (), witnesses.end (),
               [] (const Witness& a, const Witness& b) { return a.projection < b.projection; });
    for (USize i = 1; i < witnesses.size (); ++i) {
        if (witnesses[i].projection - witnesses[i - 1].projection <= 1e-9) {
            error = "linear witness points must have distinct positions along the dimension line";
            return false;
        }
    }

    element.dimension.dimAppear = APIApp_Normal;
    element.dimension.textPos = APIPos_Above;
    element.dimension.textWay = APIDir_Parallel;
    element.dimension.defStaticDim = true;
    element.dimension.usedIn3D = false;
    element.dimension.horizontalText = false;
    element.dimension.refC = line;
    element.dimension.direction = direction;
    element.dimension.nDimElem = (Int32) witnesses.size ();

    memo.dimElems = reinterpret_cast<API_DimElem**> (
        BMhAllClear (element.dimension.nDimElem * sizeof (API_DimElem)));
    if (memo.dimElems == nullptr || *memo.dimElems == nullptr) {
        error = "out of memory allocating dimension witness points";
        return false;
    }
    for (USize i = 0; i < witnesses.size (); ++i) {
        API_DimElem& dimElem = (*memo.dimElems)[i];
        dimElem.base.loc = witnesses[i].point;
        dimElem.note = element.dimension.defNote;
        dimElem.witnessVal = element.dimension.defWitnessVal;
        dimElem.witnessForm = element.dimension.defWitnessForm;
        dimElem.fixedPos = true;
        dimElem.pos = { line.x + witnesses[i].projection * direction.x,
                        line.y + witnesses[i].projection * direction.y };
    }
    return true;
}

bool BuildStaticAngular (const GS::ObjectState& item, API_Element& element, GS::UniString& error)
{
    API_Coord origin = {}, ray1 = {}, ray2 = {};
    double radius = 0.0;
    if (!ReadPoint (item, "origin", origin) || !ReadPoint (item, "ray1", ray1) ||
        !ReadPoint (item, "ray2", ray2) || !ReadFiniteNumber (item, "radius", radius) || radius <= 0.0) {
        error = "angular needs origin, two ray endpoints and positive arc radius";
        return false;
    }
    double x1 = ray1.x - origin.x, y1 = ray1.y - origin.y;
    double x2 = ray2.x - origin.x, y2 = ray2.y - origin.y;
    const double l1 = std::hypot (x1, y1), l2 = std::hypot (x2, y2);
    if (!std::isfinite (l1) || !std::isfinite (l2) || l1 <= 0.0 || l2 <= 0.0) {
        error = "angular rays must be nonzero";
        return false;
    }
    x1 /= l1; y1 /= l1; x2 /= l2; y2 /= l2;
    const double cross = x1 * y2 - y1 * x2;
    if (std::fabs (cross) < 1e-10) {
        error = "angular rays must not be parallel or opposite";
        return false;
    }
    bool smallArc = true;
    item.Get ("smallArc", smallArc);
    const double sumX = x1 + x2, sumY = y1 + y2;
    const double bisectorLength = std::hypot (sumX, sumY);
    const double sign = smallArc ? 1.0 : -1.0;
    element.angleDimension.base[0].loc = origin;
    element.angleDimension.base[1].loc = ray1;
    element.angleDimension.base[2].loc = origin;
    element.angleDimension.base[3].loc = ray2;
    element.angleDimension.origo = origin;
    element.angleDimension.pos = { origin.x + sign * radius * sumX / bisectorLength,
                                   origin.y + sign * radius * sumY / bisectorLength };
    element.angleDimension.smallArc = smallArc;
    element.angleDimension.textPos = APIPos_Above;
    element.angleDimension.textWay = APIDir_Parallel;
    return true;
}

bool BuildStaticRadial (const GS::ObjectState& item, API_Element& element, GS::UniString& error)
{
    API_Coord base = {}, end = {};
    double radius = 0.0;
    if (!ReadPoint (item, "base", base) || !ReadPoint (item, "end", end) ||
        !ReadFiniteNumber (item, "radius", radius) || radius <= 0.0 ||
        (base.x == end.x && base.y == end.y)) {
        error = "radial needs a point on the circle, a distinct dimension-line end, and positive radius";
        return false;
    }
    // API_Base describes a NON-static association. A zeroed base leaves this
    // dimension independent; AC29 supplies no radial create example, so this
    // path needs a live placement/readback probe before relying on it in scripts.
    element.radialDimension.base.base.type = API_ZombieElemID;
    element.radialDimension.base.loc = base;
    element.radialDimension.endC = end;
    element.radialDimension.dimVal = radius;
    return true;
}

class CreateDraftingDimensionsCommand : public WriteCommand {
  public:
    GS::String GetName () const override { return "CreateDraftingDimensions"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Array<GS::ObjectState> items;
        if (!params.Get ("dimensions", items) || items.IsEmpty ())
            return NativeCommandResult::Failure (EVP_FAIL ("need non-empty dimensions array", "Tapioca.CreateDraftingDimensions"));

        GS::UniString targetDatabaseGuid, databaseError;
        AnchoredWorksheetDatabase database;
        if (!database.Activate (params, targetDatabaseGuid, databaseError))
            return NativeCommandResult::Failure (databaseError);

        GS::Array<GS::ObjectState> results;
        GS::Int32 created = 0;
        for (const GS::ObjectState& item : items) {
            GS::ObjectState rec;
            GS::UniString kind;
            item.Get ("kind", kind);
            rec.Add ("kind", kind);
            const API_ElemTypeID typeId = kind == "linear" ? API_DimensionID
                : kind == "angular" ? API_AngleDimensionID
                : kind == "radial" ? API_RadialDimensionID : API_ZombieElemID;
            bool wrongField = false;
            item.EnumerateFields ([&] (const GS::String& field) {
                if (field == "kind" || field == "pen" || field == "layer" || field == "floorInd") return;
                if (kind == "linear" && (field == "line" || field == "direction" || field == "points")) return;
                if (kind == "angular" && (field == "origin" || field == "ray1" || field == "ray2" ||
                                           field == "radius" || field == "smallArc")) return;
                if (kind == "radial" && (field == "base" || field == "end" || field == "radius")) return;
                wrongField = true;
            });
            if (typeId == API_ZombieElemID || wrongField) {
                rec.Add ("succeeded", false);
                rec.Add ("error", EVP_FAIL ("kind must be linear|radial|angular, with only that kind's fields", "Tapioca.CreateDraftingDimensions"));
                results.Push (rec);
                continue;
            }

            API_Element element = {};
            element.header.type = typeId;
            const GSErrCode defaultsError = ACAPI_Element_GetDefaults (&element, nullptr);
            GS::UniString layerError;
            if (defaultsError != NoError || !ResolveLayerParam (item, element.header, layerError)) {
                rec.Add ("succeeded", false);
                rec.Add ("error", defaultsError != NoError
                    ? EVP_ACAPI_FAIL ("ACAPI_Element_GetDefaults", defaultsError, kind) : layerError);
                results.Push (rec);
                continue;
            }
            GS::Int32 floorInd = 0;
            if (item.Get ("floorInd", floorInd)) element.header.floorInd = (short) floorInd;
            GS::Int32 pen = 0;
            if (item.Contains ("pen") && (!item.Get ("pen", pen) || pen < 1 || pen > 255)) {
                rec.Add ("succeeded", false);
                rec.Add ("error", EVP_FAIL ("pen must be 1..255", "Tapioca.CreateDraftingDimensions"));
                results.Push (rec);
                continue;
            }
            API_ElementMemo memo = {};
            GS::UniString geometryError;
            const bool valid = typeId == API_DimensionID ? BuildStaticLinear (item, element, memo, geometryError)
                : typeId == API_AngleDimensionID ? BuildStaticAngular (item, element, geometryError)
                : BuildStaticRadial (item, element, geometryError);
            if (!valid) {
                ACAPI_DisposeElemMemoHdls (&memo);
                rec.Add ("succeeded", false);
                rec.Add ("error", EVP_FAIL (geometryError, "Tapioca.CreateDraftingDimensions"));
                results.Push (rec);
                continue;
            }
            if (pen != 0) {
                if (typeId == API_DimensionID) element.dimension.linPen = (short) pen;
                else if (typeId == API_RadialDimensionID) element.radialDimension.linPen = (short) pen;
                else element.angleDimension.linPen = (short) pen;
            }
            const GSErrCode createError = ACAPI_Element_Create (&element, typeId == API_DimensionID ? &memo : nullptr);
            ACAPI_DisposeElemMemoHdls (&memo);
            if (createError != NoError) {
                rec.Add ("succeeded", false);
                rec.Add ("error", EVP_ACAPI_FAIL ("ACAPI_Element_Create", createError, kind));
            } else {
                GS::ObjectState elementId;
                elementId.Add ("guid", GS::UniString (APIGuidToString (element.header.guid).ToCStr ()));
                rec.Add ("succeeded", true);
                rec.Add ("elementId", elementId);
                GS::UniString verificationError;
                if (!VerifyCreatedDraftingElement (element.header.guid, typeId, targetDatabaseGuid, rec, verificationError))
                    return NativeCommandResult::Failure (verificationError);
                ++created;
            }
            results.Push (rec);
        }
        bool failOnError = false;
        params.Get ("failOnError", failOnError);
        if (failOnError && created != (GS::Int32) items.GetSize ())
            return NativeCommandResult::Failure (EVP_FAIL ("one or more dimensions failed; transaction should roll back", "Tapioca.CreateDraftingDimensions"));
        GS::ObjectState response;
        response.Add ("results", results);
        response.Add ("count", created);
        return response;
    }
};

class GetDraftingDimensionsCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override { return "GetDraftingDimensions"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Array<GS::ObjectState> requested;
        const bool haveElements = params.Get ("elements", requested);
        GS::UniString scope ("database");
        params.Get ("scope", scope);
        if (scope != "database" && scope != "selection")
            return NativeCommandResult::Failure (EVP_FAIL ("scope must be database or selection", "Tapioca.GetDraftingDimensions"));
        GS::Array<API_Guid> guids;
        if (haveElements) {
            scope = "elements";
            for (const GS::ObjectState& item : requested) {
                GS::ObjectState elementId;
                GS::UniString guid;
                if (!item.Get ("elementId", elementId) || !elementId.Get ("guid", guid) || guid.IsEmpty ())
                    return NativeCommandResult::Failure (EVP_FAIL ("each element needs elementId.guid", "Tapioca.GetDraftingDimensions"));
                guids.Push (APIGuidFromString (guid.ToCStr ().Get ()));
            }
        } else if (scope == "selection") {
            API_SelectionInfo selectionInfo = {};
            GS::Array<API_Neig> neigs;
            const GSErrCode selectionError = ACAPI_Selection_Get (&selectionInfo, &neigs, false);
            if (selectionInfo.marquee.coords != nullptr)
                BMKillHandle (reinterpret_cast<GSHandle*> (&selectionInfo.marquee.coords));
            if (selectionError != NoError && selectionError != APIERR_NOSEL)
                return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Selection_Get", selectionError, "dimension selection"));
            for (const API_Neig& neig : neigs) guids.Push (neig.guid);
        } else {
            for (const API_ElemTypeID typeId : { API_DimensionID, API_RadialDimensionID,
                                                 API_AngleDimensionID, API_LevelDimensionID }) {
                GS::Array<API_Guid> found;
                const GSErrCode listError = ACAPI_Element_GetElemList (typeId, &found);
                if (listError != NoError)
                    return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Element_GetElemList", listError, "dimensions"));
                for (const API_Guid& guid : found) guids.Push (guid);
            }
        }

        GS::Array<GS::ObjectState> records;
        GS::Int32 skipped = 0;
        for (const API_Guid& guid : guids) {
            API_Element element = {};
            element.header.guid = guid;
            const GSErrCode getError = ACAPI_Element_Get (&element);
            if (getError != NoError || DimensionKind (element.header.type.typeID) == nullptr) {
                ++skipped;
                continue;
            }
            const API_ElemTypeID typeId = element.header.type.typeID;
            GS::ObjectState record, elementId;
            elementId.Add ("guid", GS::UniString (APIGuidToString (guid).ToCStr ()));
            record.Add ("elementId", elementId);
            record.Add ("kind", GS::UniString (DimensionKind (typeId)));
            record.Add ("floorInd", (GS::Int32) element.header.floorInd);
            record.Add ("layer", AttributeIndexToName (API_LayerID, element.header.layer));
            if (typeId == API_DimensionID) {
                record.Add ("pen", (GS::Int32) element.dimension.linPen);
                record.Add ("line", Point (element.dimension.refC));
                record.Add ("direction", Point (element.dimension.direction));
                record.Add ("isStatic", element.dimension.defStaticDim);
                record.Add ("nDimElem", (GS::Int32) element.dimension.nDimElem);
                API_ElementMemo memo = {};
                const GSErrCode memoError = ACAPI_Element_GetMemo (guid, &memo);
                bool memoRead = memoError == NoError && memo.dimElems != nullptr && *memo.dimElems != nullptr;
                GS::Array<GS::ObjectState> points;
                if (memoRead) {
                    const USize available = BMGetHandleSize (reinterpret_cast<GSHandle> (memo.dimElems)) / sizeof (API_DimElem);
                    const USize count = std::min ((USize) std::max (0, element.dimension.nDimElem), available);
                    memoRead = available >= (USize) std::max (0, element.dimension.nDimElem);
                    for (USize i = 0; i < count; ++i)
                        points.Push (Point ((*memo.dimElems)[i].base.loc));
                }
                ACAPI_DisposeElemMemoHdls (&memo);
                record.Add ("memoRead", memoRead);
                record.Add ("points", points);
            } else if (typeId == API_RadialDimensionID) {
                record.Add ("pen", (GS::Int32) element.radialDimension.linPen);
                record.Add ("base", Point (element.radialDimension.base.loc));
                record.Add ("end", Point (element.radialDimension.endC));
                record.Add ("radius", element.radialDimension.dimVal);
                record.Add ("showOrigin", element.radialDimension.showOrigo);
            } else if (typeId == API_AngleDimensionID) {
                record.Add ("pen", (GS::Int32) element.angleDimension.linPen);
                record.Add ("origin", Point (element.angleDimension.origo));
                record.Add ("position", Point (element.angleDimension.pos));
                GS::Array<GS::ObjectState> bases;
                for (int i = 0; i < 4; ++i) bases.Push (Point (element.angleDimension.base[i].loc));
                record.Add ("bases", bases);
                record.Add ("angleValue", element.angleDimension.dimVal);
                record.Add ("smallArc", element.angleDimension.smallArc);
            } else {
                record.Add ("pen", (GS::Int32) element.levelDimension.pen);
                record.Add ("x", element.levelDimension.loc.x);
                record.Add ("y", element.levelDimension.loc.y);
                record.Add ("level", element.levelDimension.level);
                record.Add ("isStatic", element.levelDimension.staticLevel);
                record.Add ("markerSize", element.levelDimension.markerSize);
                record.Add ("angle", element.levelDimension.angle);
                record.Add ("parentGuid", GS::UniString (APIGuidToString (element.levelDimension.parentGuid).ToCStr ()));
            }
            records.Push (record);
        }
        GS::ObjectState response;
        response.Add ("scope", scope);
        response.Add ("dimensions", records);
        response.Add ("count", (GS::Int32) records.GetSize ());
        response.Add ("skipped", skipped);
        return response;
    }
};

class SetDraftingDimensionStyleCommand : public WriteCommand {
  public:
    GS::String GetName () const override { return "SetDraftingDimensionStyle"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Array<GS::ObjectState> edits;
        if (!params.Get ("edits", edits) || edits.IsEmpty ())
            return NativeCommandResult::Failure (EVP_FAIL ("need non-empty edits array", "Tapioca.SetDraftingDimensionStyle"));
        GS::Array<GS::ObjectState> results;
        GS::Int32 changed = 0;
        for (const GS::ObjectState& edit : edits) {
            GS::ObjectState rec, elementId;
            GS::UniString guidString;
            if (!edit.Get ("elementId", elementId) || !elementId.Get ("guid", guidString) || guidString.IsEmpty ()) {
                rec.Add ("succeeded", false);
                rec.Add ("error", EVP_FAIL ("edit needs elementId.guid", "Tapioca.SetDraftingDimensionStyle"));
                results.Push (rec);
                continue;
            }
            rec.Add ("elementId", elementId);
            API_Element element = {};
            element.header.guid = APIGuidFromString (guidString.ToCStr ().Get ());
            const GSErrCode getError = ACAPI_Element_Get (&element);
            const char* kind = getError == NoError ? DimensionKind (element.header.type.typeID) : nullptr;
            if (kind == nullptr) {
                rec.Add ("succeeded", false);
                rec.Add ("error", EVP_FAIL ("dimension not found or unsupported type", "Tapioca.SetDraftingDimensionStyle"));
                results.Push (rec);
                continue;
            }
            rec.Add ("kind", GS::UniString (kind));
            const API_ElemTypeID typeId = element.header.type.typeID;
            bool wrongField = false;
            edit.EnumerateFields ([&] (const GS::String& field) {
                if (field == "elementId" || field == "pen") return;
                if (typeId == API_RadialDimensionID && field == "showOrigin") return;
                if (typeId == API_AngleDimensionID && field == "smallArc") return;
                if (typeId == API_LevelDimensionID && field == "markerSize") return;
                wrongField = true;
            });
            GS::Int32 pen = 0;
            const bool hasPen = edit.Contains ("pen");
            bool flag = false;
            const char* flagName = typeId == API_RadialDimensionID ? "showOrigin" : "smallArc";
            const bool hasFlag = (typeId == API_RadialDimensionID || typeId == API_AngleDimensionID) && edit.Contains (flagName);
            double markerSize = 0.0;
            const bool hasMarker = typeId == API_LevelDimensionID && edit.Contains ("markerSize");
            if (wrongField || (!hasPen && !hasFlag && !hasMarker) ||
                (hasPen && (!edit.Get ("pen", pen) || pen < 1 || pen > 255)) ||
                (hasFlag && !edit.Get (flagName, flag)) ||
                (hasMarker && (!ReadFiniteNumber (edit, "markerSize", markerSize) || markerSize <= 0.0))) {
                rec.Add ("succeeded", false);
                rec.Add ("error", EVP_FAIL ("invalid/unsupported style fields for this dimension kind", "Tapioca.SetDraftingDimensionStyle"));
                results.Push (rec);
                continue;
            }
            API_Element mask;
            ACAPI_ELEMENT_MASK_CLEAR (mask);
            if (typeId == API_DimensionID) {
                if (hasPen) {
                    element.dimension.linPen = (short) pen;
                    ACAPI_ELEMENT_MASK_SET (mask, API_DimensionType, linPen);
                }
            } else if (typeId == API_RadialDimensionID) {
                if (hasPen) {
                    element.radialDimension.linPen = (short) pen;
                    ACAPI_ELEMENT_MASK_SET (mask, API_RadialDimensionType, linPen);
                }
                if (hasFlag) {
                    element.radialDimension.showOrigo = flag;
                    ACAPI_ELEMENT_MASK_SET (mask, API_RadialDimensionType, showOrigo);
                }
            } else if (typeId == API_AngleDimensionID) {
                if (hasPen) {
                    element.angleDimension.linPen = (short) pen;
                    ACAPI_ELEMENT_MASK_SET (mask, API_AngleDimensionType, linPen);
                }
                if (hasFlag) {
                    element.angleDimension.smallArc = flag;
                    ACAPI_ELEMENT_MASK_SET (mask, API_AngleDimensionType, smallArc);
                }
            } else {
                if (hasPen) {
                    element.levelDimension.pen = (short) pen;
                    ACAPI_ELEMENT_MASK_SET (mask, API_LevelDimensionType, pen);
                }
                if (hasMarker) {
                    element.levelDimension.markerSize = markerSize;
                    ACAPI_ELEMENT_MASK_SET (mask, API_LevelDimensionType, markerSize);
                }
            }
            const GSErrCode changeError = ACAPI_Element_Change (&element, &mask, nullptr, 0, true);
            rec.Add ("succeeded", changeError == NoError);
            if (changeError == NoError) ++changed;
            else rec.Add ("error", EVP_ACAPI_FAIL ("ACAPI_Element_Change", changeError, guidString));
            results.Push (rec);
        }
        bool failOnError = false;
        params.Get ("failOnError", failOnError);
        if (failOnError && changed != (GS::Int32) edits.GetSize ())
            return NativeCommandResult::Failure (EVP_FAIL ("one or more dimension styles failed; transaction should roll back", "Tapioca.SetDraftingDimensionStyle"));
        GS::ObjectState response;
        response.Add ("results", results);
        response.Add ("count", (GS::Int32) results.GetSize ());
        response.Add ("changed", changed);
        return response;
    }
};

const NativeCommandRegistration s_draftingDimensionRegistrations[] = {
    { "CreateDraftingDimensions", &MakeRegisteredNativeCommand<CreateDraftingDimensionsCommand>, false,
      R"json({"type":"object","properties":{"dimensions":{"type":"array","minItems":1,"items":{"type":"object","properties":{"kind":{"type":"string","enum":["linear","radial","angular"]},"line":{"$ref":"#Point2D"},"direction":{"$ref":"#Point2D"},"points":{"type":"array","minItems":2,"maxItems":64,"items":{"$ref":"#Point2D"}},"base":{"$ref":"#Point2D"},"end":{"$ref":"#Point2D"},"origin":{"$ref":"#Point2D"},"ray1":{"$ref":"#Point2D"},"ray2":{"$ref":"#Point2D"},"radius":{"type":"number","exclusiveMinimum":0},"smallArc":{"type":"boolean"},"pen":{"type":"integer","minimum":1,"maximum":255},"layer":{"type":"string"},"floorInd":{"type":"integer"}},"additionalProperties":false,"required":["kind"]}},"databaseAnchorElementId":{"$ref":"#ElementId"},"failOnError":{"type":"boolean"}},"additionalProperties":false,"required":["dimensions"]})json",
      R"json({"type":"object","properties":{"results":{"type":"array","items":{"type":"object","properties":{"kind":{"type":"string","enum":["linear","radial","angular"]},"succeeded":{"type":"boolean"},"elementId":{"$ref":"#ElementId"},"databaseId":{"$ref":"#ElementId"},"layer":{"type":"string"},"verified":{"type":"boolean"},"error":{"type":"string"}},"additionalProperties":false,"required":["kind","succeeded"]}},"count":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["results","count"]})json" },
    { "GetDraftingDimensions", &MakeRegisteredNativeCommand<GetDraftingDimensionsCommand>, false,
      R"json({"type":"object","properties":{"elements":{"$ref":"#Elements"},"scope":{"type":"string","enum":["database","selection"]}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"scope":{"type":"string","enum":["database","selection","elements"]},"dimensions":{"type":"array","items":{"type":"object","properties":{"elementId":{"$ref":"#ElementId"},"kind":{"type":"string","enum":["linear","radial","angular","level"]},"floorInd":{"type":"integer"},"layer":{"type":"string"},"pen":{"type":"integer"},"line":{"$ref":"#Point2D"},"direction":{"$ref":"#Point2D"},"points":{"type":"array","items":{"$ref":"#Point2D"}},"nDimElem":{"type":"integer"},"memoRead":{"type":"boolean"},"base":{"$ref":"#Point2D"},"end":{"$ref":"#Point2D"},"radius":{"type":"number"},"showOrigin":{"type":"boolean"},"origin":{"$ref":"#Point2D"},"position":{"$ref":"#Point2D"},"bases":{"type":"array","items":{"$ref":"#Point2D"}},"angleValue":{"type":"number"},"smallArc":{"type":"boolean"},"x":{"type":"number"},"y":{"type":"number"},"level":{"type":"number"},"isStatic":{"type":"boolean"},"markerSize":{"type":"number"},"angle":{"type":"number"},"parentGuid":{"type":"string"}},"additionalProperties":false,"required":["elementId","kind","floorInd","layer","pen"]}},"count":{"type":"integer","minimum":0},"skipped":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["scope","dimensions","count","skipped"]})json" },
    { "SetDraftingDimensionStyle", &MakeRegisteredNativeCommand<SetDraftingDimensionStyleCommand>, false,
      R"json({"type":"object","properties":{"edits":{"type":"array","minItems":1,"items":{"type":"object","properties":{"elementId":{"$ref":"#ElementId"},"pen":{"type":"integer","minimum":1,"maximum":255},"showOrigin":{"type":"boolean"},"smallArc":{"type":"boolean"},"markerSize":{"type":"number","exclusiveMinimum":0}},"additionalProperties":false,"required":["elementId"],"minProperties":2}},"failOnError":{"type":"boolean"}},"additionalProperties":false,"required":["edits"]})json",
      R"json({"type":"object","properties":{"results":{"type":"array","items":{"type":"object","properties":{"elementId":{"$ref":"#ElementId"},"kind":{"type":"string","enum":["linear","radial","angular","level"]},"succeeded":{"type":"boolean"},"error":{"type":"string"}},"additionalProperties":false,"required":["succeeded"]}},"count":{"type":"integer","minimum":0},"changed":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["results","count","changed"]})json" },
};

} // namespace

NativeCommandRegistrations GetDraftingDimensionCommandRegistrations ()
{
    return MakeRegistrationView (s_draftingDimensionRegistrations);
}

} // namespace geomsrv
