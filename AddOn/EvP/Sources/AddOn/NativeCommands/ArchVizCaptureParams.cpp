#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/ArchVizCaptureParams.hpp"

#include "ObjectState.hpp"

namespace geomsrv {

archviz::CameraStart ReadCaptureCamera (const GS::ObjectState& params)
{
    GS::ObjectState p;
    params.Get ("camera", p);
    archviz::CameraStart camera;
    double value = 0.0;
    p.Get ("eyeX", value);
    camera.eye[0] = float (value);
    p.Get ("eyeY", value);
    camera.eye[1] = float (value);
    p.Get ("eyeZ", value);
    camera.eye[2] = float (value);
    p.Get ("targetX", value);
    camera.target[0] = float (value);
    p.Get ("targetY", value);
    camera.target[1] = float (value);
    p.Get ("targetZ", value);
    camera.target[2] = float (value);
    p.Get ("viewConeDegreesHorizontal", value);
    camera.viewConeDegreesHorizontal = float (value);
    GS::UniString source;
    p.Get ("source", source);
    camera.source = source.ToCStr ().Get ();
    p.Get ("valid", camera.valid);
    p.Get ("orthographic", camera.orthographic);
    p.Get ("viewMoving", camera.viewMoving);
    return camera;
}

// The storey section overlay, as capture parameters.
//
// ⚠️ EXPLICIT PARAMETERS RATHER THAN THE VIEWER'S PERSISTED TOGGLES. A capture is
// SCRIPTED -- a massing feasibility report asks for one and expects the same
// image every run -- so inheriting whatever a human last ticked in the HUD would
// make the output depend on invisible prior UI state. Everything defaults to off,
// so an existing caller gets exactly the image it got before.
archviz::DiligentViewport::CaptureOverlays ReadCaptureOverlays (const GS::ObjectState& params)
{
    archviz::DiligentViewport::CaptureOverlays overlays;
    params.Get ("storySlices", overlays.storySlices);
    params.Get ("storySliceFill", overlays.storySliceFill);

    double width = 0.0;
    if (params.Get ("storySliceWidthPixels", width) && width > 0.0)
        overlays.storySliceWidthPixels = float (width);

    // ⚠️ NAMED, NOT NUMBERED. "behind geometry: dashed" survives being read back
    // out of a saved report; a 1 does not, and a caller that guesses wrong gets a
    // plausible picture rather than an error.
    GS::UniString occluded;
    if (params.Get ("storySliceOccluded", occluded)) {
        if (occluded == "hidden")
            overlays.storySliceOccluded = archviz::SliceOccludedStyle::Hidden;
        else if (occluded == "solid")
            overlays.storySliceOccluded = archviz::SliceOccludedStyle::Solid;
        else
            overlays.storySliceOccluded = archviz::SliceOccludedStyle::Dashed;
    }

    GS::Int32 rgba = 0;
    if (params.Get ("storySliceRgba", rgba))
        overlays.storySliceRgba = uint32_t (rgba);
    if (params.Get ("storySliceFillRgba", rgba))
        overlays.storySliceFillRgba = uint32_t (rgba);
    return overlays;
}

} // namespace geomsrv
