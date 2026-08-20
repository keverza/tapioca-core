#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/PlanTrackCommands.hpp"
#include "NativeCommands/CommandRegistration.hpp"

#include "PlanOverlay/OverlayWindow.hpp"
#include "Notify/ChangeTracker.hpp"

#include "Diagnostics/ApiError.hpp"

#include <memory>
#include <vector>

namespace geomsrv {

namespace {

namespace po = planoverlay;

GS::ObjectState PointState (const po::Point2& p)
{
    GS::ObjectState os;
    os.Add ("x", p.x);
    os.Add ("y", p.y);
    return os;
}

GS::ObjectState TransformState (const po::Transform& t)
{
    GS::ObjectState os;
    os.Add ("valid", t.valid);
    os.Add ("scaleX", t.scaleX);
    os.Add ("scaleY", t.scaleY);
    os.Add ("offX", t.offX);
    os.Add ("offY", t.offY);
    // The reference pairs are reported, not just the result. A projection that
    // is wrong is otherwise indistinguishable from geometry that is wrong.
    os.Add ("refModelA", PointState (t.refModelA));
    os.Add ("refModelB", PointState (t.refModelB));
    os.Add ("refPointA", PointState (t.refPointA));
    os.Add ("refPointB", PointState (t.refPointB));
    // Settles which window the projection is expressed in, by measurement.
    os.Add ("impliedW", t.impliedW);
    os.Add ("impliedH", t.impliedH);
    os.Add ("canvasW", t.canvasW);
    os.Add ("canvasH", t.canvasH);
    os.Add ("dpiX", t.dpiX);
    os.Add ("dpiY", t.dpiY);
    os.Add ("dpiApplied", t.dpiApplied);
    return os;
}

// ---------------------------------------------------------------------------
// EvP.OverlayTransform {} -> { ok, transform }
//
// One recompute, reported. The probe calls this at two zoom levels to establish
// the projection convention BEFORE anything is built on it.
// ---------------------------------------------------------------------------
class OverlayTransformCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "OverlayTransform"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        if (po::Current () == nullptr)
            return NativeCommandResult::Failure (
                "no overlay window — create one with ProbeOverlayShow first; "
                "the transform is derived from the canvas it is parented to");

        GS::ObjectState os;
        os.Add ("transform", TransformState (po::ComputeTransform ()));
        return os;
    }
};

// ---------------------------------------------------------------------------
// EvP.SetOverlayGeometry {polylines: [[x,y,x,y,...], ...]} -> { ok, count }
//
// Model-space, metres, flat pairs per polyline. Empty restores the test pattern.
// ---------------------------------------------------------------------------
class SetOverlayGeometryCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "SetOverlayGeometry"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Array<GS::Array<double>> raw;
        params.Get ("polylines", raw);

        std::vector<po::Polyline> polys;
        GS::Int32 points = 0;
        for (const auto& flat : raw) {
            po::Polyline poly;
            for (USize i = 0; i + 1 < flat.GetSize (); i += 2)
                poly.push_back ({ flat[i], flat[i + 1] });
            points += static_cast<GS::Int32> (poly.size ());
            polys.push_back (poly);
        }

        po::SetGeometry (polys);

        GS::ObjectState os;
        os.Add ("polylines", static_cast<GS::Int32> (polys.size ()));
        os.Add ("points", points);
        return os;
    }
};

// ---------------------------------------------------------------------------
// EvP.SetOverlayTracking {enable, intervalMs?} -> { ok, tracking, intervalMs }
//
// There is NO pan/zoom notification in the DevKit — measured in Phase 0, the
// window tree does not change and no callback fires — so a poll is the only
// mechanism available (§15.2). The timer lives on the overlay window, so it runs
// on the UI thread, which is what makes calling ACAPI from it legal.
// ---------------------------------------------------------------------------
class SetOverlayTrackingCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "SetOverlayTracking"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        if (po::Current () == nullptr)
            return NativeCommandResult::Failure ("no overlay window to track with — create one first");

        bool enable = true;
        params.Get ("enable", enable);

        GS::Int32 intervalMs = 33;   // ~30 Hz
        params.Get ("intervalMs", intervalMs);

        po::SetTracking (enable, static_cast<UINT> (intervalMs));

        const po::TrackStats s = po::GetTrackStats ();
        GS::ObjectState os;
        os.Add ("tracking", s.tracking);
        os.Add ("intervalMs", static_cast<GS::Int32> (s.intervalMs));
        return os;
    }
};

// ---------------------------------------------------------------------------
// EvP.OverlayTrackStats {} -> { ok, tracking, polls, recomputes, repaints, ... }
//
// polls vs recomputes vs repaints is the §16.3 budget made visible: an idle
// overlay must poll and do nothing else. If repaints tracks polls while the view
// is still, the "render nothing when nothing changed" rule is broken.
//
// modelToken is E25's ChangeTracker: it moves when the model changes, which is
// the signal for re-deriving the geometry rather than the transform.
// ---------------------------------------------------------------------------
class OverlayTrackStatsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "OverlayTrackStats"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        const po::TrackStats s = po::GetTrackStats ();

        GS::ObjectState os;
        os.Add ("exists", po::Current () != nullptr);
        os.Add ("tracking", s.tracking);
        os.Add ("intervalMs", static_cast<GS::Int32> (s.intervalMs));
        os.Add ("polls", static_cast<GS::Int64> (s.polls));
        os.Add ("recomputes", static_cast<GS::Int64> (s.recomputes));
        os.Add ("repaints", static_cast<GS::Int64> (s.repaints));
        os.Add ("acapiFailures", static_cast<GS::Int64> (s.acapiFailures));
        os.Add ("transform", TransformState (s.transform));
        os.Add ("modelToken", static_cast<GS::Int64> (ChangeTracker::Get ().Token ()));
        return os;
    }
};

// ---------------------------------------------------------------------------
// EvP.OverlayCalibrate {} -> { ok, rows[] }
//
// One pass, every candidate answered. Asked for after the pin landed in the
// wrong place twice: testing one hypothesis per palette run and waiting for a
// human to look is how two sessions went on a single question.
//
// Each row is "what the projection would look like if it were expressed in THIS
// window's client space". The window it IS expressed in is the one whose
// horizontal and vertical factors AGREE — a wrong window differs from the right
// one by unequal insets, so its kx and ky come apart. The agreeing row's k is
// then the display-scaling factor, measured rather than assumed.
// ---------------------------------------------------------------------------
class OverlayCalibrateCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "OverlayCalibrate"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        if (po::Current () == nullptr)
            return NativeCommandResult::Failure (
                "no overlay window — create one first; calibration is relative to the canvas it covers");

        GS::Array<GS::ObjectState> arr;
        for (const po::CalibRow& r : po::Calibrate ()) {
            GS::ObjectState row;
            row.Add ("window", GS::UniString (r.label));
            row.Add ("clientW", r.clientW);
            row.Add ("clientH", r.clientH);
            row.Add ("impliedW", r.impliedW);
            row.Add ("impliedH", r.impliedH);
            row.Add ("kx", r.kx);
            row.Add ("ky", r.ky);
            row.Add ("disagree", r.disagree);
            arr.Push (row);
        }

        GS::ObjectState os;
        os.Add ("rows", arr);
        return os;
    }
};

const NativeCommandRegistration kPlanTrackCommandRegistrations[] = {
    { "OverlayTransform", &MakeRegisteredNativeCommand<OverlayTransformCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"transform":{"type":"object","properties":{"valid":{"type":"boolean"},"scaleX":{"type":"number"},"scaleY":{"type":"number"},"offX":{"type":"number"},"offY":{"type":"number"},"refModelA":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"}},"additionalProperties":false,"required":["x","y"]},"refModelB":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"}},"additionalProperties":false,"required":["x","y"]},"refPointA":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"}},"additionalProperties":false,"required":["x","y"]},"refPointB":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"}},"additionalProperties":false,"required":["x","y"]},"impliedW":{"type":"number"},"impliedH":{"type":"number"},"canvasW":{"type":"number"},"canvasH":{"type":"number"},"dpiX":{"type":"number"},"dpiY":{"type":"number"},"dpiApplied":{"type":"boolean"}},"additionalProperties":false,"required":["valid","scaleX","scaleY","offX","offY","refModelA","refModelB","refPointA","refPointB","impliedW","impliedH","canvasW","canvasH","dpiX","dpiY","dpiApplied"]}},"additionalProperties":false,"required":["transform"]})json" },
    { "SetOverlayGeometry", &MakeRegisteredNativeCommand<SetOverlayGeometryCommand>, false,
      R"json({"type":"object","properties":{"polylines":{"type":"array","items":{"type":"array","items":{"type":"number"}}}},"additionalProperties":false,"required":["polylines"]})json",
      R"json({"type":"object","properties":{"polylines":{"type":"integer","minimum":0},"points":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["polylines","points"]})json" },
    { "SetOverlayTracking", &MakeRegisteredNativeCommand<SetOverlayTrackingCommand>, false,
      R"json({"type":"object","properties":{"enable":{"type":"boolean"},"intervalMs":{"type":"integer","minimum":1}},"additionalProperties":false,"required":["enable"]})json",
      R"json({"type":"object","properties":{"tracking":{"type":"boolean"},"intervalMs":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["tracking","intervalMs"]})json" },
    { "OverlayTrackStats", &MakeRegisteredNativeCommand<OverlayTrackStatsCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"exists":{"type":"boolean"},"tracking":{"type":"boolean"},"intervalMs":{"type":"integer","minimum":0},"polls":{"type":"integer","minimum":0},"recomputes":{"type":"integer","minimum":0},"repaints":{"type":"integer","minimum":0},"acapiFailures":{"type":"integer","minimum":0},"transform":{"type":"object","properties":{"valid":{"type":"boolean"},"scaleX":{"type":"number"},"scaleY":{"type":"number"},"offX":{"type":"number"},"offY":{"type":"number"},"refModelA":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"}},"additionalProperties":false,"required":["x","y"]},"refModelB":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"}},"additionalProperties":false,"required":["x","y"]},"refPointA":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"}},"additionalProperties":false,"required":["x","y"]},"refPointB":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"}},"additionalProperties":false,"required":["x","y"]},"impliedW":{"type":"number"},"impliedH":{"type":"number"},"canvasW":{"type":"number"},"canvasH":{"type":"number"},"dpiX":{"type":"number"},"dpiY":{"type":"number"},"dpiApplied":{"type":"boolean"}},"additionalProperties":false,"required":["valid","scaleX","scaleY","offX","offY","refModelA","refModelB","refPointA","refPointB","impliedW","impliedH","canvasW","canvasH","dpiX","dpiY","dpiApplied"]},"modelToken":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["exists","tracking","intervalMs","polls","recomputes","repaints","acapiFailures","transform","modelToken"]})json" },
    { "OverlayCalibrate", &MakeRegisteredNativeCommand<OverlayCalibrateCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"rows":{"type":"array","items":{"type":"object","properties":{"window":{"type":"string"},"clientW":{"type":"number"},"clientH":{"type":"number"},"impliedW":{"type":"number"},"impliedH":{"type":"number"},"kx":{"type":"number"},"ky":{"type":"number"},"disagree":{"type":"number"}},"additionalProperties":false,"required":["window","clientW","clientH","impliedW","impliedH","kx","ky","disagree"]}}},"additionalProperties":false,"required":["rows"]})json" },
};

}   // namespace

NativeCommandRegistrations GetPlanTrackCommandRegistrations ()
{
    return MakeRegistrationView (kPlanTrackCommandRegistrations);
}

}   // namespace geomsrv
