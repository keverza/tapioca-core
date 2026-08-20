#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/CaptureCommands.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/CommandUtils.hpp"

#include "Screenshot/ScreenshotCapture.hpp"
#include "Screenshot/ScreenshotStore.hpp"
#include "Server/ServerState.hpp"

#include <filesystem>
#include <memory>

namespace geomsrv {

namespace {

// ---------------------------------------------------------------------------
// Tapioca.CaptureScreenshot { view: "current"|"top" }
// Native 3D PNG with the live shading style. Requires the 3D window to be front.
// The bytes are cached; fetch them from the data plane (/screenshot/current|top).
// ---------------------------------------------------------------------------
class CaptureScreenshotCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "CaptureScreenshot"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString view ("current");
        params.Get ("view", view);
        const bool top = (view == "top");

        std::string png, err;
        const bool ok = top ? CaptureTopDown (png, err) : CaptureCurrentView (png, err);

        if (!ok)
            return NativeCommandResult::Failure (GS::UniString (err.c_str ()));

        GS::ObjectState os;
        const size_t bytes = png.size ();
        ScreenshotStore::Get ().Publish (top ? "top" : "current", std::move (png));

        os.Add ("view",  view);
        os.Add ("bytes", static_cast<GS::Int64> (bytes));
        os.Add ("url",   GS::UniString (top ? "http://127.0.0.1:19191/screenshot/top"
                                            : "http://127.0.0.1:19191/screenshot/current"));
        AddMemory (os);
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.Get3DProjection {} — the live "3D Projection Settings", raw.
//
// Everything a screen-space overlay needs to reconstruct the projection that
// produced the PhotoRender/window capture. Perspective carries NO transform
// matrix (API_PerspPars has none — verified in APIdefs_Elements.h), so the
// camera basis must be rebuilt from pos/target/roll/viewCone downstream; the
// two viewCone conventions (H- vs V-FOV) are exactly what the probe calibrates.
// Parallel (axono) DOES carry the 3x4 tranmat/invtranmat (model<->projected),
// but NOT the projected->pixel viewport transform — that gap is real and the
// probe demonstrates solving it from a fitted bounding box.
//
// Read-only; no undo scope. Does not require the 3D window to be front — the
// settings exist regardless — but the numbers only describe the 3D view.
// ---------------------------------------------------------------------------
class Get3DProjectionCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "Get3DProjection"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        API_3DProjectionInfo proj = {};
        const GSErrCode err = ACAPI_View_Get3DProjectionSets (&proj);
        if (err != NoError)
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_View_Get3DProjectionSets", err,
                                                  "reading the 3D window's projection settings"));

        os.Add ("isPersp", proj.isPersp);

        if (proj.isPersp) {
            const API_PerspPars& p = proj.u.persp;
            os.Add ("posX",            p.pos.x);
            os.Add ("posY",            p.pos.y);
            os.Add ("cameraZ",         p.cameraZ);
            os.Add ("targetX",         p.target.x);
            os.Add ("targetY",         p.target.y);
            os.Add ("targetZ",         p.targetZ);
            os.Add ("azimuth",         p.azimuth);
            os.Add ("rollAngle",       p.rollAngle);
            os.Add ("viewCone",        p.viewCone);
            os.Add ("distance",        p.distance);
            os.Add ("isTwoPointPersp", p.isTwoPointPersp);
        } else {
            const API_AxonoPars& a = proj.u.axono;
            os.Add ("azimuth", a.azimuth);
            os.Add ("projMod", (GS::Int32) a.projMod);

            GS::Array<double> tranmat, invtranmat;
            for (int i = 0; i < 12; ++i) tranmat.Push (a.tranmat.tmx[i]);
            for (int i = 0; i < 12; ++i) invtranmat.Push (a.invtranmat.tmx[i]);
            os.Add ("tranmat", tranmat);        // model -> projected: x'=m[0..2].P+m[3], etc.
            os.Add ("invtranmat", invtranmat);  // projected -> model
        }
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.Get3DWindowSets {} -> the 3D window's pixel size + projection zoom (pan/scale).
//
// This is the piece Get3DProjection does NOT carry: how "projected" coordinates
// (model * API_AxonoPars.tranmat) map into rendered PIXELS. API_3DWindowInfo exposes
// it directly — hSize/vSize (window pixels) and zoomScaleX/Y + zoomDispX/Y (the
// projection's scale + offset). For AXONO this makes the overlay EXACT and matches
// the user's actual pan/zoom, so no bbox zoom-to-fit guess is needed. (Perspective
// framing is already fully determined by the persp params, so this is axono's fix.)
//
// The exact projected->pixel convention (origin corner, y direction, whether the
// scale is px-per-projected-unit) is calibrated offline against a real dump — the
// same empirical loop the perspective FOV used. Read-only; no undo scope.
// ---------------------------------------------------------------------------
class Get3DWindowSetsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "Get3DWindowSets"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        API_3DWindowInfo info = {};
        const GSErrCode err = ACAPI_View_Get3DWindowSets (&info);
        if (err != NoError)
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_View_Get3DWindowSets", err,
                                                  "reading the 3D window settings"));

        os.Add ("hSize",      (GS::Int32) info.hSize);   // window width in pixels
        os.Add ("vSize",      (GS::Int32) info.vSize);   // window height in pixels
        os.Add ("zoomScaleX", info.zoomScaleX);          // projection scale (x)
        os.Add ("zoomScaleY", info.zoomScaleY);          // projection scale (y)
        os.Add ("zoomDispX",  info.zoomDispX);           // projection offset (x)
        os.Add ("zoomDispY",  info.zoomDispY);           // projection offset (y)
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.Set3DProjection { azimuthDelta?, azimuth?, distance?, cameraZ?, targetZ?,
//                           viewCone?, regenerate? } -> { changed, before{}, after{} }
//
// Write the 3D window's projection. The counterpart to Get3DProjection, and the
// verb that makes the model-overlay matrix (plan §24) measurable at all: block A
// needs a CAMERA NUDGE as its harshest provocation, and block B has to POSE the
// camera at a dozen known attitudes rather than ask a human to fly there.
//
// ⚠️ SPARSE, AND IT READS BEFORE IT WRITES. Only the fields present in the call
// are changed; everything else keeps the user's value. Anything else would make
// a probe that wanted to nudge the azimuth silently reset the user's whole view.
//
// ⚠️ THIS IS THE USER'S OWN 3D VIEW AND IT IS NOT UNDOABLE. `before` is returned
// in full so a caller can put it back, and any probe that uses this MUST restore
// in a `finally` — a run that dies mid-sweep otherwise leaves the view rotated
// and the user has no undo step to reach for (it is a view setting, not a
// database edit).
//
// ⚠️ `azimuthDelta` IS DEGREES, and it is the reason this verb is not just
// "set". A nudge of +0.5° and back is the cheapest provocation that exercises
// the real navigation present path, and expressing it as a delta means the
// caller does not have to read, add and write — three round trips in which the
// user could have moved the camera themselves.
//
// Perspective and axono share `azimuth`; the rest of the fields are perspective
// only, and asking for them on an axono view is refused by name rather than
// silently ignored.
// ---------------------------------------------------------------------------
class Set3DProjectionCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "Set3DProjection"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        API_3DProjectionInfo proj = {};
        const GSErrCode readErr = ACAPI_View_Get3DProjectionSets (&proj);
        if (readErr != NoError)
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_View_Get3DProjectionSets", readErr,
                                                  "reading the projection before changing it - nothing "
                                                  "was written, so the view is untouched"));

        // What it was, in full, so the caller can restore it exactly.
        GS::ObjectState before;
        before.Add ("isPersp", proj.isPersp);
        if (proj.isPersp) {
            before.Add ("azimuth",  proj.u.persp.azimuth);
            before.Add ("distance", proj.u.persp.distance);
            before.Add ("cameraZ",  proj.u.persp.cameraZ);
            before.Add ("targetZ",  proj.u.persp.targetZ);
            before.Add ("viewCone", proj.u.persp.viewCone);
        } else {
            before.Add ("azimuth", proj.u.axono.azimuth);
            before.Add ("projMod", (GS::Int32) proj.u.axono.projMod);
        }

        double  azimuthDelta = 0.0;
        double  value        = 0.0;
        bool    touched      = false;
        GS::Array<GS::UniString> refused;

        const auto apply = [&params, &value] (const char* key) {
            return params.Get (key, value);
        };

        if (params.Get ("azimuthDelta", azimuthDelta) && azimuthDelta != 0.0) {
            // ⚠️ DEGREES, LIKE `azimuth` ITSELF. API_PerspPars::azimuth is in
            // degrees — the DevKit does not say so, and this repo has already
            // paid once for assuming radians (plan §8.7's landmine list).
            if (proj.isPersp) proj.u.persp.azimuth += azimuthDelta;
            else              proj.u.axono.azimuth += azimuthDelta;
            touched = true;
        }
        if (apply ("azimuth")) {
            if (proj.isPersp) proj.u.persp.azimuth = value;
            else              proj.u.axono.azimuth = value;
            touched = true;
        }
        if (apply ("distance")) {
            if (proj.isPersp) { proj.u.persp.distance = value; touched = true; }
            else refused.Push ("distance (this is an axono view; it has no camera distance)");
        }
        if (apply ("cameraZ")) {
            if (proj.isPersp) { proj.u.persp.cameraZ = value; touched = true; }
            else refused.Push ("cameraZ (axono)");
        }
        if (apply ("targetZ")) {
            if (proj.isPersp) { proj.u.persp.targetZ = value; touched = true; }
            else refused.Push ("targetZ (axono)");
        }
        if (apply ("viewCone")) {
            if (proj.isPersp) { proj.u.persp.viewCone = value; touched = true; }
            else refused.Push ("viewCone (axono has no field of view)");
        }

        if (!touched) {
            os.Add ("changed", false);
            os.Add ("before", before);
            os.Add ("after", before);
            if (!refused.IsEmpty ())
                os.Add ("refused", refused);
            return os;
        }

        const GSErrCode err = ACAPI_View_Change3DProjectionSets (&proj);
        if (err != NoError)
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_View_Change3DProjectionSets", err,
                                                  "writing the 3D window's projection settings"));

        bool regenerate = true;
        params.Get ("regenerate", regenerate);
        if (regenerate)
            ACAPI_View_Redraw ();

        // Read back rather than echo: the only trustworthy report of what
        // Archicad accepted is Archicad's, and it does clamp (a viewCone outside
        // its range comes back different from what was asked for).
        API_3DProjectionInfo now = {};
        GS::ObjectState after;
        if (ACAPI_View_Get3DProjectionSets (&now) == NoError) {
            after.Add ("isPersp", now.isPersp);
            if (now.isPersp) {
                after.Add ("azimuth",  now.u.persp.azimuth);
                after.Add ("distance", now.u.persp.distance);
                after.Add ("cameraZ",  now.u.persp.cameraZ);
                after.Add ("targetZ",  now.u.persp.targetZ);
                after.Add ("viewCone", now.u.persp.viewCone);
            } else {
                after.Add ("azimuth", now.u.axono.azimuth);
                after.Add ("projMod", (GS::Int32) now.u.axono.projMod);
            }
        }

        os.Add ("changed", true);
        os.Add ("before", before);
        os.Add ("after", after);
        if (!refused.IsEmpty ())
            os.Add ("refused", refused);
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.ModelToScreen { points: [[x,y,z], ...] }
//   -> { screen: [{succeeded,x,y}|refused, ...], coord: [...] }
//
// Archicad's OWN model->screen projection, for the current window.
//
// ⚠️ TWO CALLS, NOT ONE, AND THE FIRST ONE'S NAME LIES.
// `ACAPI_View_ModelToScreen` does NOT return pixels. It returns an API_Coord —
// a DRAWING-SPACE coordinate in metres. Pixels need the second hop through
// `ACAPI_View_CoordToPoint`, exactly as the DevKit's own example does it
// (Examples/Notification_Manager/Src/NotificationBubble_Test.cpp: ModelToScreen
// then CoordToPoint, then Show(point)).
//
// This cost a full block of ModelOverlayMatrix. Returning the intermediate as
// `screen` made every probe point land within ±5 of the origin — the target
// projected to exactly 0,0 and a 5 m offset moved it 4.5 — so block B's four
// camera hypotheses were all scored against (0,0) and came back 726-754 px
// apart, which is just sqrt(640² + 360²): the distance from a 1280x720 centre
// to the origin. Four hypotheses, three runs, no measurement. BOTH values are
// returned now so a caller can never again mistake one for the other.
//
// ⚠️ THIS IS THE GROUND TRUTH THE OVERLAY WORK NEEDS, and whether it works is
// itself the first question (plan §24 block B0). If it returns sane values for
// the 3D window, the whole camera-convention sweep is machine-scored: pose the
// camera, project known points, compare against each candidate matrix
// construction. If it does not, every hypothesis needs a human to look at a
// coloured cross, and the sweep costs a round trip per pose instead of one for
// all of them.
//
// It answers for whatever window is CURRENT, which is why the window type is
// reported back: the same call against a floor plan returns plan coordinates,
// and a caller that did not check would compare 3D hypotheses against 2D truth
// and conclude that all of them are wrong.
//
// ⚠️ API_Point is TWO SHORTS (APIdefs_Base.h), "pixels from the top/left corner
// of the window". A point that projects far off-screen — which a pose sweep
// WILL produce, and a point behind the camera certainly does — can therefore
// wrap rather than refuse. A caller scoring registration must sanity-check the
// pixel against the window rect instead of trusting succeeded:true, and cross-read
// `coord` when a value looks wrong: the metres do not overflow.
//
// Read-only, no undo scope. Verified symbols: ACAPI_View_ModelToScreen,
// ACAPI_View_CoordToPoint (both ACAPI_Database.h).
// ---------------------------------------------------------------------------
class ModelToScreenCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "ModelToScreen"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Array<GS::Array<double>> points;
        if (!params.Get ("points", points) || points.IsEmpty ())
            return NativeCommandResult::Failure (EVP_FAIL ("no points given",
                                                "projecting model coordinates to screen; pass "
                                                "points: [[x,y,z], ...] in metres"));

        // Which window answered. See the block above: without this a caller
        // cannot tell 3D truth from plan truth, and they look identical.
        API_WindowInfo wi = {};
        if (ACAPI_Window_GetCurrentWindow (&wi) == NoError) {
            os.Add ("windowType", (GS::Int32) wi.typeID);
            os.Add ("is3DWindow", wi.typeID == APIWind_3DModelID);
        }

        GS::Array<GS::ObjectState> screen;      // pixels — what a caller wants
        GS::Array<GS::ObjectState> coord;       // drawing metres — the intermediate
        GS::Int32 failed = 0;
        for (UIndex i = 0; i < points.GetSize (); ++i) {
            const GS::Array<double>& p = points[i];
            GS::ObjectState pixEntry;
            GS::ObjectState mmEntry;
            if (p.GetSize () < 3) {
                pixEntry.Add ("succeeded", false);
                pixEntry.Add ("why", GS::UniString ("a point needs three coordinates"));
                mmEntry = pixEntry;
                ++failed;
                screen.Push (pixEntry);
                coord.Push (mmEntry);
                continue;
            }

            API_Coord3D model = { p[0], p[1], p[2] };
            API_Coord    mid  = {};
            const GSErrCode err = ACAPI_View_ModelToScreen (model, mid);
            if (err != NoError) {
                // ⚠️ NOT a bare code. The whole point of B0 is to find out
                // WHETHER this call works in the 3D window, so its failure is
                // the result, not an inconvenience — it has to be legible.
                pixEntry.Add ("succeeded", false);
                pixEntry.Add ("why", evp::DescribeErr (err));
                mmEntry = pixEntry;
                ++failed;
                screen.Push (pixEntry);
                coord.Push (mmEntry);
                continue;
            }

            // The drawing-space coordinate is reported whether or not the pixel
            // hop succeeds: if CoordToPoint is the half that refuses, a caller
            // has to be able to see that the projection itself worked.
            mmEntry.Add ("succeeded", true);
            mmEntry.Add ("x", mid.x);
            mmEntry.Add ("y", mid.y);

            API_Point pt = {};
            const GSErrCode perr = ACAPI_View_CoordToPoint (&mid, &pt);
            if (perr != NoError) {
                pixEntry.Add ("succeeded", false);
                pixEntry.Add ("why", evp::DescribeErr (perr));
                ++failed;
            } else {
                pixEntry.Add ("succeeded", true);
                pixEntry.Add ("x", (double) pt.h);
                pixEntry.Add ("y", (double) pt.v);
            }
            screen.Push (pixEntry);
            coord.Push (mmEntry);
        }

        os.Add ("count", (GS::Int32) screen.GetSize ());
        os.Add ("failed", failed);
        os.Add ("screen", screen);
        os.Add ("coord", coord);
        return os;
    }
};

const NativeCommandRegistration kCaptureCommandRegistrations[] = {
    { "CaptureScreenshot", &MakeRegisteredNativeCommand<CaptureScreenshotCommand>, false,
      R"json({"type":"object","properties":{"view":{"type":"string","enum":["current","top"]}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"view":{"type":"string","enum":["current","top"]},"bytes":{"type":"integer","minimum":0},"url":{"type":"string","minLength":1},"retainedBytes":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["view","bytes","url","retainedBytes"]})json" },
    { "Get3DProjection", &MakeRegisteredNativeCommand<Get3DProjectionCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"isPersp":{"type":"boolean"},"posX":{"type":"number"},"posY":{"type":"number"},"cameraZ":{"type":"number"},"targetX":{"type":"number"},"targetY":{"type":"number"},"targetZ":{"type":"number"},"azimuth":{"type":"number"},"rollAngle":{"type":"number"},"viewCone":{"type":"number"},"distance":{"type":"number"},"isTwoPointPersp":{"type":"boolean"},"projMod":{"type":"integer"},"tranmat":{"type":"array","description":"Model-to-projected 3x4 matrix in row-major order.","items":{"type":"number"},"minItems":12,"maxItems":12},"invtranmat":{"type":"array","description":"Projected-to-model 3x4 matrix in row-major order.","items":{"type":"number"},"minItems":12,"maxItems":12}},"additionalProperties":false,"required":["isPersp","azimuth"],"oneOf":[{"properties":{"isPersp":{"const":true}},"required":["posX","posY","cameraZ","targetX","targetY","targetZ","rollAngle","viewCone","distance","isTwoPointPersp"]},{"properties":{"isPersp":{"const":false}},"required":["projMod","tranmat","invtranmat"]}]})json" },
    { "Get3DWindowSets", &MakeRegisteredNativeCommand<Get3DWindowSetsCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"hSize":{"type":"integer"},"vSize":{"type":"integer"},"zoomScaleX":{"type":"number"},"zoomScaleY":{"type":"number"},"zoomDispX":{"type":"number"},"zoomDispY":{"type":"number"}},"additionalProperties":false,"required":["hSize","vSize","zoomScaleX","zoomScaleY","zoomDispX","zoomDispY"]})json" },
    { "Set3DProjection", &MakeRegisteredNativeCommand<Set3DProjectionCommand>, false,
      R"json({"type":"object","properties":{"azimuthDelta":{"type":"number"},"azimuth":{"type":"number"},"distance":{"type":"number"},"cameraZ":{"type":"number"},"targetZ":{"type":"number"},"viewCone":{"type":"number"},"regenerate":{"type":"boolean"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"changed":{"type":"boolean"},"before":{"type":"object","properties":{"isPersp":{"type":"boolean"},"azimuth":{"type":"number"},"distance":{"type":"number"},"cameraZ":{"type":"number"},"targetZ":{"type":"number"},"viewCone":{"type":"number"},"projMod":{"type":"integer"}},"additionalProperties":false},"after":{"type":"object","properties":{"isPersp":{"type":"boolean"},"azimuth":{"type":"number"},"distance":{"type":"number"},"cameraZ":{"type":"number"},"targetZ":{"type":"number"},"viewCone":{"type":"number"},"projMod":{"type":"integer"}},"additionalProperties":false},"refused":{"type":"array","items":{"type":"string"}}},"additionalProperties":false,"required":["changed","before","after"]})json" },
    { "ModelToScreen", &MakeRegisteredNativeCommand<ModelToScreenCommand>, false,
      R"json({"type":"object","properties":{"points":{"type":"array","minItems":1,"items":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}}},"additionalProperties":false,"required":["points"]})json",
      R"json({"type":"object","properties":{"windowType":{"type":"integer"},"is3DWindow":{"type":"boolean"},"count":{"type":"integer","minimum":0},"failed":{"type":"integer","minimum":0},"screen":{"type":"array","description":"Pixel results positionally aligned with input points.","items":{"type":"object","properties":{"succeeded":{"type":"boolean"},"x":{"type":"number"},"y":{"type":"number"},"why":{"type":"string"}},"additionalProperties":false,"required":["succeeded"]}},"coord":{"type":"array","description":"Drawing-space results positionally aligned with input points.","items":{"type":"object","properties":{"succeeded":{"type":"boolean"},"x":{"type":"number"},"y":{"type":"number"},"why":{"type":"string"}},"additionalProperties":false,"required":["succeeded"]}}},"additionalProperties":false,"required":["count","failed","screen","coord"]})json" }
};

}   // namespace

NativeCommandRegistrations GetCaptureCommandRegistrations ()
{
    return MakeRegistrationView (kCaptureCommandRegistrations);
}

GSErrCode InstallCaptureJsonCommands ()
{
    return ACAPI_AddOnAddOnCommunication_InstallAddOnCommandHandler (
        GS::NewOwned<RegisteredNativeCommand<CaptureScreenshotCommand>> (kCaptureCommandRegistrations[0]));
}

} // namespace geomsrv
