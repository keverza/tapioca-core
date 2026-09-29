#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/DraftingPrimitiveCommands.hpp"
#include "NativeCommands/CommandUtils.hpp"
#include "NativeCommands/DraftingDatabaseTarget.hpp"

namespace geomsrv {
namespace {

// Line, arc, circle and hotspot have no creation memo (ACAPinc.h:3358-3361).
// Keep a single batch/undo boundary while each item still reports its own error.
class CreateDraftingPrimitivesCommand : public WriteCommand {
  public:
    GS::String GetName () const override { return "CreateDraftingPrimitives"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Array<GS::ObjectState> items;
        if (!params.Get ("elements", items) || items.IsEmpty ())
            return NativeCommandResult::Failure (EVP_FAIL ("need non-empty elements array", "Tapioca.CreateDraftingPrimitives"));

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
            API_ElemTypeID typeId = API_ZombieElemID;
            if (kind == "line") typeId = API_LineID;
            else if (kind == "arc") typeId = API_ArcID;
            else if (kind == "circle") typeId = API_CircleID;
            else if (kind == "hotspot") typeId = API_HotspotID;

            bool wrongField = false;
            item.EnumerateFields ([&] (const GS::String& field) {
                if (field == "kind" || field == "x" || field == "y" || field == "pen" ||
                    field == "layer" || field == "floorInd") return;
                if (kind == "line" && (field == "endX" || field == "endY")) return;
                if (kind == "arc" && (field == "radius" || field == "begAngle" || field == "endAngle")) return;
                if (kind == "circle" && field == "radius") return;
                if (kind == "hotspot" && field == "height") return;
                wrongField = true;
            });

            double x = 0.0, y = 0.0, endX = 0.0, endY = 0.0;
            double radius = 0.0, begAngle = 0.0, endAngle = 0.0;
            const bool havePosition = ReadFiniteNumber (item, "x", x) && ReadFiniteNumber (item, "y", y);
            const bool haveLine = kind != "line" || (ReadFiniteNumber (item, "endX", endX) &&
                                                      ReadFiniteNumber (item, "endY", endY) &&
                                                      (x != endX || y != endY));
            const bool haveArc = (kind != "arc" && kind != "circle") ||
                                 (ReadFiniteNumber (item, "radius", radius) && radius > 0.0 &&
                                  (kind != "arc" || (ReadFiniteNumber (item, "begAngle", begAngle) &&
                                                       ReadFiniteNumber (item, "endAngle", endAngle) &&
                                                       begAngle != endAngle)));
            if (typeId == API_ZombieElemID || wrongField || !havePosition || !haveLine || !haveArc) {
                rec.Add ("succeeded", false);
                rec.Add ("error", EVP_FAIL ("need kind=line|arc|circle|hotspot, finite x/y, only fields for that kind, and valid geometry (nonzero line or radius; distinct arc angles)", "Tapioca.CreateDraftingPrimitives"));
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
            if (item.Get ("pen", pen) && (pen < 1 || pen > 255)) {
                rec.Add ("succeeded", false);
                rec.Add ("error", EVP_FAIL ("pen must be 1..255", "Tapioca.CreateDraftingPrimitives"));
                results.Push (rec);
                continue;
            }
            if (typeId == API_LineID) {
                element.line.begC = { x, y };
                element.line.endC = { endX, endY };
                if (pen != 0) element.line.linePen.penIndex = (short) pen;
            } else if (typeId == API_HotspotID) {
                element.hotspot.pos = { x, y };
                if (pen != 0) element.hotspot.pen = (short) pen;
                if (item.Contains ("height") && !ReadFiniteNumber (item, "height", element.hotspot.height)) {
                    rec.Add ("succeeded", false);
                    rec.Add ("error", EVP_FAIL ("height must be a finite number", "Tapioca.CreateDraftingPrimitives"));
                    results.Push (rec);
                    continue;
                }
            } else {
                element.arc.origC = { x, y };
                element.arc.r = radius;
                element.arc.ratio = 1.0;
                element.arc.angle = 0.0;
                if (typeId == API_ArcID) {
                    element.arc.begAng = begAngle;
                    element.arc.endAng = endAngle;
                }
                // `whole` is output-only on create (APIdefs_Elements.h:10626).
                if (pen != 0) element.arc.linePen.penIndex = (short) pen;
            }

            const GSErrCode createError = ACAPI_Element_Create (&element, nullptr);
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
            return NativeCommandResult::Failure (EVP_FAIL ("one or more drafting primitives failed; transaction should roll back", "Tapioca.CreateDraftingPrimitives"));
        GS::ObjectState response;
        response.Add ("results", results);
        response.Add ("count", created);
        return response;
    }
};

const NativeCommandRegistration s_draftingPrimitiveRegistrations[] = {
        { "CreateDraftingPrimitives", &MakeRegisteredNativeCommand<CreateDraftingPrimitivesCommand>, false,
          R"json({"type":"object","properties":{"elements":{"type":"array","minItems":1,"items":{"type":"object","properties":{"kind":{"type":"string","enum":["line","arc","circle","hotspot"]},"x":{"type":"number"},"y":{"type":"number"},"endX":{"type":"number"},"endY":{"type":"number"},"radius":{"type":"number","exclusiveMinimum":0},"begAngle":{"type":"number"},"endAngle":{"type":"number"},"height":{"type":"number"},"pen":{"type":"integer","minimum":1,"maximum":255},"floorInd":{"type":"integer"},"layer":{"type":"string"}},"additionalProperties":false,"required":["kind","x","y"]}},"databaseAnchorElementId":{"$ref":"#ElementId"},"failOnError":{"type":"boolean"}},"additionalProperties":false,"required":["elements"]})json",
          R"json({"type":"object","properties":{"results":{"type":"array","items":{"type":"object","properties":{"kind":{"type":"string","enum":["line","arc","circle","hotspot"]},"succeeded":{"type":"boolean"},"elementId":{"$ref":"#ElementId"},"databaseId":{"$ref":"#ElementId"},"layer":{"type":"string"},"verified":{"type":"boolean"},"error":{"type":"string"}},"additionalProperties":false,"required":["kind","succeeded"]}},"count":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["results","count"]})json" },
};

} // namespace

NativeCommandRegistrations GetDraftingPrimitiveCommandRegistrations ()
{
    return MakeRegistrationView (s_draftingPrimitiveRegistrations);
}

} // namespace geomsrv
