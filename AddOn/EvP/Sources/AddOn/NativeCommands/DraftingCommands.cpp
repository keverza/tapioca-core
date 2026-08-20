#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/DraftingCommands.hpp"
#include "NativeCommands/CommandRegistration.hpp"
#include "NativeCommands/CommandUtils.hpp"      // ResolveLayerParam, ParseAnchor

#include "File.hpp"          // IO::File / IO::Location — the picture bytes
#include "GXImageBase.h"     // GX::ImageBase::GetFileInfo — pixel dimensions

namespace geomsrv {

namespace {

// ===========================================================================
// EvP.CreateText — a monostyle text element with every basic input exposed.
//
// WHY THIS EXISTS ALONGSIDE Tapir.CreateTexts (E11 routes Tapir already):
// Tapir's CreateTexts takes coordinate / text / height / pen / angle /
// justification / floorIndex and nothing else — verified in the vendored source
// (`archicad-addon/Sources/ElementCreationCommands.cpp`,
// CreateTextsCommand::SetTypeSpecificParameters). It cannot set the LAYER, the
// ANCHOR (which corner of the text the coordinate means), the FACE STYLE
// (bold / italic / underline), the FONT, the fixed-width box, or the
// fixed-angle / fixed-size flags. A numbering or annotation run needs all of
// those, so this is the fuller write.
//
// UNITS, from Tapir's own schema and the AC29 header (both checked, they agree):
//   size   character height in MILLIMETRES (paper), not metres
//   width  horizontal size of the text BOX, also MILLIMETRES — the header says so
//          in as many words ("Horizontal size of text box in mm"). Passing a
//          metre-scale number here does not make a small box, it makes a box a
//          fraction of a millimetre wide, and the text then wraps to ONE
//          CHARACTER PER LINE. That is what a "0.20" looked like on screen.
//   angle  RADIANS, counter-clockwise
//   x,y    model coordinates of the current database
//
// THE DEFAULTS TRAP, and why this command RESETS rather than inherits:
// ACAPI_Element_GetDefaults returns the Text tool's CURRENT state — whatever the
// user (or the last script) left set. So a caller that says only {text, x, y}
// does not get plain horizontal text; it gets whatever rotation, bold and
// justification were lying around, and the effect compounds run over run. That
// was reported from a real run: "rotated text is rotated even more".
//
// So every FORMATTING/ORIENTATION field this command exposes starts from a
// deterministic baseline — angle 0, plain face, no effects, left justified,
// bottom-left anchor, no fixed-width box, not fixed angle/size — and the caller
// raises only what it asks for. What is NOT reset is the tool's IDENTITY: size,
// pen and font, because a script that does not mention them genuinely wants the
// user's current text style.
//
// `inheritDefaults: true` on an item restores the old behaviour (take the tool
// state wholesale) for a caller that deliberately wants "whatever the user has
// set up right now".
//
// THE PARAGRAPH TRAP: even a single-style, single-line text will not create
// correctly from `memo.textContent` alone — API_TextType.nLine and a paragraph
// with one run and one EOL per line must be built too. That is not in the
// header docs; it is what Tapir's shipped implementation does, and it is
// reproduced faithfully below (SetParagraph/SetRun/SetEOL). Skipping it is the
// obvious-looking shortcut that produces an empty or single-line text.
// ===========================================================================

// --- the paragraph machinery -----------------------------------------------
// Shape taken from the DevKit's Element_Test example and Tapir's shipped
// CreateTexts; the allocation kinds (BMhAll for the handle, BMpAllClear for the
// arrays inside it) are load-bearing — Archicad frees them through
// ACAPI_DisposeElemMemoHdls and expects exactly these.

GSErrCode SetParagraph (API_ParagraphType** paragraph, UInt32 parNum, Int32 from, Int32 range,
                        Int32 numOfTabs, Int32 numOfRuns, Int32 numOfEolPos)
{
    if (paragraph == nullptr || numOfTabs < 1 || numOfRuns < 1 || numOfEolPos < 0)
        return APIERR_BADPARS;
    if (parNum >= (BMhGetSize (reinterpret_cast<GSHandle> (paragraph)) / sizeof (API_ParagraphType)))
        return APIERR_BADPARS;

    (*paragraph)[parNum].from  = from;
    (*paragraph)[parNum].range = range;
    (*paragraph)[parNum].tab   = reinterpret_cast<API_TabType*> (BMpAllClear (numOfTabs * sizeof (API_TabType)));
    (*paragraph)[parNum].run   = reinterpret_cast<API_RunType*> (BMpAllClear (numOfRuns * sizeof (API_RunType)));
    if (numOfEolPos > 0)
        (*paragraph)[parNum].eolPos = reinterpret_cast<Int32*> (BMpAllClear (numOfEolPos * sizeof (Int32)));
    return NoError;
}

GSErrCode SetRun (API_ParagraphType** paragraph, UInt32 parNum, UInt32 runNum, Int32 from, Int32 range,
                  short pen, unsigned short faceBits, short font, Int32 effectBits, double size)
{
    if (paragraph == nullptr)
        return APIERR_BADPARS;
    if (parNum >= (BMhGetSize (reinterpret_cast<GSHandle> (paragraph)) / sizeof (API_ParagraphType)))
        return APIERR_BADPARS;
    if (runNum >= BMGetPtrSize (reinterpret_cast<GSPtr> ((*paragraph)[parNum].run)) / sizeof (API_RunType))
        return APIERR_BADPARS;

    API_RunType& run = (*paragraph)[parNum].run[runNum];
    run.from       = from;
    run.range      = range;
    run.pen        = pen;
    run.faceBits   = faceBits;
    run.font       = font;
    run.effectBits = (unsigned short) effectBits;
    run.size       = size;
    return NoError;
}

GSErrCode SetEOL (API_ParagraphType** paragraph, UInt32 parNum, UInt32 eolNum, Int32 offset)
{
    if (paragraph == nullptr || offset < 0)
        return APIERR_BADPARS;
    if (parNum >= (BMhGetSize (reinterpret_cast<GSHandle> (paragraph)) / sizeof (API_ParagraphType)))
        return APIERR_BADPARS;
    if (eolNum >= BMGetPtrSize (reinterpret_cast<GSPtr> ((*paragraph)[parNum].eolPos)) / sizeof (Int32))
        return APIERR_BADPARS;

    (*paragraph)[parNum].eolPos[eolNum] = offset;
    return NoError;
}

// Content + the one paragraph that makes it render. `textData` must already
// carry the final pen/faceBits/font/effectsBits/size — the run copies them, and
// a monostyle text renders from the RUN, not from the struct fields, so setting
// them after this call has no effect.
void SetTextContentAndParagraphs (API_ElementMemo& memo, API_TextType& textData, const GS::UniString& text)
{
    delete memo.textContent;
    memo.textContent = new GS::UniString (text);

    const GS::UniChar newlineChar = GS::UniChar (char ('\n'));
    textData.nLine = text.Count (newlineChar) + 1;

    memo.paragraphs = reinterpret_cast<API_ParagraphType**> (BMhAll (1 * sizeof (API_ParagraphType)));
    if (memo.paragraphs == nullptr)
        return;

    SetParagraph (memo.paragraphs, 0, 0, text.GetLength (), 1, 1, textData.nLine);
    SetRun (memo.paragraphs, 0, 0, 0, text.GetLength (),
            textData.pen, textData.faceBits, textData.font, textData.effectsBits, textData.size);

    // One EOL offset per line: the length of that line, not its absolute
    // position. Same walk as Tapir's, which is the only place this is written
    // down.
    Int32 lastEolPos = 0;
    for (Int32 eolIndex = 0; eolIndex < textData.nLine; ++eolIndex) {
        const Int32 eolPos = text.FindFirst (newlineChar, eolIndex == 0 ? 0 : lastEolPos + 1);
        const Int32 offset = (eolPos != (Int32) MaxUIndex ? eolPos : (Int32) text.GetLength ()) - lastEolPos - (eolIndex == 0 ? 0 : 1);
        lastEolPos = eolPos;
        SetEOL (memo.paragraphs, 0, eolIndex, offset < 0 ? 0 : offset);
    }
}

API_JustID ParseJust (const GS::UniString& name)
{
    if (name == "center") return APIJust_Center;
    if (name == "right")  return APIJust_Right;
    if (name == "full")   return APIJust_Full;
    return APIJust_Left;
}

// ParseAnchor ("topLeft" -> APIAnc_LT) moved to CommandUtils when the drawing
// placement became its second domain — see the rule in CommandUtils.hpp.

const char* AnchorName (API_AnchorID anchor)
{
    switch (anchor) {
        case APIAnc_LT: return "topLeft";
        case APIAnc_MT: return "topCenter";
        case APIAnc_RT: return "topRight";
        case APIAnc_LM: return "middleLeft";
        case APIAnc_MM: return "middleCenter";
        case APIAnc_RM: return "middleRight";
        case APIAnc_LB: return "bottomLeft";
        case APIAnc_MB: return "bottomCenter";
        case APIAnc_RB: return "bottomRight";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Tapioca.GetTextElements { elements?: [{elementId:{guid}}] }
//   -> { texts:[...], count, skipped, fromSelection }
//
// The READ side of CreateText, and the reason it exists: NOTHING could read a
// text element before this. EvP.GetElementDetails has no `text` kind, and
// Tapir's TypeSpecificDetails union has no TextDetails either (checked in the
// vendored common_schema_definitions.js) — a survey drawing whose spot heights
// are TEXT elements was simply unreadable through either add-on.
//
// `guids` omitted => the CURRENT SELECTION, which is the shape the caller
// actually wants: the user marquees the spot-height labels in a worksheet and
// runs the command. That saves a second bus hop through EvP.GetSelection and,
// more importantly, guarantees the selection is read on the SAME main-thread
// visit as the text bodies, so it cannot change in between.
//
// ⚠️ THE BOTTOM-LEFT CORNER IS NOT `loc`. API_TextType.loc is the ANCHOR point,
// and which corner that is depends on `anchor` (APIAnc_LT…APIAnc_RB) — the user
// styles labels however they like, so `loc` is bottom-left only by luck. The box
// size (`width`/`height`) is in PAPER MILLIMETRES, not model units, so deriving
// the corner from loc + width/height would also need the database's drawing
// scale. ACAPI_Element_CalcBounds sidesteps both: it returns the element's real
// extent in MODEL coordinates of its own database, so (xMin, yMin) IS the
// bottom-left corner, rotation and anchor already accounted for. That is what
// `x`/`y` report. `anchorX`/`anchorY` carry the raw loc alongside, because a
// caller that wants the styled insertion point should not have to ask twice.
//
// `pen` is the monostyle text's pen. It is the join key for matching a label to
// a polyline drawn in the same colour (the surveyor's convention), so it is
// reported even though a MULTISTYLE text's real pens live per-run in the
// paragraphs memo — for a multistyle text this is the element-level fallback and
// `multiStyle` says so, rather than the value quietly meaning something else.
// ---------------------------------------------------------------------------
class GetTextElementsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetTextElements"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        // --- which elements: explicit typed IDs, else the current selection ---
        GS::Array<GS::ObjectState> requested;
        GS::Array<GS::UniString> guidStrings;
        const bool fromSelection = !params.Get ("elements", requested);
        if (!fromSelection) {
            for (const GS::ObjectState& item : requested) {
                GS::ObjectState elementId;
                GS::UniString guid;
                if (!item.Get ("elementId", elementId) || !elementId.Get ("guid", guid)) {
                    return NativeCommandResult::Failure ("every element needs elementId.guid");
                }
                guidStrings.Push (guid);
            }
        }
        if (fromSelection) {
            API_SelectionInfo   selectionInfo = {};
            GS::Array<API_Neig> neigs;
            const GSErrCode selErr = ACAPI_Selection_Get (&selectionInfo, &neigs, false);
            if (selectionInfo.marquee.coords != nullptr)
                BMKillHandle (reinterpret_cast<GSHandle*> (&selectionInfo.marquee.coords));
            // APIERR_NOSEL is "nothing selected", not a failure — an empty
            // answer is the honest one, exactly as EvP.GetSelection treats it.
            if (selErr != NoError && selErr != APIERR_NOSEL) {
                return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Selection_Get", selErr,
                                                                      "reading the selection for EvP.GetTextElements"));
            }
            for (const API_Neig& neig : neigs)
                guidStrings.Push (GS::UniString (APIGuidToString (neig.guid).ToCStr ()));
        }

        GS::Array<GS::ObjectState> texts;
        GS::Int32                  skipped = 0;   // selected things that are not texts

        for (const GS::UniString& guidString : guidStrings) {
            API_Element element = {};
            element.header.guid = APIGuidFromString (guidString.ToCStr ().Get ());
            if (ACAPI_Element_Get (&element) != NoError) {
                ++skipped;
                continue;
            }
            // A marquee over a survey drawing catches polylines and dimensions
            // too. Those are the CALLER's other input, not an error here, so
            // they are counted and dropped rather than refused.
            if (element.header.type.typeID != API_TextID) {
                ++skipped;
                continue;
            }

            API_ElementMemo memo = {};
            const GSErrCode memoErr = ACAPI_Element_GetMemo (element.header.guid, &memo,
                                                            APIMemoMask_TextContent);
            GS::UniString content;
            if (memoErr == NoError && memo.textContent != nullptr)
                content = *memo.textContent;
            ACAPI_DisposeElemMemoHdls (&memo);

            // Model-space extent — see the header note on why loc is not enough.
            // A failure here is not fatal: the content and the anchor are still
            // worth returning, and `hasBounds` tells the caller which it got.
            API_Box3D bounds = {};
            const bool hasBounds =
                ACAPI_Element_CalcBounds (&element.header, &bounds) == NoError;

            GS::ObjectState rec;
            GS::ObjectState elementId;
            elementId.Add ("guid", guidString);
            rec.Add ("elementId",  elementId);
            rec.Add ("content",    content);
            // THE point of this command: bottom-left of the real model-space box.
            rec.Add ("x",          hasBounds ? bounds.xMin : element.text.loc.x);
            rec.Add ("y",          hasBounds ? bounds.yMin : element.text.loc.y);
            rec.Add ("hasBounds",  hasBounds);
            rec.Add ("xMin",       bounds.xMin);
            rec.Add ("yMin",       bounds.yMin);
            rec.Add ("xMax",       bounds.xMax);
            rec.Add ("yMax",       bounds.yMax);
            rec.Add ("anchorX",    element.text.loc.x);
            rec.Add ("anchorY",    element.text.loc.y);
            rec.Add ("anchor",     GS::UniString (AnchorName (element.text.anchor)));
            rec.Add ("pen",        (GS::Int32) element.text.pen);
            rec.Add ("angle",      element.text.angle);
            rec.Add ("size",       element.text.size);
            rec.Add ("multiStyle", element.text.multiStyle);
            rec.Add ("nLine",      (GS::Int32) element.text.nLine);
            rec.Add ("floorInd",   (GS::Int32) element.header.floorInd);
            rec.Add ("layer",      AttributeIndexToName (API_LayerID, element.header.layer));
            texts.Push (rec);
        }

        os.Add ("fromSelection", fromSelection);
        os.Add ("texts", texts);
        os.Add ("count", (GS::Int32) texts.GetSize ());
        os.Add ("skipped", skipped);
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.GetArcElements { elements?: [...], scope?: "database"|"selection", wholeOnly?: bool }
//   -> { arcs: [ { elementId, x, y, radius, isCircle, begAngle, endAngle, ratio,
//                      angle, pen, layer, floorInd } ], count, scope }
//
// The CIRCLE MARKERS a surveyor drops on each measured point. The height is
// written as a nearby TEXT, but the point itself is the little circle, and the
// text is set beside it so it does not cover the drawing — so the text's own
// position is a couple of metres off the thing it describes. Reading the circles
// is what lets a caller put the survey point where the surveyor put it.
//
// SCOPE DEFAULTS TO THE WHOLE DATABASE, not the selection, and that is the point
// of the command: the user selects the TEXTS (or drag-selects everything) and the
// circles must be found either way. `scope:"selection"` narrows it to what is
// selected, and explicit `guids` narrows it further still.
//
// Archicad has TWO types here and a caller needs both: API_CircleID (the Circle
// tool) and API_ArcID (the Arc tool). They share one struct — the header says
// `using API_CircleType = API_ArcType` in as many words — so one branch serves
// both and `isCircle` distinguishes them. `wholeOnly` (default false) keeps only
// full circles, for a drawing where partial arcs mean something else.
//
// ⚠️ `ACAPI_Element_GetElemList` reads the CURRENT database. Run from a worksheet
// it returns that worksheet's arcs, which is exactly what is wanted here; the
// answer echoes `scope` so a caller can log which database it actually searched.
// ---------------------------------------------------------------------------
class GetArcElementsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetArcElements"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        bool wholeOnly = false;
        params.Get ("wholeOnly", wholeOnly);

        GS::UniString scope;
        if (!params.Get ("scope", scope) || scope.IsEmpty ())
            scope = "database";
        if (scope != "database" && scope != "selection") {
            return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("unknown scope: %T (want \"database\" or \"selection\")",
                                                                                  scope.ToPrintf ()),
                                                            "EvP.GetArcElements"));
        }

        GS::Array<GS::ObjectState> requested;
        const bool haveElements = params.Get ("elements", requested);

        GS::Array<API_Guid> candidates;
        if (haveElements) {
            for (const GS::ObjectState& item : requested) {
                GS::ObjectState elementId;
                GS::UniString guid;
                if (!item.Get ("elementId", elementId) || !elementId.Get ("guid", guid)) {
                    return NativeCommandResult::Failure ("every element needs elementId.guid");
                }
                candidates.Push (APIGuidFromString (guid.ToCStr ().Get ()));
            }
            scope = "elements";

        } else if (scope == "selection") {
            API_SelectionInfo   selectionInfo = {};
            GS::Array<API_Neig> neigs;
            const GSErrCode selErr = ACAPI_Selection_Get (&selectionInfo, &neigs, false);
            if (selectionInfo.marquee.coords != nullptr)
                BMKillHandle (reinterpret_cast<GSHandle*> (&selectionInfo.marquee.coords));
            if (selErr != NoError && selErr != APIERR_NOSEL) {
                return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Selection_Get", selErr,
                                                                      "reading the selection for EvP.GetArcElements"));
            }
            for (const API_Neig& neig : neigs)
                candidates.Push (neig.guid);

        } else {
            // Both tools, in one list. A caller asking for "the circles" means the
            // markers, and a surveyor's template may well have drawn them with
            // either tool.
            for (const API_ElemTypeID typeId : { API_CircleID, API_ArcID }) {
                GS::Array<API_Guid> found;
                if (const GSErrCode listErr = ACAPI_Element_GetElemList (typeId, &found); listErr != NoError) {
                    return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Element_GetElemList", listErr,
                                                                          typeId == API_CircleID ? "API_CircleID (listing circle markers)"
                                                                                                 : "API_ArcID (listing arc markers)"));
                }
                for (const API_Guid& g : found)
                    candidates.Push (g);
            }
        }

        GS::Array<GS::ObjectState> arcs;
        GS::Int32                  skipped = 0;

        for (const API_Guid& guid : candidates) {
            API_Element element = {};
            element.header.guid = guid;
            if (ACAPI_Element_Get (&element) != NoError) {
                ++skipped;
                continue;
            }
            const API_ElemTypeID typeId = element.header.type.typeID;
            if (typeId != API_CircleID && typeId != API_ArcID) {
                ++skipped;          // a selection full of texts lands here
                continue;
            }

            // API_CircleType IS API_ArcType (APIdefs_Elements.h), so one read
            // serves both and only the type tells them apart.
            const API_ArcType& arc = element.arc;
            const bool isCircle = (typeId == API_CircleID);
            if (wholeOnly && !isCircle) {
                ++skipped;
                continue;
            }

            GS::ObjectState rec;
            GS::ObjectState elementId;
            elementId.Add ("guid", GS::UniString (APIGuidToString (guid).ToCStr ()));
            rec.Add ("elementId", elementId);
            rec.Add ("x",        arc.origC.x);      // the CENTRE - the survey point
            rec.Add ("y",        arc.origC.y);
            rec.Add ("radius",   arc.r);
            rec.Add ("isCircle", isCircle);
            rec.Add ("begAngle", arc.begAng);
            rec.Add ("endAngle", arc.endAng);
            rec.Add ("ratio",    arc.ratio);        // != 1 means an ELLIPSE
            rec.Add ("angle",    arc.angle);
            rec.Add ("pen",      (GS::Int32) arc.linePen.penIndex);
            rec.Add ("layer",    AttributeIndexToName (API_LayerID, element.header.layer));
            rec.Add ("floorInd", (GS::Int32) element.header.floorInd);
            arcs.Push (rec);
        }

        os.Add ("scope", scope);
        os.Add ("arcs", arcs);
        os.Add ("count", (GS::Int32) arcs.GetSize ());
        os.Add ("skipped", skipped);
        return os;
    }
};

// ---------------------------------------------------------------------------
// EvP.CreateText
//   { texts: [ { text, x, y, floorInd?, layer?, size?, angle?, pen?, font?,
//                just?, anchor?, bold?, italic?, underline?, strikeOut?,
//                width?, fixedAngle?, fixedSize?, spacing?, widthFactor?,
//                charSpaceFactor? }, … ] }
//   -> { count, results: [ {succeeded, elementId?, error?} ] }
//
// A BATCH like CreateWall: one call, one undo step, because the caller that
// wants this (numbering along a polyline) places tens of texts at once and a
// gate hop per text is the cost CLAUDE.md forbids.
//
// Every optional field defaults to the Text tool's current default via
// ACAPI_Element_GetDefaults — so a caller that sends only {text, x, y} gets what
// the user would get by typing it, which is the right floor for "leave it alone".
// ---------------------------------------------------------------------------
class CreateTextCommand : public WriteCommand {
public:
    GS::String GetName () const override { return "CreateText"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Array<GS::ObjectState> items;
        if (!params.Get ("texts", items) || items.IsEmpty ()) {
            return NativeCommandResult::Failure (EVP_FAIL ("need texts=[{text, x, y, …}, …] (non-empty)", "EvP.CreateText"));
        }

        GS::Array<GS::ObjectState> results;
        GS::Int32                  created = 0;

        for (const GS::ObjectState& item : items) {
            GS::ObjectState rec;

            GS::UniString content;
            double x = 0.0, y = 0.0;
            if (!item.Get ("text", content) || !item.Get ("x", x) || !item.Get ("y", y)) {
                rec.Add ("succeeded", false);
                rec.Add ("error", EVP_FAIL ("each item needs text, x and y", "EvP.CreateText"));
                results.Push (rec);
                continue;
            }

            API_Element element = {};
            element.header.type = API_TextID;
            if (const GSErrCode err = ACAPI_Element_GetDefaults (&element, nullptr); err != NoError) {
                rec.Add ("succeeded", false);
                rec.Add ("error", EVP_ACAPI_FAIL ("ACAPI_Element_GetDefaults", err, "API_TextID"));
                results.Push (rec);
                continue;
            }

            GS::UniString layerErr;
            if (!ResolveLayerParam (item, element.header, layerErr)) {
                rec.Add ("succeeded", false);
                rec.Add ("error", layerErr);
                results.Push (rec);
                continue;
            }

            GS::Int32 floorInd = 0;
            if (item.Get ("floorInd", floorInd))
                element.header.floorInd = (short) floorInd;

            element.text.loc.x = x;
            element.text.loc.y = y;

            // Monostyle: these struct fields are the ones the single run copies.
            element.text.multiStyle = false;

            // The deterministic baseline — see the trap at the top of this file.
            // Everything below this may then be raised by an explicit input.
            bool inheritDefaults = false;
            item.Get ("inheritDefaults", inheritDefaults);
            if (!inheritDefaults) {
                element.text.angle       = 0.0;
                element.text.faceBits    = APIFace_Plain;
                element.text.effectsBits = 0;
                element.text.just        = APIJust_Left;
                element.text.anchor      = APIAnc_LB;
                element.text.fixedAngle  = false;
                element.text.fixedSize   = false;
                // width 0 + nonBreaking means "grow to fit the content". A stale
                // narrow width inherited from the tool wraps the text to one
                // character per line, which is the same failure a caller-supplied
                // width in the wrong unit produces.
                element.text.width       = 0.0;
                element.text.nonBreaking = true;
            }

            item.Get ("size",            element.text.size);        // mm, character height
            item.Get ("angle",           element.text.angle);       // radians
            item.Get ("spacing",         element.text.spacing);
            item.Get ("widthFactor",     element.text.widthFactor);
            item.Get ("charSpaceFactor", element.text.charSpaceFactor);

            GS::Int32 pen = 0;
            if (item.Get ("pen", pen))
                element.text.pen = (short) pen;
            GS::Int32 font = 0;
            // Font is a raw attribute INDEX, not a name: the AC29 headers expose
            // no font-name -> index lookup (there is no API_FontID attribute
            // type), so this stays an index and defaults to the tool's font
            // rather than inventing a resolution that does not exist.
            if (item.Get ("font", font))
                element.text.font = (short) font;

            GS::UniString just;
            if (item.Get ("just", just))
                element.text.just = ParseJust (just);

            GS::UniString anchorName;
            if (item.Get ("anchor", anchorName)) {
                API_AnchorID anchor;
                if (!ParseAnchor (anchorName, anchor)) {
                    rec.Add ("succeeded", false);
                    rec.Add ("error", EVP_FAIL (GS::UniString::Printf ("unknown anchor: %T (want topLeft…bottomRight)", anchorName.ToPrintf ()), "EvP.CreateText"));
                    results.Push (rec);
                    continue;
                }
                element.text.anchor = anchor;
            }

            // Face style is a BITMASK, so the three flags compose. Absent flags
            // leave the default's bit alone rather than clearing it — otherwise
            // sending `bold` alone would silently turn off a default italic.
            bool flag = false;
            auto setFace = [&element, &item, &flag] (const char* key, unsigned short bit) {
                if (item.Get (key, flag))
                    element.text.faceBits = flag ? (unsigned short) (element.text.faceBits | bit)
                                                 : (unsigned short) (element.text.faceBits & ~bit);
            };
            setFace ("bold",      APIFace_Bold);
            setFace ("italic",    APIFace_Italic);
            setFace ("underline", APIFace_Underline);

            if (item.Get ("strikeOut", flag))
                element.text.effectsBits = flag ? (element.text.effectsBits | APIEffect_StrikeOut)
                                                : (element.text.effectsBits & ~APIEffect_StrikeOut);

            // `width` > 0 turns the text into a fixed-width box that wraps;
            // omitted, it grows to fit its content. MILLIMETRES — see the units
            // note at the top; a metre-scale value here wraps every character
            // onto its own line rather than making a modest box.
            double width = 0.0;
            if (item.Get ("width", width) && width > 0.0) {
                element.text.width       = width;
                element.text.nonBreaking = false;   // false = DO wrap at the box edge
            }
            item.Get ("fixedAngle", element.text.fixedAngle);
            item.Get ("fixedSize",  element.text.fixedSize);

            API_ElementMemo memo = {};
            SetTextContentAndParagraphs (memo, element.text, content);

            // NO undo scope here — see WriteCommand. The caller has one open.
            const GSErrCode err = ACAPI_Element_Create (&element, &memo);
            ACAPI_DisposeElemMemoHdls (&memo);

            if (err != NoError) {
                rec.Add ("succeeded", false);
                rec.Add ("error", EVP_ACAPI_FAIL ("ACAPI_Element_Create", err,
                                                  GS::UniString::Printf ("text \"%T\" at (%.3f, %.3f)", content.ToPrintf (), x, y)));
            } else {
                const GS::UniString guid (APIGuidToString (element.header.guid).ToCStr ());
                GS::ObjectState elementId;
                elementId.Add ("guid", guid);
                rec.Add ("succeeded", true);
                rec.Add ("elementId", elementId);
                ++created;
            }
            results.Push (rec);
        }

        os.Add ("results", results);
        os.Add ("count",   created);
        return os;
    }
};

// ---------------------------------------------------------------------------
// EvP.PlacePicture { path, x, y, floorInd?, layer?, width?, height?, rotAngle?,
//                    anchor?, mirrored?, transparent?, name? }
//   -> { elementId, pixelWidth, pixelHeight, placedWidth?, placedHeight? }
//
// The Figure element (API_PictureID) — the only way to get a RASTER image into
// an Archicad database. There is no JSON-API route to it at all: neither the
// `archicad` package nor Tapir creates one, because the image BYTES have to
// cross as a memo handle (`memo.pictHdl`), which no JSON schema can carry.
//
// So the file stays on disk and this command reads it: the caller passes a
// PATH (typically something it just wrote under %LOCALAPPDATA%\EvP\output via
// evp.paths) and Archicad slurps the bytes. That also keeps a multi-megabyte
// PNG out of the bus envelope.
//
// SIZING — always an explicit destBox, deliberately:
//   width/height given  -> that rectangle in MODEL units (metres), exactly.
//   width/height absent -> computed from the image's own pixel count at `dpi`
//                          (default 96): metres = pixels / dpi * 0.0254.
//
// ⚠️ The API's own "natural size" mode (usePixelSize = true, destBox.xMax/yMax
// ignored) is NOT the default here, because it is not reproducible. Measured on
// a real run, one 64x48 PNG placed that way came out 0.355 x 0.266 m on the
// floor plan and 0.335 x 0.252 m on a layout — the placed size depends on the
// view it lands in, so a script cannot lay anything out around it. Deriving the
// box from pixels and a stated DPI gives the same metres everywhere.
// `usePixelSize: true` still opts into the raw API behaviour for anyone who
// wants it.
// Recipe follows the DevKit's Do_CreatePicture (Examples/Element_Test/Src/
// Element_Basics.cpp) exactly, including the BMAllocateHandle/ReadBin pair and
// the ACAPI_DisposeElemMemoHdls on every path.
// ---------------------------------------------------------------------------
class PlacePictureCommand : public WriteCommand {
public:
    GS::String GetName () const override { return "PlacePicture"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::UniString path;
        double x = 0.0, y = 0.0;
        if (!params.Get ("path", path) || path.IsEmpty () ||
            !params.Get ("x", x) || !params.Get ("y", y)) {
            return NativeCommandResult::Failure (EVP_FAIL ("need path, x and y", "EvP.PlacePicture"));
        }

        const IO::Location location (path);

        API_Element element = {};
        element.header.type = API_PictureID;
        if (const GSErrCode err = ACAPI_Element_GetDefaults (&element, nullptr); err != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Element_GetDefaults", err, "API_PictureID"));
        }

        GS::UniString layerErr;
        if (!ResolveLayerParam (params, element.header, layerErr)) {
            return NativeCommandResult::Failure (layerErr);
        }

        GS::Int32 floorInd = 0;
        if (params.Get ("floorInd", floorInd))
            element.header.floorInd = (short) floorInd;

        // Storage format from the extension. The picture is stored VERBATIM —
        // Archicad does not sniff the bytes — so a wrong format here yields a
        // figure that will not display, which is why an unknown extension is a
        // refusal rather than a guess.
        API_PictureFormat storageFormat = APIPictForm_Default;
        {
            GS::UniString lower = path;
            lower.SetToLowerCase ();
            if      (lower.EndsWith (".png"))                             storageFormat = APIPictForm_PNG;
            else if (lower.EndsWith (".jpg") || lower.EndsWith (".jpeg")) storageFormat = APIPictForm_JPEG;
            else if (lower.EndsWith (".tif") || lower.EndsWith (".tiff")) storageFormat = APIPictForm_TIFF;
            else if (lower.EndsWith (".gif"))                             storageFormat = APIPictForm_GIF;
            else if (lower.EndsWith (".bmp"))                             storageFormat = APIPictForm_Bitmap;
            else {
                return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("unsupported image type: %T (want .png .jpg .tif .gif .bmp)", path.ToPrintf ()),
                                                               "EvP.PlacePicture"));
            }
        }
        element.picture.storageFormat = storageFormat;

        // The image's own pixel dimensions come FIRST: they are required when
        // usePixelSize is true, and they are what the default box is derived
        // from, so nothing about sizing can be decided before this.
        Int32 pixelW = 0, pixelH = 0;
        {
            Int32 hRes = 0, vRes = 0, pixelBitNum = 0;
            const GSErrCode infoErr = GX::ImageBase::GetFileInfo (location, &pixelW, &pixelH,
                                                                  &hRes, &vRes, &pixelBitNum);
            if (infoErr != NoError) {
                return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("GX::ImageBase::GetFileInfo", infoErr,
                                                                      GS::UniString ("reading image dimensions of ") + path));
            }
            element.picture.pixelSizeX = (short) pixelW;
            element.picture.pixelSizeY = (short) pixelH;
            os.Add ("pixelWidth",  (GS::Int32) pixelW);
            os.Add ("pixelHeight", (GS::Int32) pixelH);
        }

        double placedWidth = 0.0, placedHeight = 0.0;
        const bool haveSize = params.Get ("width", placedWidth) && params.Get ("height", placedHeight)
                              && placedWidth > 0.0 && placedHeight > 0.0;

        bool usePixelSize = false;          // see the SIZING note above
        params.Get ("usePixelSize", usePixelSize);

        double dpi = 96.0;
        if (!params.Get ("dpi", dpi) || dpi <= 0.0)
            dpi = 96.0;

        element.picture.destBox.xMin = x;
        element.picture.destBox.yMin = y;
        element.picture.usePixelSize = usePixelSize;
        if (!usePixelSize) {
            if (!haveSize) {
                // Deterministic default: the image at `dpi`, in metres. Same
                // result in a plan, a worksheet and a layout.
                placedWidth  = (double) pixelW / dpi * 0.0254;
                placedHeight = (double) pixelH / dpi * 0.0254;
            }
            element.picture.destBox.xMax = x + placedWidth;
            element.picture.destBox.yMax = y + placedHeight;
            os.Add ("placedWidth",  placedWidth);
            os.Add ("placedHeight", placedHeight);
        }

        params.Get ("rotAngle",    element.picture.rotAngle);
        params.Get ("mirrored",    element.picture.mirrored);
        params.Get ("transparent", element.picture.transparent);

        element.picture.anchorPoint = APIAnc_LB;
        GS::UniString anchorName;
        if (params.Get ("anchor", anchorName)) {
            API_AnchorID anchor;
            if (!ParseAnchor (anchorName, anchor)) {
                return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("unknown anchor: %T (want topLeft…bottomRight)", anchorName.ToPrintf ()), "EvP.PlacePicture"));
            }
            element.picture.anchorPoint = anchor;
        }

        GS::UniString name;
        if (params.Get ("name", name) && !name.IsEmpty ())
            GS::ucscpy (element.picture.pictName, name.ToUStr ());

        // The bytes. A failure to read them must NOT reach ACAPI_Element_Create
        // with a null handle — that creates an empty figure the user then has to
        // hunt down and delete.
        API_ElementMemo memo = {};
        {
            IO::File file (location);
            if (file.Open (IO::File::ReadMode) != NoError) {
                return NativeCommandResult::Failure (EVP_FAIL (GS::UniString ("cannot open image file: ") + path, "EvP.PlacePicture"));
            }
            USize nBytes = 0;
            if (file.GetDataLength (&nBytes) != NoError || nBytes == 0) {
                file.Close ();
                return NativeCommandResult::Failure (EVP_FAIL (GS::UniString ("image file is empty or unreadable: ") + path, "EvP.PlacePicture"));
            }
            memo.pictHdl = BMAllocateHandle ((GSSize) nBytes, ALLOCATE_CLEAR, 0);
            if (memo.pictHdl == nullptr) {
                file.Close ();
                return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("out of memory allocating %d bytes for the image", (int) nBytes), "EvP.PlacePicture"));
            }
            USize readCount = 0;
            const GSErrCode readErr = file.ReadBin (*memo.pictHdl, nBytes, &readCount);
            file.Close ();
            if (readErr != NoError || readCount != nBytes) {
                ACAPI_DisposeElemMemoHdls (&memo);
                return NativeCommandResult::Failure (EVP_FAIL (GS::UniString ("short read on image file: ") + path, "EvP.PlacePicture"));
            }
        }

        // NO undo scope here — see WriteCommand. The caller has one open.
        const GSErrCode err = ACAPI_Element_Create (&element, &memo);
        ACAPI_DisposeElemMemoHdls (&memo);

        if (err != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Element_Create", err,
                                                                  GS::UniString::Printf ("picture %T at (%.3f, %.3f)", path.ToPrintf (), x, y)));
        }

        GS::ObjectState elementId;
        elementId.Add ("guid", GS::UniString (APIGuidToString (element.header.guid).ToCStr ()));
        os.Add ("elementId", elementId);
        return os;
    }
};

const NativeCommandRegistration DraftingCommandRegistrations[] = {
    { "CreateText", &MakeRegisteredNativeCommand<CreateTextCommand>, false,
      R"json({"type":"object","properties":{"texts":{"type":"array","minItems":1,"items":{"type":"object","properties":{"text":{"type":"string"},"x":{"type":"number"},"y":{"type":"number"},"floorInd":{"type":"integer"},"layer":{"type":"string"},"size":{"type":"number"},"angle":{"type":"number"},"pen":{"type":"integer"},"font":{"type":"integer"},"just":{"type":"string","enum":["left","center","right","full"]},"anchor":{"type":"string","enum":["topLeft","topCenter","topRight","middleLeft","middleCenter","middleRight","bottomLeft","bottomCenter","bottomRight"]},"bold":{"type":"boolean"},"italic":{"type":"boolean"},"underline":{"type":"boolean"},"strikeOut":{"type":"boolean"},"width":{"type":"number"},"fixedAngle":{"type":"boolean"},"fixedSize":{"type":"boolean"},"spacing":{"type":"number"},"widthFactor":{"type":"number"},"charSpaceFactor":{"type":"number"},"inheritDefaults":{"type":"boolean"}},"additionalProperties":false,"required":["text","x","y"]}}},"additionalProperties":false,"required":["texts"]})json",
      R"json({"type":"object","properties":{"results":{"type":"array","items":{"type":"object","properties":{"succeeded":{"type":"boolean"},"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"error":{"type":"string"}},"additionalProperties":false,"required":["succeeded"]}},"count":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["results","count"]})json" },
    { "GetTextElements", &MakeRegisteredNativeCommand<GetTextElementsCommand>, false,
      R"json({"type":"object","properties":{"elements":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]}}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"fromSelection":{"type":"boolean"},"texts":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"content":{"type":"string"},"x":{"type":"number"},"y":{"type":"number"},"hasBounds":{"type":"boolean"},"xMin":{"type":"number"},"yMin":{"type":"number"},"xMax":{"type":"number"},"yMax":{"type":"number"},"anchorX":{"type":"number"},"anchorY":{"type":"number"},"anchor":{"type":"string"},"pen":{"type":"integer"},"angle":{"type":"number"},"size":{"type":"number"},"multiStyle":{"type":"boolean"},"nLine":{"type":"integer"},"floorInd":{"type":"integer"},"layer":{"type":"string"}},"additionalProperties":false,"required":["elementId","content","x","y","hasBounds","xMin","yMin","xMax","yMax","anchorX","anchorY","anchor","pen","angle","size","multiStyle","nLine","floorInd","layer"]}},"count":{"type":"integer","minimum":0},"skipped":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["fromSelection","texts","count","skipped"]})json" },
    { "GetArcElements", &MakeRegisteredNativeCommand<GetArcElementsCommand>, false,
      R"json({"type":"object","properties":{"elements":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]}},"scope":{"type":"string","enum":["database","selection"]},"wholeOnly":{"type":"boolean"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"scope":{"type":"string","enum":["database","selection","elements"]},"arcs":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"x":{"type":"number"},"y":{"type":"number"},"radius":{"type":"number"},"isCircle":{"type":"boolean"},"begAngle":{"type":"number"},"endAngle":{"type":"number"},"ratio":{"type":"number"},"angle":{"type":"number"},"pen":{"type":"integer"},"layer":{"type":"string"},"floorInd":{"type":"integer"}},"additionalProperties":false,"required":["elementId","x","y","radius","isCircle","begAngle","endAngle","ratio","angle","pen","layer","floorInd"]}},"count":{"type":"integer","minimum":0},"skipped":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["scope","arcs","count","skipped"]})json" },
    { "PlacePicture", &MakeRegisteredNativeCommand<PlacePictureCommand>, false,
      R"json({"type":"object","properties":{"path":{"type":"string","minLength":1},"x":{"type":"number"},"y":{"type":"number"},"floorInd":{"type":"integer"},"layer":{"type":"string"},"width":{"type":"number"},"height":{"type":"number"},"rotAngle":{"type":"number"},"anchor":{"type":"string","enum":["topLeft","topCenter","topRight","middleLeft","middleCenter","middleRight","bottomLeft","bottomCenter","bottomRight"]},"mirrored":{"type":"boolean"},"transparent":{"type":"boolean"},"name":{"type":"string"},"usePixelSize":{"type":"boolean"},"dpi":{"type":"number","exclusiveMinimum":0}},"additionalProperties":false,"required":["path","x","y"]})json",
      R"json({"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"pixelWidth":{"type":"integer","minimum":1},"pixelHeight":{"type":"integer","minimum":1},"placedWidth":{"type":"number"},"placedHeight":{"type":"number"}},"additionalProperties":false,"required":["elementId","pixelWidth","pixelHeight"]})json" },
};

}   // namespace

NativeCommandRegistrations GetDraftingCommandRegistrations ()
{
    return MakeRegistrationView (DraftingCommandRegistrations);
}

} // namespace geomsrv
