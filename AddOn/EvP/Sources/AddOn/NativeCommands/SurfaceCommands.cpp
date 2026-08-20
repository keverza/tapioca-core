#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/SurfaceCommands.hpp"
#include "NativeCommands/CommandBase.hpp"

namespace geomsrv {

namespace {


// ---------------------------------------------------------------------------
// Tapioca.SetElementSurface paints/clears typed `elements`, or restores named
// per-element `restores` records returned by an earlier call.
//
// Element-level surface (material) override — paints selected elements a named
// surface in 3D (MassingFeasibility marks over-size slabs red), or reverts it.
// This is the destructive per-element
// override (API_OverriddenAttribute on the element itself), NOT the
// ACAPI_GraphicalOverride_* rule API — rules select by criterion XML, not a
// GUID list, which is the wrong tool for "these N elements" (see §7.4).
//
// Supported types: SLAB (slab.{topMat,sideMat,botMat}) and WALL
// (wall.{refMat,oppMat,sidMat}) — both carry a clean top-level triple of
// API_OverriddenAttribute (APIdefs_Elements.h:5823/5829/5835, 2008/2014/2020).
// Roof/beam/column store surfaces in nested shellBase / per-segment structs and
// are reported `unsupported` here rather than half-written; slab is all
// MassingFeasibility needs and wall proves the mechanism generalises.
//
// Three ops (priority: restore > clear > paint):
//   paint    surface="<name>" -> all three mats to that surface.
//   clear    clear:true       -> all three mats to APINullValue (default surface).
//   restore  restores records -> write explicit per-element triples back (-1 =
//                               no override). This makes a round-trip non-destructive.
//
// EVERY result returns the state found BEFORE writing as a named `previous`
// record. A miss remains positionally represented with sentinel values and a
// false `succeeded`. WriteCommand opens no undo scope; the dispatcher owns it.
//
// Write pattern: ACAPI_Element_Get -> set fields -> mask the changed members ->
// ACAPI_Element_Change(&elem,&mask,nullptr,0,true). The confirmed edit pattern
// (Examples/Element_Test/Src/Element_Modify_2DElements.cpp:48-54).
// ---------------------------------------------------------------------------

// Resolve a surface (material) attribute by name -> its index. Mirror of
// CreateCommands.cpp -> LayerNameToIndex but on API_MaterialID (surfaces are
// "materials" in the API, APIdefs_Attributes.h:69). Does NOT create — an absent
// name is a caller error.
static bool SurfaceNameToIndex (const GS::UniString& name, API_AttributeIndex& index)
{
    GS::Array<API_Attribute> surfaces;
    if (ACAPI_Attribute_GetAttributesByType (API_MaterialID, surfaces) != NoError)
        return false;
    for (const API_Attribute& surface : surfaces) {
        if (GS::UniString (surface.header.name) == name) {
            index = surface.header.index;
            return true;
        }
    }
    return false;
}

class SetElementSurfaceCommand : public WriteCommand {
public:
    GS::String GetName () const override { return "SetElementSurface"; }

    // Sentinel for "this mat had no override" in previous/restore records. A
    // real surface index is always >= 1, so -1 can never collide with one.
    static constexpr GS::Int32 NoOverride = -1;

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Array<GS::ObjectState> elements, restores;
        const bool restore = params.Get ("restores", restores);
        if ((!restore && (!params.Get ("elements", elements) || elements.IsEmpty ())) ||
            (restore && restores.IsEmpty ())) {
            return NativeCommandResult::Failure ("need non-empty elements or restores records");
        }

        // Three operations, in priority order:
        //   restore : write an explicit per-element per-mat record back.
        //   clear   : every mat -> APINullValue (element's default surface).
        //   paint   : every mat -> one named surface.
        // Whatever the op, we ALWAYS return the state we found BEFORE writing, so the
        // caller can restore it later.
        bool clear = false;
        params.Get ("clear", clear);

        // Resolve the surface up-front (once) only for a paint. A bad name is a hard
        // error before we touch any element — no partial paint.
        API_AttributeIndex surfaceIdx;
        if (!restore && !clear) {
            GS::UniString surfaceName;
            if (!params.Get ("surface", surfaceName) || surfaceName.IsEmpty ())
                return NativeCommandResult::Failure ("need surface=\"<name>\" (or clear:true, or restores)");
            if (!SurfaceNameToIndex (surfaceName, surfaceIdx)) {
                return NativeCommandResult::Failure (GS::UniString ("no surface named \"" + surfaceName +
                        "\". Use the exact name from the Surfaces attribute list; this "
                        "command does NOT create attributes."));
            }
        }

        GS::Array<GS::ObjectState> results;
        const GS::Array<GS::ObjectState>& targets = restore ? restores : elements;

        for (const GS::ObjectState& target : targets) {
            GS::ObjectState elementId;
            GS::UniString guidString;
            if (!target.Get ("elementId", elementId) || !elementId.Get ("guid", guidString) || guidString.IsEmpty ())
                return NativeCommandResult::Failure ("every element or restore needs elementId.guid");

            GS::ObjectState rec, previous;
            rec.Add ("elementId", elementId);

            API_Element element = {};
            element.header.guid = APIGuidFromString (guidString.ToCStr ().Get ());
            if (ACAPI_Element_Get (&element) != NoError) {
                previous.Add ("top", NoOverride); previous.Add ("side", NoOverride);
                previous.Add ("bottom", NoOverride); previous.Add ("chained", false);
                rec.Add ("found", false); rec.Add ("succeeded", false); rec.Add ("kind", "missing");
                rec.Add ("previous", previous); results.Push (rec);
                continue;
            }

            // Bind the three surface-override fields + the chaining flag for this
            // element's type. Everything below is type-agnostic; only the mask macro
            // needs the literal field names, so it stays in the per-type branch.
            const API_ElemTypeID typeId = element.header.type.typeID;
            API_OverriddenAttribute* mats[3] = { nullptr, nullptr, nullptr };
            bool*                    chainedFlag = nullptr;

            API_Element mask;
            ACAPI_ELEMENT_MASK_CLEAR (mask);

            if (typeId == API_SlabID) {
                mats[0] = &element.slab.topMat;  mats[1] = &element.slab.sideMat;  mats[2] = &element.slab.botMat;
                chainedFlag = &element.slab.materialsChained;
                ACAPI_ELEMENT_MASK_SET (mask, API_SlabType, topMat);
                ACAPI_ELEMENT_MASK_SET (mask, API_SlabType, sideMat);
                ACAPI_ELEMENT_MASK_SET (mask, API_SlabType, botMat);
                ACAPI_ELEMENT_MASK_SET (mask, API_SlabType, materialsChained);
                rec.Add ("kind", "slab");
            } else if (typeId == API_WallID) {
                mats[0] = &element.wall.refMat;  mats[1] = &element.wall.oppMat;  mats[2] = &element.wall.sidMat;
                chainedFlag = &element.wall.materialsChained;
                ACAPI_ELEMENT_MASK_SET (mask, API_WallType, refMat);
                ACAPI_ELEMENT_MASK_SET (mask, API_WallType, oppMat);
                ACAPI_ELEMENT_MASK_SET (mask, API_WallType, sidMat);
                ACAPI_ELEMENT_MASK_SET (mask, API_WallType, materialsChained);
                rec.Add ("kind", "wall");
            } else {
                previous.Add ("top", NoOverride); previous.Add ("side", NoOverride);
                previous.Add ("bottom", NoOverride); previous.Add ("chained", false);
                rec.Add ("found", false); rec.Add ("succeeded", false); rec.Add ("kind", "unsupported");
                rec.Add ("previous", previous); results.Push (rec);
                continue;
            }

            // Capture the ORIGINAL state before overwriting it.
            previous.Add ("top", mats[0]->hasValue ? mats[0]->value.ToInt32_Deprecated () : NoOverride);
            previous.Add ("side", mats[1]->hasValue ? mats[1]->value.ToInt32_Deprecated () : NoOverride);
            previous.Add ("bottom", mats[2]->hasValue ? mats[2]->value.ToInt32_Deprecated () : NoOverride);
            previous.Add ("chained", *chainedFlag);
            rec.Add ("previous", previous);

            // Apply the op.
            if (restore) {
                const char* fields[3] = { "top", "side", "bottom" };
                for (int m = 0; m < 3; ++m) {
                    GS::Int32 v = NoOverride;
                    target.Get (fields[m], v);
                    if (v == NoOverride) *mats[m] = APINullValue;
                    else                 *mats[m] = ACAPI_CreateAttributeIndex (v);
                }
                bool chained = false;
                target.Get ("chained", chained);
                *chainedFlag = chained;
            } else if (clear) {
                for (int m = 0; m < 3; ++m) *mats[m] = APINullValue;
                // leave chaining as-is on clear; the default surface applies regardless.
            } else {
                for (int m = 0; m < 3; ++m) *mats[m] = surfaceIdx;
                *chainedFlag = false;   // keep our 3 values independent
            }

            rec.Add ("found", true);
            // NO undo scope here — the dispatcher owns it (WriteCommand). withDel=true.
            const GSErrCode err = ACAPI_Element_Change (&element, &mask, nullptr, 0, true);
            rec.Add ("succeeded", err == NoError);
            results.Push (rec);
        }

        os.Add ("results", results);
        os.Add ("op", restore ? GS::UniString ("restore") : (clear ? GS::UniString ("clear") : GS::UniString ("paint")));
        return os;
    }
};

const NativeCommandRegistration commandRegistrations[] = {
    { "SetElementSurface", &MakeRegisteredNativeCommand<SetElementSurfaceCommand>, false,
      R"json({"oneOf":[{"type":"object","properties":{"elements":{"type":"array","minItems":1,"items":{"$ref":"#Element"}},"surface":{"type":"string","minLength":1}},"additionalProperties":false,"required":["elements","surface"]},{"type":"object","properties":{"elements":{"type":"array","minItems":1,"items":{"$ref":"#Element"}},"clear":{"const":true}},"additionalProperties":false,"required":["elements","clear"]},{"type":"object","properties":{"restores":{"type":"array","minItems":1,"items":{"type":"object","properties":{"elementId":{"$ref":"#ElementId"},"top":{"type":"integer"},"side":{"type":"integer"},"bottom":{"type":"integer"},"chained":{"type":"boolean"}},"additionalProperties":false,"required":["elementId","top","side","bottom","chained"]}}},"additionalProperties":false,"required":["restores"]}]})json",
      R"json({"oneOf":[{"type":"object","properties":{"results":{"type":"array","items":{"type":"object","properties":{"elementId":{"$ref":"#ElementId"},"found":{"type":"boolean"},"succeeded":{"type":"boolean"},"kind":{"type":"string","enum":["slab","wall","unsupported","missing"]},"previous":{"type":"object","properties":{"top":{"type":"integer"},"side":{"type":"integer"},"bottom":{"type":"integer"},"chained":{"type":"boolean"}},"additionalProperties":false,"required":["top","side","bottom","chained"]}},"additionalProperties":false,"required":["elementId","found","succeeded","kind","previous"]}},"op":{"type":"string","enum":["paint","clear","restore"]}},"additionalProperties":false,"required":["results","op"]},{"type":"object","properties":{"ok":{"const":false},"error":{"type":"string"}},"additionalProperties":false,"required":["ok","error"]}]})json" },
};

}   // namespace

NativeCommandRegistrations GetSurfaceCommandRegistrations ()
{
    return MakeRegistrationView (commandRegistrations);
}

} // namespace geomsrv
