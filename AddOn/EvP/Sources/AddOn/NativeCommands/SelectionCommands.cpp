#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/SelectionCommands.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/SelectionSetStore.hpp"

namespace geomsrv {

namespace {

bool ReadSelection (GS::Array<GS::UniString>& guids, GS::UniString& error)
{
    API_SelectionInfo selectionInfo = {};
    GS::Array<API_Neig> neigs;
    const GSErrCode err = ACAPI_Selection_Get (&selectionInfo, &neigs, false);
    if (selectionInfo.marquee.coords != nullptr)
        BMKillHandle (reinterpret_cast<GSHandle*> (&selectionInfo.marquee.coords));
    if (err != NoError && err != APIERR_NOSEL) {
        error = EVP_ACAPI_FAIL ("ACAPI_Selection_Get", err, "reading the current selection");
        return false;
    }
    guids.Clear ();
    for (const API_Neig& neig : neigs)
        guids.Push (GS::UniString (APIGuidToString (neig.guid).ToCStr ()));
    return true;
}

bool ResolveNeigs (const GS::Array<GS::UniString>& guidStrings, GS::Array<API_Neig>& neigs,
                   GS::Array<GS::UniString>& missing)
{
    neigs.Clear ();
    missing.Clear ();
    for (const GS::UniString& guidString : guidStrings) {
        const API_Guid guid = APIGuidFromString (guidString.ToCStr ().Get ());
        API_Neig neig = {};
        if (ACAPI_Selection_SetSelectedElementNeig (&guid, &neig) == NoError)
            neigs.Push (neig);
        else
            missing.Push (guidString);
    }
    return true;
}

bool ReadElements (const GS::ObjectState& params, GS::Array<GS::UniString>& guids)
{
    GS::Array<GS::ObjectState> elements;
    if (!params.Get ("elements", elements))
        return false;

    guids.Clear ();
    for (const GS::ObjectState& element : elements) {
        GS::ObjectState elementId;
        GS::UniString guid;
        if (!element.Get ("elementId", elementId) || !elementId.Get ("guid", guid) || guid.IsEmpty ())
            return false;
        guids.Push (guid);
    }
    return true;
}

GS::Array<GS::ObjectState> ElementRecords (const GS::Array<GS::UniString>& guids)
{
    GS::Array<GS::ObjectState> elements;
    for (const GS::UniString& guid : guids) {
        GS::ObjectState elementId, element;
        elementId.Add ("guid", guid);
        element.Add ("elementId", elementId);
        elements.Push (element);
    }
    return elements;
}

NativeCommandResult ModifySelection (const GS::UniString& op,
                                     const GS::Array<GS::UniString>& guidStrings)
{
    GS::ObjectState os;
    GS::Array<API_Neig> neigs;
    GS::Array<GS::UniString> missing;
    ResolveNeigs (guidStrings, neigs, missing);

    GSErrCode err = NoError;
    if (op == "replace" || op == "clear") {
        err = ACAPI_Selection_DeselectAll ();
        if (err == APIERR_NOSEL)
            err = NoError;
        if (err == NoError && op == "replace" && !neigs.IsEmpty ())
            err = ACAPI_Selection_Select (neigs, true);
    } else if (op == "add") {
        if (!neigs.IsEmpty ())
            err = ACAPI_Selection_Select (neigs, true);
    } else if (op == "remove") {
        if (!neigs.IsEmpty ())
            err = ACAPI_Selection_Select (neigs, false);
    } else {
        return NativeCommandResult::Failure ("op must be add, remove, replace, or clear");
    }
    if (err != NoError) {
        return NativeCommandResult::Failure (
            EVP_ACAPI_FAIL ("ACAPI_Selection_Select", err, "modifying the current selection"));
    }

    GS::Array<GS::UniString> current;
    GS::UniString error;
    if (!ReadSelection (current, error))
        return NativeCommandResult::Failure (error);
    os.Add ("selected", (GS::Int32) neigs.GetSize ());
    os.Add ("missing", ElementRecords (missing));
    os.Add ("changed", (GS::Int32) neigs.GetSize ());
    os.Add ("count", (GS::Int32) current.GetSize ());
    return os;
}

// ===========================================================================
// E3 — Selection & highlight. Read/set the current selection, colour-highlight
// elements in every window, and frame them in the view. All are MainThread
// commands (they call ACAPI) — none override NeedsMainThread(), so each is
// marshalled onto the main thread like every other ACAPI-touching command.
//
// None are IsWrite(): a selection or a highlight is UI state, not an undoable
// model edit, so wrapping them in an undo scope would cost the user an empty
// undo step for nothing (and ACAPI_Selection_* is not undoable anyway).
// ===========================================================================

// ---------------------------------------------------------------------------
// Tapioca.GetSelection {} -> { elements:[{elementId:{guid}}] }.
//
// APIERR_NOSEL ("nothing selected") is explicitly NOT a failure — it returns an
// empty set. A marquee-type selection allocates a coords handle on the info
// struct that is ours to dispose, exactly like GetStories' story handle.
// ---------------------------------------------------------------------------
class GetSelectionCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetSelection"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        API_SelectionInfo   selectionInfo = {};
        GS::Array<API_Neig> neigs;
        const GSErrCode err = ACAPI_Selection_Get (&selectionInfo, &neigs, false);

        // Marquee selections leave a coords handle to free; element selections
        // leave it null. Dispose it regardless of what we return.
        if (selectionInfo.marquee.coords != nullptr)
            BMKillHandle (reinterpret_cast<GSHandle*> (&selectionInfo.marquee.coords));

        if (err != NoError && err != APIERR_NOSEL) {
            return NativeCommandResult::Failure (
                EVP_ACAPI_FAIL ("ACAPI_Selection_Get", err, "reading the current selection"));
        }

        GS::Array<GS::ObjectState> elements;
        for (const API_Neig& neig : neigs) {
            GS::ObjectState elementId, element;
            elementId.Add ("guid", GS::UniString (APIGuidToString (neig.guid).ToCStr ()));
            element.Add ("elementId", elementId);
            elements.Push (element);
        }

        os.Add ("elements", elements);
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.SetSelection { elements:[{elementId:{guid}}], add?:bool }
//   -> { selected, missing:[{elementId:{guid}}], count }.
//
// add=false (default) replaces the selection: deselect all, then select `guids`.
// add=true leaves the current selection and adds to it. A guid that no longer
// resolves to a neig is reported in `missing` rather than silently dropped —
// selecting the wrong (or an empty) set is exactly the quiet wrongness the read
// commands guard against.
// ---------------------------------------------------------------------------
class SetSelectionCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "SetSelection"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Array<GS::UniString> guidStrings;
        if (!ReadElements (params, guidStrings))
            return NativeCommandResult::Failure ("need elements=[{elementId:{guid}}]");

        bool add = false;
        params.Get ("add", add);

        if (!add) {
            const GSErrCode derr = ACAPI_Selection_DeselectAll ();
            if (derr != NoError && derr != APIERR_NOSEL) {
                return NativeCommandResult::Failure (
                    EVP_ACAPI_FAIL ("ACAPI_Selection_DeselectAll", derr, "clearing before a replace-style select"));
            }
        }

        GS::Array<API_Neig>      neigs;
        GS::Array<GS::UniString> missing;
        for (const GS::UniString& guidString : guidStrings) {
            const API_Guid guid = APIGuidFromString (guidString.ToCStr ().Get ());
            API_Neig neig = {};
            if (ACAPI_Selection_SetSelectedElementNeig (&guid, &neig) == NoError)
                neigs.Push (neig);
            else
                missing.Push (guidString);
        }

        // Always add=true here: when the caller asked for a replace we already
        // cleared the selection above, so this is a set either way.
        if (!neigs.IsEmpty ()) {
            const GSErrCode serr = ACAPI_Selection_Select (neigs, true);
            if (serr != NoError) {
                return NativeCommandResult::Failure (
                    EVP_ACAPI_FAIL ("ACAPI_Selection_Select", serr, GS::UniString::Printf ("%u element(s)", (unsigned) neigs.GetSize ())));
            }
        }

        GS::Array<GS::UniString> current;
        GS::UniString readError;
        if (!ReadSelection (current, readError))
            return NativeCommandResult::Failure (readError);

        os.Add ("selected", (GS::Int32) neigs.GetSize ());
        os.Add ("missing", ElementRecords (missing));
        os.Add ("count", (GS::Int32) current.GetSize ());
        return os;
    }
};

// Tapioca.ModifySelection { op:"add"|"remove"|"replace"|"clear", elements?:[...] }.
// Unlike SetSelection's legacy count, this reports the final active-selection count.
class ModifySelectionCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "ModifySelection"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString op;
        if (!params.Get ("op", op))
            return NativeCommandResult::Failure ("need op=add|remove|replace|clear");
        GS::Array<GS::UniString> guids;
        if (op != "clear" && !ReadElements (params, guids))
            return NativeCommandResult::Failure ("need elements=[{elementId:{guid}}] unless op=clear");
        return ModifySelection (op, guids);
    }
};

// Saved role sets live in SelectionSetStore. `current:true` captures the active
// Archicad selection atomically on the main thread for palette and Python callers.
class ModifySelectionSetCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "ModifySelectionSet"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;
        GS::UniString name, op;
        if (!params.Get ("name", name) || !params.Get ("op", op))
            return NativeCommandResult::Failure ("need name and op=update|add|remove|clear");
        GS::Array<GS::UniString> guids;
        bool current = false;
        params.Get ("current", current);
        GS::UniString error;
        if (current && !ReadSelection (guids, error))
            return NativeCommandResult::Failure (error);
        if (!current && op != "clear" && !ReadElements (params, guids))
            return NativeCommandResult::Failure ("need elements=[{elementId:{guid}}] or current=true");
        SelectionSetStore::Mutation mutation = SelectionSetStore::Mutation::Replace;
        if (op == "add") mutation = SelectionSetStore::Mutation::Add;
        else if (op == "remove") mutation = SelectionSetStore::Mutation::Remove;
        else if (op == "clear") guids.Clear ();
        else if (op != "update")
            return NativeCommandResult::Failure ("op must be update, add, remove, or clear");
        GS::Int32 changed = 0;
        if (!SelectionSetStore::Get ().Mutate (name, guids, mutation, changed, error))
            return NativeCommandResult::Failure (error);
        const GS::Array<GS::UniString> values = SelectionSetStore::Get ().Values (name);
        os.Add ("changed", changed);
        os.Add ("count", (GS::Int32) values.GetSize ());
        os.Add ("elements", ElementRecords (values));
        return os;
    }
};

class GetSelectionSetCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetSelectionSet"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;
        GS::UniString name;
        if (!params.Get ("name", name) || !SelectionSetStore::Get ().IsDeclared (name))
            return NativeCommandResult::Failure ("selection set is not declared for the active command");
        const GS::Array<GS::UniString> values = SelectionSetStore::Get ().Values (name);
        os.Add ("elements", ElementRecords (values));
        os.Add ("count", (GS::Int32) values.GetSize ());
        return os;
    }
};

class ListSelectionSetsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "ListSelectionSets"; }
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        GS::ObjectState os;
        os.Add ("names", SelectionSetStore::Get ().Names ());
        return os;
    }
};

class ReselectSelectionSetCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "ReselectSelectionSet"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString name;
        if (!params.Get ("name", name) || !SelectionSetStore::Get ().IsDeclared (name))
            return NativeCommandResult::Failure ("selection set is not declared for the active command");
        return ModifySelection ("replace", SelectionSetStore::Get ().Values (name));
    }
};

// ---------------------------------------------------------------------------
// Tapioca.HighlightElements { elements:[{elementId:{guid},color?:[r,g,b,a]}], color?:[r,g,b,a],
//                         wireframe3D?:bool, dimOthers?:[r,g,b,a] } -> { count }.
//
// `color` components are 0..1 (API_RGBAColor's own range), default opaque red.
// `colors` is the PER-ELEMENT form: a FLAT array of 4 components per guid, in
// guid order, used instead of `color` when present. It exists because
// ACAPI_UserInput_SetElementHighlight takes a guid->colour MAP, and one call
// REPLACES the whole map — so a caller that wants two colours (errors red,
// warnings orange) cannot get there with two calls, only with one call carrying
// both. Flat, not nested, because ObjectState does not read a nested array back.
//
// `wireframe3D` switches non-highlighted 3D elements to wireframe. `dimOthers`
// is the colour+alpha applied to every NON-highlighted element (that is the
// API's nonHighlightedElemsColor) — an [r,g,b,a] tuple, not a bool, because
// that is what the underlying call takes; a low alpha dims the rest.
//
// The model must be redrawn after changing highlights (documented on the API),
// so this issues ACAPI_View_Redraw itself.
// ---------------------------------------------------------------------------
class HighlightElementsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "HighlightElements"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Array<GS::ObjectState> elements;
        if (!params.Get ("elements", elements))
            return NativeCommandResult::Failure ("need elements=[{elementId:{guid}}]");

        API_RGBAColor rgba = { 1.0, 0.2, 0.2, 1.0 };
        GS::Array<double> color;
        if (params.Get ("color", color) && color.GetSize () >= 3) {
            rgba.f_red   = color[0];
            rgba.f_green = color[1];
            rgba.f_blue  = color[2];
            rgba.f_alpha = (color.GetSize () >= 4) ? color[3] : 1.0;
        }

        GS::HashTable<API_Guid, API_RGBAColor> highlight;
        for (const GS::ObjectState& element : elements) {
            GS::ObjectState elementId;
            GS::UniString guidString;
            if (!element.Get ("elementId", elementId) || !elementId.Get ("guid", guidString))
                return NativeCommandResult::Failure ("every element needs elementId.guid");
            API_RGBAColor c = rgba;
            GS::Array<double> itemColor;
            if (element.Get ("color", itemColor) && itemColor.GetSize () >= 3) {
                c.f_red   = itemColor[0];
                c.f_green = itemColor[1];
                c.f_blue  = itemColor[2];
                c.f_alpha = itemColor.GetSize () >= 4 ? itemColor[3] : 1.0;
            }
            highlight.Add (APIGuidFromString (guidString.ToCStr ().Get ()), c);
        }

        GS::Optional<bool> wireframe3D;
        bool wf = false;
        if (params.Get ("wireframe3D", wf))
            wireframe3D = wf;

        GS::Optional<API_RGBAColor> dimColor;
        GS::Array<double> dim;
        if (params.Get ("dimOthers", dim) && dim.GetSize () >= 3) {
            API_RGBAColor d = { dim[0], dim[1], dim[2], (dim.GetSize () >= 4) ? dim[3] : 1.0 };
            dimColor = d;
        }

        ACAPI_UserInput_SetElementHighlight (highlight, wireframe3D, dimColor);
        ACAPI_View_Redraw ();

        os.Add ("count", (GS::Int32) highlight.GetSize ());
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.ClearHighlights {} -> {}. Removes highlights set by HighlightElements and
// redraws.
// ---------------------------------------------------------------------------
class ClearHighlightsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "ClearHighlights"; }
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        ACAPI_UserInput_ClearElementHighlight ();
        ACAPI_View_Redraw ();

        GS::ObjectState os;
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.ZoomTo { elements:[{elementId:{guid}}] } -> { count }.
// active view.
// ---------------------------------------------------------------------------
class ZoomToCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "ZoomTo"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Array<GS::UniString> guidStrings;
        if (!ReadElements (params, guidStrings) || guidStrings.IsEmpty ())
            return NativeCommandResult::Failure ("need non-empty elements=[{elementId:{guid}}]");

        GS::Array<API_Guid> guids;
        for (const GS::UniString& guidString : guidStrings)
            guids.Push (APIGuidFromString (guidString.ToCStr ().Get ()));

        const GSErrCode err = ACAPI_View_ZoomToElements (&guids);
        if (err != NoError) {
            return NativeCommandResult::Failure (
                EVP_ACAPI_FAIL ("ACAPI_View_ZoomToElements", err, GS::UniString::Printf ("%u guid(s)", (unsigned) guids.GetSize ())));
        }

        os.Add ("count", (GS::Int32) guids.GetSize ());
        return os;
    }
};

const NativeCommandRegistration commandRegistrations[] = {
    { "GetSelection", &MakeRegisteredNativeCommand<GetSelectionCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({
            "type":"object",
            "properties":{
                "elements":{"type":"array","items":{
                    "type":"object",
                    "properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]}},
                    "additionalProperties":false,
                    "required":["elementId"]
                }}
            },
            "additionalProperties":false,
            "required":["elements"]
        })json" },
    { "SetSelection", &MakeRegisteredNativeCommand<SetSelectionCommand>, false,
      R"json({
            "type":"object",
            "properties":{
                "elements":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]}},
                "add":{"type":"boolean"}
            },
            "additionalProperties":false,
            "required":["elements"]
        })json",
      R"json({
            "type":"object",
            "properties":{
                "selected":{"type":"integer","minimum":0},
                "missing":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]}},
                "count":{"type":"integer","minimum":0}
            },
            "additionalProperties":false,
            "required":["selected","missing","count"]
        })json" },
    { "ModifySelection", &MakeRegisteredNativeCommand<ModifySelectionCommand>, false,
      R"json({
            "type":"object",
            "properties":{
                "op":{"type":"string","enum":["add","remove","replace","clear"]},
                "elements":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]}}
            },
            "additionalProperties":false,
            "required":["op"]
        })json",
      R"json({
            "type":"object",
            "properties":{
                "selected":{"type":"integer","minimum":0},
                "missing":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]}},
                "changed":{"type":"integer","minimum":0},
                "count":{"type":"integer","minimum":0}
            },
            "additionalProperties":false,
            "required":["selected","missing","changed","count"]
        })json" },
    { "ModifySelectionSet", &MakeRegisteredNativeCommand<ModifySelectionSetCommand>, false,
      R"json({"type":"object","properties":{"name":{"type":"string","minLength":1},"op":{"type":"string","enum":["update","add","remove","clear"]},"current":{"type":"boolean"},"elements":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]}}},"additionalProperties":false,"required":["name","op"]})json",
      R"json({"type":"object","properties":{"changed":{"type":"integer","minimum":0},"count":{"type":"integer","minimum":0},"elements":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]}}},"additionalProperties":false,"required":["changed","count","elements"]})json" },
    { "GetSelectionSet", &MakeRegisteredNativeCommand<GetSelectionSetCommand>, false,
      R"json({"type":"object","properties":{"name":{"type":"string","minLength":1}},"additionalProperties":false,"required":["name"]})json",
      R"json({"type":"object","properties":{"elements":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]}},"count":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["elements","count"]})json" },
    { "ListSelectionSets", &MakeRegisteredNativeCommand<ListSelectionSetsCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"names":{"type":"array","items":{"type":"string"}}},"additionalProperties":false,"required":["names"]})json" },
    { "ReselectSelectionSet", &MakeRegisteredNativeCommand<ReselectSelectionSetCommand>, false,
      R"json({"type":"object","properties":{"name":{"type":"string","minLength":1}},"additionalProperties":false,"required":["name"]})json",
      R"json({"type":"object","properties":{"selected":{"type":"integer","minimum":0},"missing":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]}},"changed":{"type":"integer","minimum":0},"count":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["selected","missing","changed","count"]})json" },
    { "HighlightElements", &MakeRegisteredNativeCommand<HighlightElementsCommand>, false,
      R"json({
            "type":"object",
            "properties":{
                "elements":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]},"color":{"type":"array","items":{"type":"number","minimum":0,"maximum":1},"minItems":3,"maxItems":4}},"additionalProperties":false,"required":["elementId"]}},
                "color":{"type":"array","items":{"type":"number","minimum":0,"maximum":1},"minItems":3,"maxItems":4},
                "wireframe3D":{"type":"boolean"},
                "dimOthers":{"type":"array","items":{"type":"number","minimum":0,"maximum":1},"minItems":3,"maxItems":4}
            },
            "additionalProperties":false,
            "required":["elements"]
        })json",
      R"json({"type":"object","properties":{"count":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["count"]})json" },
    { "ClearHighlights", &MakeRegisteredNativeCommand<ClearHighlightsCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{},"additionalProperties":false})json" },
    { "ZoomTo", &MakeRegisteredNativeCommand<ZoomToCommand>, false,
      R"json({"type":"object","properties":{"elements":{"type":"array","minItems":1,"items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]}}},"additionalProperties":false,"required":["elements"]})json",
      R"json({"type":"object","properties":{"count":{"type":"integer","minimum":1}},"additionalProperties":false,"required":["count"]})json" },
};

}   // namespace

NativeCommandRegistrations GetSelectionCommandRegistrations ()
{
    return MakeRegistrationView (commandRegistrations);
}

} // namespace geomsrv
