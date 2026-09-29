#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/ElementModifyCommands.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/CommandUtils.hpp"

namespace geomsrv {

namespace {

// ---------------------------------------------------------------------------
// Tapioca.SetElementDetails { edits: [ { elementId:{guid}, details:{...} }, ... ] }
//   -> { count, changed, results: [ { elementId, succeeded, kind, applied, error? } ] }
//
// The symmetric write for EvP.GetElementDetails (E16 Path 3). Same guid, same
// `kind` vocabulary, same field SPELLINGS — read a record, change a value, send
// the changed value back. It deliberately replaces the per-field
// `SetElementLevelOffset` that E10 asked for, because "level" is ONE semantic
// spread over five struct members (slab.level, roof.shellBase.level,
// wall.bottomOffset, beam.level, column.bottomOffset, mesh.level,
// object.level). One command owns that mapping; every future scalar tweak is a
// row in the table below rather than a new command.
//
// THE CONTRACT — SPARSE, NOT A WHOLE RECORD. `details` carries ONLY the fields
// the caller changed. Two reasons, both learned from Path 0–2:
//
//   * A read record emits BOTH halves of a union, zeroed on the side that does
//     not apply (§E16.5b: a poly roof still reports a `baseLine` of zeros).
//     Echoing a whole record back would push those zeros in as a real pivot
//     line. Sparse makes that structurally impossible.
//   * Much of the record is a FACT ABOUT the element, not a setting —
//     `roofClass`, `nSegments`, `libraryPartName`, the polygon. Sending one is
//     a caller bug, so it is REJECTED with the field named, never silently
//     dropped. A silently-ignored field is the same class of bug as the
//     JSON-int one (§E16.5a) — the caller believes it wrote something.
//
// WHAT IS WRITABLE — the table in WritableFields below, and nothing else. Most
// entries are scalar settings; line/arc/circle/hotspot also allow direct geometry
// fields per ACAPinc.h:3476-3480. Polygon/memo geometry remains out of scope.
// Deliberately NOT writable, so the reasons are recorded rather than rediscovered:
//
//   * polygon / memo geometry (`polygonOutline`, `holes`, `polygonZ`,
//     `meshSublines`, `pivotOutline`, `levels`) — a memo rewrite, not a field
//     poke. It needs ACAPI_Element_GetMemo + a memoMask and is its own job.
//   * `slantAngle` on wall / beam / column — the slant is not an independent
//     scalar there; it is coupled to `isSlanted` and to the begC/endC pair, so
//     writing the angle alone half-writes the element. Roof IS writable
//     (§7.9) because a PLANE roof's pitch genuinely is one number.
//   * `roofClass`, `nSegments`, `isSlanted`, `sectionWidth/Height`,
//     `libraryPartName`, `hasHoles`, `skirtType`, `ridges` — facts, enums or
//     derived values, not single-scalar settings.
//
// ⚠️ JSON INTEGERS. `Get(field, double&)` returns FALSE for a JSON integer
// (§E16.5a — the trap that returned 0.0 from coordinate arrays). Read-modify-
// write walks straight into it, because the response serializer emits a whole
// number as a JSON int: read `level: 3`, send `level: 3` back, and a naive read
// would report "field absent" and write nothing. ReadFiniteNumber below asks
// IsReal/IsInt/IsUInt first, so an int is accepted and converted. This closes
// the trap for SCALARS here; the ARRAY case (§E16.6 open item 1) is untouched
// and still needs its probe.
//
// WriteCommand — NO undo scope of its own (CommandBase.hpp). A pull-to-mesh run
// puts the whole batch in one evp.transaction and the user gets one undo.
// Per-element results rather than all-or-nothing, exactly as SetElementIds: a
// mixed selection is the normal input, and a caller that wants atomicity has
// evp.transaction for it.
// ---------------------------------------------------------------------------

// Set one real member and mark it in the mask. The mask macro needs the member
// name as a LITERAL relative to the type struct, so the union member path
// (`element.slab.level`) and the mask path (`API_SlabType, level`) are spelled
// separately — that is why this is a macro and not a function.
#define EVP_WRITE_REAL(jsonName, unionMember, TypeName, member)                       \
    do {                                                                              \
        if (details.Contains (jsonName)) {                                            \
            double value = 0.0;                                                       \
            if (!ReadFiniteNumber (details, jsonName, value)) {                              \
                badValue.Push (GS::UniString (jsonName));                              \
            } else {                                                                  \
                element.unionMember.member = value;                                    \
                ACAPI_ELEMENT_MASK_SET (mask, TypeName, member);                       \
                applied.Push (GS::UniString (jsonName));                               \
            }                                                                         \
        }                                                                             \
    } while (0)

#define EVP_WRITE_BOOL(jsonName, unionMember, TypeName, member)                       \
    do {                                                                              \
        if (details.Contains (jsonName)) {                                            \
            bool value = false;                                                       \
            if (!details.Get (jsonName, value)) {                                      \
                badValue.Push (GS::UniString (jsonName));                              \
            } else {                                                                  \
                element.unionMember.member = value;                                    \
                ACAPI_ELEMENT_MASK_SET (mask, TypeName, member);                       \
                applied.Push (GS::UniString (jsonName));                               \
            }                                                                         \
        }                                                                             \
    } while (0)

// The writable set, per kind. This IS the command's contract — a field absent
// here is rejected by name, so adding a settable scalar means one row here and
// one EVP_WRITE_* line in the matching branch, nothing else.
//
// Spellings are GetElementDetails' own. If a name here ever disagrees with the
// read, the pair has stopped being symmetric and the caller's read-modify-write
// silently stops working — that is the one invariant worth checking by hand.
static const char* const* WritableFields (const GS::UniString& kind, USize& count)
{
    static const char* const slabFields[]   = { "level", "thickness" };
    static const char* const roofFields[]   = { "level", "thickness", "slantAngle" };
    static const char* const meshFields[]   = { "level", "skirtLevel" };
    static const char* const wallFields[]   = { "level", "thickness", "height" };
    static const char* const beamFields[]   = { "level" };
    static const char* const columnFields[] = { "level", "height", "planAngle" };
    static const char* const objectFields[] = { "level", "planAngle", "xRatio", "yRatio", "reflected" };
    static const char* const lineFields[] = { "begCoordinate", "endCoordinate", "pen" };
    static const char* const arcFields[] = { "x", "y", "radius", "begAngle", "endAngle", "ratio", "angle", "pen" };
    static const char* const hotspotFields[] = { "x", "y", "height", "pen" };
    static const char* const fillFields[] = { "fill", "pen", "fillPen", "fillBGPen" };

    if (kind == "slab")   { count = 2; return slabFields;   }
    if (kind == "roof")   { count = 3; return roofFields;   }
    if (kind == "mesh")   { count = 2; return meshFields;   }
    if (kind == "wall")   { count = 3; return wallFields;   }
    if (kind == "beam")   { count = 1; return beamFields;   }
    if (kind == "column") { count = 3; return columnFields; }
    if (kind == "object" || kind == "lamp") { count = 5; return objectFields; }
    if (kind == "line") { count = 3; return lineFields; }
    if (kind == "arc" || kind == "circle") { count = 8; return arcFields; }
    if (kind == "hotspot") { count = 4; return hotspotFields; }
    if (kind == "fill") { count = 4; return fillFields; }

    count = 0;              // polyline — read-only here; polygon geometry needs a memo rewrite
    return nullptr;
}

class SetElementDetailsCommand : public WriteCommand {
public:
    GS::String GetName () const override { return "SetElementDetails"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Array<GS::ObjectState> edits;
        if (!params.Get ("edits", edits))
            return NativeCommandResult::Failure (EVP_FAIL ("need edits=[{elementId:{guid},details:{...}}]", "Tapioca.SetElementDetails"));

        GS::ObjectState os;

        GS::Array<GS::ObjectState> results;
        GS::Int32 changed = 0;

        for (const GS::ObjectState& edit : edits) {
            GS::ObjectState rec;

            GS::ObjectState elementId;
            GS::UniString guidString;
            if (!edit.Get ("elementId", elementId) || !elementId.Get ("guid", guidString) || guidString.IsEmpty ()) {
                GS::ObjectState emptyElementId;
                emptyElementId.Add ("guid", GS::UniString ());
                rec.Add ("elementId", emptyElementId);
                rec.Add ("succeeded", false);
                rec.Add ("kind",  GS::UniString ());
                rec.Add ("error", EVP_FAIL ("edit has no elementId.guid", "Tapioca.SetElementDetails"));
                results.Push (rec);
                continue;
            }
            rec.Add ("elementId", elementId);

            GS::ObjectState details;
            if (!edit.Get ("details", details) || details.GetFieldCount () == 0) {
                rec.Add ("succeeded", false);
                rec.Add ("kind",  GS::UniString ());
                rec.Add ("error", EVP_FAIL ("edit has no non-empty details={…}. Send ONLY the "
                                            "fields you changed — this command is sparse, not a "
                                            "whole-record echo.", "EvP.SetElementDetails"));
                results.Push (rec);
                continue;
            }

            API_Element element = {};
            element.header.guid = APIGuidFromString (guidString.ToCStr ().Get ());
            const GSErrCode getErr = ACAPI_Element_Get (&element);
            if (getErr != NoError) {
                rec.Add ("succeeded", false);
                rec.Add ("kind",  GS::UniString ());
                rec.Add ("error", EVP_ACAPI_FAIL ("ACAPI_Element_Get", getErr,
                                                  GS::UniString ("reading " + guidString +
                                                                 " before writing its details")));
                results.Push (rec);
                continue;
            }

            // Same kind vocabulary as GetElementDetails — the caller branches on
            // one word for both halves of the round trip.
            const API_ElemTypeID typeId = element.header.type.typeID;
            GS::UniString kind;
            switch (typeId) {
                case API_SlabID:   kind = "slab";   break;
                case API_RoofID:   kind = "roof";   break;
                case API_MeshID:   kind = "mesh";   break;
                case API_WallID:   kind = "wall";   break;
                case API_BeamID:   kind = "beam";   break;
                case API_ColumnID: kind = "column"; break;
                case API_ObjectID: kind = "object"; break;
                case API_LampID:   kind = "lamp";   break;
                case API_PolyLineID: kind = "polyline"; break;
                case API_HatchID:  kind = "fill";   break;
                case API_LineID: kind = "line"; break;
                case API_ArcID: kind = "arc"; break;
                case API_CircleID: kind = "circle"; break;
                case API_HotspotID: kind = "hotspot"; break;
                default: break;
            }
            rec.Add ("kind", kind);

            if (kind.IsEmpty ()) {
                GS::UniString typeName;
                ACAPI_Element_GetElemTypeName (element.header.type, typeName);
                rec.Add ("succeeded", false);
                rec.Add ("error", EVP_FAIL (GS::UniString ("EvP.SetElementDetails does not speak \"" +
                                            typeName + "\" — GetElementDetails cannot read it either, "
                                            "so there is no record to send back."),
                                            "EvP.SetElementDetails"));
                results.Push (rec);
                continue;
            }

            // REJECT before writing anything. A field this kind cannot settle is a
            // caller bug — say which one, and touch nothing. Half-applying an edit
            // and reporting success is the failure mode this guards.
            USize writableCount = 0;
            const char* const* writable = WritableFields (kind, writableCount);

            GS::Array<GS::UniString> unwritable;
            details.EnumerateFields ([&] (const GS::String& name) {
                for (USize w = 0; w < writableCount; ++w) {
                    if (name == writable[w])
                        return;
                }
                unwritable.Push (GS::UniString (name.ToCStr ()));
            });

            if (!unwritable.IsEmpty ()) {
                GS::UniString names;
                for (UIndex n = 0; n < unwritable.GetSize (); ++n) {
                    if (n > 0) names += ", ";
                    names += unwritable[n];
                }
                GS::UniString allowed;
                for (USize w = 0; w < writableCount; ++w) {
                    if (w > 0) allowed += ", ";
                    allowed += writable[w];
                }
                if (allowed.IsEmpty ())
                    allowed = "(nothing — this kind is read-only)";

                GS::UniString why = "kind \"";
                why += kind;
                why += "\" cannot write: ";
                why += names;
                why += ". Writable here: ";
                why += allowed;
                why += ". Polygon/memo geometry, derived values and type facts are read-only "
                       "by design; send only the scalar settings you changed.";

                rec.Add ("succeeded", false);
                rec.Add ("error", EVP_FAIL (why, "EvP.SetElementDetails"));
                results.Push (rec);
                continue;
            }

            API_Element mask;
            ACAPI_ELEMENT_MASK_CLEAR (mask);

            GS::Array<GS::UniString> applied;    // fields actually staged for the write
            GS::Array<GS::UniString> badValue;   // present, but not a number/bool

            if (typeId == API_SlabID) {
                EVP_WRITE_REAL ("level",     slab, API_SlabType, level);
                EVP_WRITE_REAL ("thickness", slab, API_SlabType, thickness);

            } else if (typeId == API_RoofID) {
                EVP_WRITE_REAL ("level",     roof, API_RoofType, shellBase.level);
                EVP_WRITE_REAL ("thickness", roof, API_RoofType, shellBase.thickness);
                // §7.9. A PLANE roof's pitch is one number (u.planeRoof.angle). A
                // POLY roof's is per level (u.polyRoof.levelData), which is a
                // different edit entirely — and the read already reports 0.0 there,
                // so accepting the field would write that 0.0 back as a real pitch.
                // Branch on roofClass, never on the zero (§E16.5b).
                if (details.Contains ("slantAngle") && element.roof.roofClass != API_PlaneRoofID) {
                    rec.Add ("succeeded", false);
                    rec.Add ("error", EVP_FAIL ("slantAngle is writable on a PLANE roof only — a "
                                                "poly roof's pitch is per-level (u.polyRoof.levelData) "
                                                "and the read reports 0.0 for it, so writing this "
                                                "field back would flatten the roof.",
                                                "EvP.SetElementDetails"));
                    results.Push (rec);
                    continue;
                }
                EVP_WRITE_REAL ("slantAngle", roof, API_RoofType, u.planeRoof.angle);

            } else if (typeId == API_MeshID) {
                EVP_WRITE_REAL ("level",      mesh, API_MeshType, level);
                EVP_WRITE_REAL ("skirtLevel", mesh, API_MeshType, skirtLevel);

            } else if (typeId == API_WallID) {
                // `level` is the wall's bottomOffset — the same semantic the other
                // kinds spell `level`, which is the point of the shared spelling.
                EVP_WRITE_REAL ("level",     wall, API_WallType, bottomOffset);
                EVP_WRITE_REAL ("thickness", wall, API_WallType, thickness);
                EVP_WRITE_REAL ("height",    wall, API_WallType, height);

            } else if (typeId == API_BeamID) {
                // ⚠️ beam.level is the beam's TOP relative to the story level
                // (APIdefs_Elements.h, API_BeamType::level), not its underside. The
                // read reports the same field, so a round trip is consistent — but a
                // caller computing "rest this beam ON a surface" must subtract the
                // section height itself.
                EVP_WRITE_REAL ("level", beam, API_BeamType, level);

            } else if (typeId == API_ColumnID) {
                EVP_WRITE_REAL ("level",     column, API_ColumnType, bottomOffset);
                EVP_WRITE_REAL ("height",    column, API_ColumnType, height);
                EVP_WRITE_REAL ("planAngle", column, API_ColumnType, axisRotationAngle);

            } else if (typeId == API_HatchID) {
                bool flagsChanged = false;
                if (element.hatch.hatchType != API_FillHatch && !details.Contains ("fill") &&
                    (details.Contains ("fillPen") || details.Contains ("fillBGPen"))) {
                    badValue.Push ("fillPen/fillBGPen on a Building Material hatch: choose a fill attribute first");
                }
                GS::UniString fillName;
                if (details.Contains ("fill")) {
                    if (!details.Get ("fill", fillName) || fillName.IsEmpty () ||
                        !AttributeNameToIndex (API_FilltypeID, fillName, element.hatch.fillInd)) {
                        badValue.Push ("fill (unknown fill attribute name)");
                    } else {
                        element.hatch.hatchType = API_FillHatch;
                        element.hatch.hatchFlags &= ~(APIHatch_OverrideFgPen | APIHatch_OverrideBkgPen |
                                                      APIHatch_OverrideFillInd);
                        flagsChanged = true;
                        ACAPI_ELEMENT_MASK_SET (mask, API_HatchType, hatchType);
                        ACAPI_ELEMENT_MASK_SET (mask, API_HatchType, fillInd);
                        applied.Push ("fill");
                    }
                }
                for (const char* field : { "pen", "fillPen", "fillBGPen" }) {
                    if (!details.Contains (field)) continue;
                    GS::Int32 pen = 0;
                    const bool isFillPen = GS::String (field) == "fillPen";
                    if (!details.Get (field, pen) || pen < (isFillPen ? 1 : 0) || pen > 255) {
                        badValue.Push (GS::UniString (field));
                        continue;
                    }
                    if (GS::String (field) == "pen") {
                        element.hatch.contPen.penIndex = (short) pen;
                        element.hatch.contPen.colorOverridePenIndex = 0;
                        ACAPI_ELEMENT_MASK_SET (mask, API_HatchType, contPen);
                    } else if (isFillPen) {
                        element.hatch.fillPen.penIndex = (short) pen;
                        element.hatch.fillPen.colorOverridePenIndex = 0;
                        element.hatch.hatchFlags &= ~APIHatch_HasFgRGBColor;
                        flagsChanged = true;
                        ACAPI_ELEMENT_MASK_SET (mask, API_HatchType, fillPen);
                    } else {
                        element.hatch.fillBGPen = (short) pen;
                        element.hatch.hatchFlags &= ~APIHatch_HasBkgRGBColor;
                        flagsChanged = true;
                        ACAPI_ELEMENT_MASK_SET (mask, API_HatchType, fillBGPen);
                    }
                    applied.Push (GS::UniString (field));
                }
                if (flagsChanged)
                    ACAPI_ELEMENT_MASK_SET (mask, API_HatchType, hatchFlags);

            } else if (typeId == API_LineID || typeId == API_ArcID ||
                       typeId == API_CircleID || typeId == API_HotspotID) {
                // Geometry edits are supported for these types by ACAPinc.h:3476-3480.
                // Keep each coordinate pair atomic in the mask, and preserve the
                // omitted half for sparse edits to a point.
                if (typeId == API_LineID) {
                    for (const char* name : { "begCoordinate", "endCoordinate" }) {
                        if (!details.Contains (name)) continue;
                        GS::ObjectState point;
                        double x = 0.0, y = 0.0, z = 0.0;
                        if (!details.Get (name, point) || !ReadFiniteNumber (point, "x", x) ||
                            !ReadFiniteNumber (point, "y", y) ||
                            (point.Contains ("z") && (!ReadFiniteNumber (point, "z", z) || z != 0.0))) {
                            badValue.Push (GS::UniString (name));
                            continue;
                        }
                        if (GS::String (name) == "begCoordinate") {
                            element.line.begC = { x, y };
                            ACAPI_ELEMENT_MASK_SET (mask, API_LineType, begC);
                        } else {
                            element.line.endC = { x, y };
                            ACAPI_ELEMENT_MASK_SET (mask, API_LineType, endC);
                        }
                        applied.Push (GS::UniString (name));
                    }
                    if (element.line.begC.x == element.line.endC.x &&
                        element.line.begC.y == element.line.endC.y)
                        badValue.Push ("line endpoints must differ");
                } else {
                    if (typeId == API_HotspotID) {
                        double x = element.hotspot.pos.x, y = element.hotspot.pos.y;
                        if (details.Contains ("x") && !ReadFiniteNumber (details, "x", x)) badValue.Push ("x");
                        if (details.Contains ("y") && !ReadFiniteNumber (details, "y", y)) badValue.Push ("y");
                        if (details.Contains ("x") || details.Contains ("y")) {
                            element.hotspot.pos = { x, y };
                            ACAPI_ELEMENT_MASK_SET (mask, API_HotspotType, pos);
                            if (details.Contains ("x")) applied.Push ("x");
                            if (details.Contains ("y")) applied.Push ("y");
                        }
                        EVP_WRITE_REAL ("height", hotspot, API_HotspotType, height);
                    } else {
                        double x = element.arc.origC.x, y = element.arc.origC.y;
                        if (details.Contains ("x") && !ReadFiniteNumber (details, "x", x)) badValue.Push ("x");
                        if (details.Contains ("y") && !ReadFiniteNumber (details, "y", y)) badValue.Push ("y");
                        if (details.Contains ("x") || details.Contains ("y")) {
                            element.arc.origC = { x, y };
                            ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, origC);
                            if (details.Contains ("x")) applied.Push ("x");
                            if (details.Contains ("y")) applied.Push ("y");
                        }
                        EVP_WRITE_REAL ("radius", arc, API_ArcType, r);
                        EVP_WRITE_REAL ("ratio", arc, API_ArcType, ratio);
                        EVP_WRITE_REAL ("angle", arc, API_ArcType, angle);
                        if (typeId == API_ArcID) {
                            EVP_WRITE_REAL ("begAngle", arc, API_ArcType, begAng);
                            EVP_WRITE_REAL ("endAngle", arc, API_ArcType, endAng);
                        } else if (details.Contains ("begAngle") || details.Contains ("endAngle")) {
                            badValue.Push ("circle angles (use an arc element)");
                        }
                        if ((details.Contains ("radius") && element.arc.r <= 0.0) ||
                            (details.Contains ("ratio") && element.arc.ratio <= 0.0))
                            badValue.Push ("radius/ratio must be positive");
                    }
                }
                if (details.Contains ("pen")) {
                    GS::Int32 pen = 0;
                    if (!details.Get ("pen", pen) || pen < 1 || pen > 255) {
                        badValue.Push ("pen");
                    } else {
                        if (typeId == API_HotspotID) {
                            element.hotspot.pen = (short) pen;
                            ACAPI_ELEMENT_MASK_SET (mask, API_HotspotType, pen);
                        } else if (typeId == API_LineID) {
                            element.line.linePen.penIndex = (short) pen;
                            ACAPI_ELEMENT_MASK_SET (mask, API_LineType, linePen.penIndex);
                        } else {
                            element.arc.linePen.penIndex = (short) pen;
                            ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, linePen.penIndex);
                        }
                        applied.Push ("pen");
                    }
                }

            } else {   // API_ObjectID / API_LampID — API_LampType IS API_ObjectType
                EVP_WRITE_REAL ("level",     object, API_ObjectType, level);
                EVP_WRITE_REAL ("planAngle", object, API_ObjectType, angle);
                // A/B size. Only takes effect for a library part that honours the
                // fixed sizes (object.useXYFixSize); for one that does not, Archicad
                // reports no error and the part keeps its own size.
                EVP_WRITE_REAL ("xRatio",    object, API_ObjectType, xRatio);
                EVP_WRITE_REAL ("yRatio",    object, API_ObjectType, yRatio);
                EVP_WRITE_BOOL ("reflected", object, API_ObjectType, reflected);
            }

            if (!badValue.IsEmpty ()) {
                GS::UniString names;
                for (UIndex n = 0; n < badValue.GetSize (); ++n) {
                    if (n > 0) names += ", ";
                    names += badValue[n];
                }
                GS::UniString why = "not a usable value for: ";
                why += names;
                why += ". Numbers may be JSON int or real; `reflected` must be a bool.";
                rec.Add ("succeeded", false);
                rec.Add ("error", EVP_FAIL (why, "EvP.SetElementDetails"));
                results.Push (rec);
                continue;
            }

            // NO undo scope here — see WriteCommand. The caller has one open.
            const GSErrCode err = ACAPI_Element_Change (&element, &mask, nullptr, 0, true);
            if (err == NoError) {
                rec.Add ("succeeded", true);
                rec.Add ("applied", applied);
                ++changed;
            } else {
                rec.Add ("succeeded", false);
                rec.Add ("applied", GS::Array<GS::UniString> ());
                rec.Add ("error",   EVP_ACAPI_FAIL ("ACAPI_Element_Change", err,
                                                    GS::UniString ("writing " + kind + " details on " +
                                                                   guidString)));
            }
            results.Push (rec);
        }

        os.Add ("results", results);
        os.Add ("count",   (GS::Int32) results.GetSize ());
        os.Add ("changed", changed);
        return os;
    }
};

#undef EVP_WRITE_REAL
#undef EVP_WRITE_BOOL

const NativeCommandRegistration kElementModifyCommandRegistrations[] = {
    { "SetElementDetails", &MakeRegisteredNativeCommand<SetElementDetailsCommand>, false,
      R"json({"type":"object","properties":{"edits":{"type":"array","items":{"type":"object","properties":{"elementId":{"$ref":"#ElementId"},"details":{"type":"object","properties":{"level":{"type":"number"},"thickness":{"type":"number"},"height":{"type":"number"},"slantAngle":{"type":"number"},"planAngle":{"type":"number"},"skirtLevel":{"type":"number"},"xRatio":{"type":"number"},"yRatio":{"type":"number"},"reflected":{"type":"boolean"},"begCoordinate":{"$ref":"#Point3D"},"endCoordinate":{"$ref":"#Point3D"},"x":{"type":"number"},"y":{"type":"number"},"radius":{"type":"number"},"begAngle":{"type":"number"},"endAngle":{"type":"number"},"ratio":{"type":"number"},"angle":{"type":"number"},"pen":{"type":"integer","minimum":0,"maximum":255},"fill":{"type":"string","minLength":1},"fillPen":{"type":"integer","minimum":1,"maximum":255},"fillBGPen":{"type":"integer","minimum":0,"maximum":255}},"additionalProperties":false,"minProperties":1}},"additionalProperties":false,"required":["elementId","details"]}}},"additionalProperties":false,"required":["edits"]})json",
      R"json({"oneOf":[{"type":"object","properties":{"results":{"type":"array","items":{"type":"object","properties":{"elementId":{"$ref":"#ElementId"},"succeeded":{"type":"boolean"},"kind":{"type":"string","enum":["","slab","roof","mesh","wall","beam","column","object","lamp","polyline","fill","line","arc","circle","hotspot"]},"applied":{"type":"array","items":{"type":"string"}},"error":{"type":"string"}},"additionalProperties":false,"required":["elementId","succeeded","kind"]}},"count":{"type":"integer","minimum":0},"changed":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["results","count","changed"]},{"type":"object","properties":{"ok":{"const":false},"error":{"type":"string"}},"additionalProperties":false,"required":["ok","error"]}]})json" }
};

}   // namespace

NativeCommandRegistrations GetElementModifyCommandRegistrations ()
{
    return MakeRegistrationView (kElementModifyCommandRegistrations);
}

} // namespace geomsrv
