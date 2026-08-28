// PLAT-RE151 — when the overlay lays Archicad's building into the depth buffer
// so preview content can go BEHIND it.
//
// ⚠️ WHAT THESE TESTS ARE FOR. The prepass itself needs a GPU and cannot be
// asserted here, but its GATE does not: every case below is a picture someone
// can be shown and be wrong about, and the two that cost real time to diagnose
// in Archicad (a preview vanishing behind storeys in the PLAN overlay, and a
// second full pass over the model in a mode that already wrote the same depth)
// are both pure decisions. They are settled here instead.

#include "ArchViz/OcclusionPrepass.hpp"

#include <gtest/gtest.h>

using geomsrv::archviz::OcclusionPrepassInputs;
using geomsrv::archviz::OcclusionPrepassWanted;
using geomsrv::archviz::SceneRenderMode;

namespace {

// The overlay's own configuration: wireframe over Archicad's 3D window, with a
// model extracted and drawn. This is the case the task exists for.
OcclusionPrepassInputs OverlayDefaults ()
{
    OcclusionPrepassInputs inputs;
    inputs.enabled = true;
    inputs.modelIsDrawn = true;
    inputs.renderMode = SceneRenderMode::Wireframe;
    inputs.elementCount = 12;
    return inputs;
}

} // namespace

TEST (OcclusionPrepass, RunsInTheOverlaysOwnWireframeMode)
{
    EXPECT_TRUE (OcclusionPrepassWanted (OverlayDefaults ()));
}

TEST (OcclusionPrepass, TheSwitchIsHonoured)
{
    OcclusionPrepassInputs inputs = OverlayDefaults ();
    inputs.enabled = false;
    EXPECT_FALSE (OcclusionPrepassWanted (inputs));
}

// ⚠️ THE COST-SAVING CASE. Shaded and shaded+wireframe already rasterise the
// model's opaque surfaces into the SAME depth buffer the preview tests against,
// so the occlusion is a property those frames have for free. Running the prepass
// there would double the model's vertex work for an identical picture.
TEST (OcclusionPrepass, ShadedModesAlreadyHaveTheDepth)
{
    OcclusionPrepassInputs inputs = OverlayDefaults ();
    inputs.renderMode = SceneRenderMode::Shaded;
    EXPECT_FALSE (OcclusionPrepassWanted (inputs));
    inputs.renderMode = SceneRenderMode::ShadedWireframe;
    EXPECT_FALSE (OcclusionPrepassWanted (inputs));
}

// ⚠️ THE CORRECTNESS CASE, AND THE EXPENSIVE ONE TO FIND LIVE. `modelIsDrawn` is
// false over the PLAN, where the camera is orthographic and aimed at a 2D
// drawing. A 3D occluder there hides the preview behind storeys the plan does
// not show, and the symptom -- "my Grasshopper result disappears in plan view"
// -- points at the preview transport rather than at this rule.
TEST (OcclusionPrepass, NotOverThePlanOrWhileBlanked)
{
    OcclusionPrepassInputs inputs = OverlayDefaults ();
    inputs.modelIsDrawn = false;
    EXPECT_FALSE (OcclusionPrepassWanted (inputs));
}

// Nothing extracted yet is the first seconds of every session. A prepass over no
// geometry leaves depth exactly as cleared, so it is a no-op that still costs a
// pipeline bind on every frame until the first extraction lands.
TEST (OcclusionPrepass, NothingExtractedIsNotAnOccluder)
{
    OcclusionPrepassInputs inputs = OverlayDefaults ();
    inputs.elementCount = 0;
    EXPECT_FALSE (OcclusionPrepassWanted (inputs));
}

// ⚠️ EVERY DISQUALIFIER STANDS ALONE. Written because the four clauses are cheap
// to reorder and a `&&` that became an `||` during one would still pass all the
// single-cause tests above.
TEST (OcclusionPrepass, AnyOneDisqualifierIsEnough)
{
    OcclusionPrepassInputs inputs = OverlayDefaults ();
    inputs.enabled = false;
    inputs.modelIsDrawn = false;
    inputs.elementCount = 0;
    inputs.renderMode = SceneRenderMode::Shaded;
    EXPECT_FALSE (OcclusionPrepassWanted (inputs));

    // ...and restoring all four is what turns it back on, which is the only way
    // to show that none of them was being ignored.
    EXPECT_TRUE (OcclusionPrepassWanted (OverlayDefaults ()));
}

// The struct's own defaults must not draw. A caller that fills some fields and
// forgets the rest gets "no prepass", which is the overlay's behaviour before
// this task rather than a pass over the model with an uninitialised camera.
TEST (OcclusionPrepass, DefaultConstructedInputsDoNothing)
{
    EXPECT_FALSE (OcclusionPrepassWanted (OcclusionPrepassInputs {}));
}
