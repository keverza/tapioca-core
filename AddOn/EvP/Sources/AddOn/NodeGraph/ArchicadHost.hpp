#ifndef EVP_NODEGRAPH_ARCHICADHOST_HPP
#define EVP_NODEGRAPH_ARCHICADHOST_HPP

// The one seam between the graph runtime and Archicad.
//
// Everything above this interface - the nodes, the plan, the evaluator, the
// reports - is DevKit-free and therefore covered by the offline suite. Exactly
// one translation unit implements it against ACAPI, and that one honours
// MainThreadGate's contract. A node never sees ACAPI, never sees a thread, and
// cannot call the SDK by accident.
//
// The interface is deliberately BATCHED. MainThreadGate measured round trips at
// roughly 0.6-8ms; a per-element interface turns a thousand-element selection
// into minutes of marshalling. Every call here takes or returns a whole list.

#include "NodeGraph/ElementClassification.hpp"
#include "NodeGraph/ProjectGenerations.hpp"
#include "NodeGraph/ReferenceResolver.hpp"
#include "NodeGraph/Value.hpp"

#include <string>
#include <vector>

namespace evp::nodegraph {

// Archicad's own 3D window camera, in model coordinates and degrees.
//
// ⚠️ THE SAME ELEVEN NUMBERS Tapioca.GetArchicad3DCamera ALREADY ANSWERS
// WITH, and that is deliberate rather than incidental: the headless capture path
// consumes exactly this shape (ArchViz/DiligentViewport.hpp's CameraStart, and
// StartDiligentCapture's own schema), so a camera captured by a node travels to
// the renderer with NO conversion in between. A second spelling of a camera in
// this repository would be a second place for the aspect-ratio and handedness
// mistakes that header spends thirty lines warning about.
//
// ⚠️ DOUBLES HERE, FLOATS THERE, AND THE NARROWING HAPPENS AT THE
// RENDERER'S DOOR. ACAPI answers in doubles and every other coordinate in the
// graph runtime is a double; CameraStart is float because it is read on the
// render thread. Narrowing once, at the boundary, is one conversion in one place.
//
// ⚠️ `viewCone` IS HORIZONTAL, IN DEGREES. Archicad's is; the viewer's
// camera is VERTICAL and converts with the aspect ratio. Nothing in this file
// converts it - it is carried exactly as Archicad reported it, and the renderer
// does the conversion once it knows the target size.
struct ViewCamera {
    // False when there is no camera to copy - an axonometric 3D window, or a
    // read that failed. `source` then says which, in words a user can act on.
    bool valid = false;

    // "perspective", or why there is no camera. Carried rather than derived so
    // a refusal can name the reason instead of reporting a generic failure.
    std::string source;

    double eye[3] = { 0.0, 0.0, 0.0 };
    double target[3] = { 0.0, 0.0, 0.0 };
    double viewConeDegreesHorizontal = 0.0;

    // ---- the sun that was lighting the model when this camera was taken ----
    //
    // ⚠️ CAPTURED WITH THE CAMERA, ON PURPOSE, so a list of viewpoints
    // taken across a day carries a sun per frame. That is what makes "orbit, set
    // the time, Add, repeat" a sun study rather than a set of views all lit the
    // same way.
    //
    // ⚠️ IT COMES FROM API_PlaceInfo, NOT FROM THE PROJECTION'S OWN
    // sunAngSets, AND THAT IS NOT ARBITRARY. API_PerspPars carries a
    // sunAngSets whose convention nothing in this repository interprets -
    // ScreenshotCapture only copies it across verbatim - while
    // API_PlaceInfo::sunAngXY/sunAngZ is the sun the viewer already lights the
    // model with (ArchViz/ExtractionEnvironment.cpp) and the one convention that
    // was settled against an independent NOAA calculation, twice, at two
    // different project-north values (NativeCommands/ProjectCommands.cpp).
    // Reading the other one would put a SECOND sun vocabulary in this repository,
    // which the comments there record as having already cost a debugging cycle:
    // the two azimuths differ by a convention and the mistake renders as a
    // perfectly plausible shadow pointing the wrong way.
    //
    // False when the project has no readable place information. The camera is
    // still valid; it simply carries no sun.
    bool hasSun = false;

    // ⚠️ THE MODEL ANGLE, DEGREES, COUNTERCLOCKWISE FROM +X - which is
    // what Tapioca.SetDiligentSun takes and what DiligentViewportState reports.
    // It is NOT the compass bearing Archicad's Sun dialog shows; the two differ
    // by project north, and confusing them is the entire history of this corner
    // of the code.
    double sunAzimuthDegrees = 0.0;

    // Above the horizon. Negative means the sun is down.
    double sunAltitudeDegrees = 0.0;

    // Clockwise from geographic north - the number a person reads ("the sun is
    // at 193 degrees"). Carried as well as the model angle rather than instead
    // of it, because one of them is for the renderer and the other is for the
    // row in the node, and deriving either on demand means re-deriving the north
    // term that ProjectCommands.cpp records getting wrong the first time.
    double sunBearingDegrees = 0.0;

    // Which rule produced the three angles above: "date" when they were computed
    // for the project's moment, "angles" when the user typed them into the Sun
    // dialog and they were taken as given.
    //
    // ⚠️ CARRIED SO A WRONG SUN CAN BE DIAGNOSED WITHOUT GUESSING. The two
    // paths fail in opposite directions - a stale cache freezes the sun, a
    // needless recompute throws away a typed one - and they are indistinguishable
    // from the angles alone.
    std::string sunSource;

    // ---- Archicad's own sun settings for this view, VERBATIM ---------------
    //
    // ⚠️ ROUND-TRIPPED, NOT INTERPRETED, AND THAT IS THE WHOLE POINT.
    // Restore has to put the sun back, and the convention of
    // API_SunAngleSettings::sunAzimuth is not documented anywhere this repository
    // could check. Copying the struct out and back in needs no such knowledge:
    // whatever Archicad meant by these numbers, it means the same thing when it
    // reads them again. The DERIVED angles above are the ones with a known
    // convention, and they are what the renderer gets.
    bool sunFromDate = false;
    double sunRawAzimuth = 0.0;
    double sunRawAltitude = 0.0;
    int sunYear = 0;
    int sunMonth = 0;
    int sunDay = 0;
    int sunHour = 0;
    int sunMinute = 0;
    int sunSecond = 0;
    bool sunSummerTime = false;
};

class IArchicadHost {
  public:
    virtual ~IArchicadHost () = default;

    // False when no project is open. Everything below then reports the same.
    virtual bool IsAvailable () const = 0;

    virtual const IProjectGenerationSource& Generations () const = 0;
    virtual const IReferenceResolver& References () const = 0;

    // The current selection, in Archicad's own order. An empty selection is
    // success with an empty list, not a failure - a graph that asks "what is
    // selected" when nothing is has a correct answer.
    virtual bool GetSelection (std::vector<ArchicadElementRef>& elements, std::string& error) const = 0;

    // What each of these elements IS: its type, and the settings
    // ElementClassification says are worth showing for that type.
    //
    // ⚠️ ONE CALL FOR THE WHOLE LIST, like everything else here, and for the
    // same measured reason: a per-element read of a 500-element selection is
    // 500 gate crossings.
    //
    // ⚠️ A MISSING ELEMENT IS A REPORTED ROW, NOT A FAILED CALL. Half a
    // selection deleted since it was captured is an ordinary thing for a user to
    // want to see, and failing the whole read would show them nothing at all.
    // The row comes back with `available` false and a `detail` saying why. The
    // call returns false only when the read could not be attempted, and the
    // order of `descriptions` always matches `elements`.
    virtual bool DescribeElements (const std::vector<ArchicadElementRef>& elements,
                                   std::vector<ElementDescription>& descriptions, std::string& error) const = 0;

    // Replaces the selection with `elements`. All-or-nothing at the API level is
    // not available, so the contract is: every reference is resolved before the
    // first change is made, and a reference that does not resolve fails the call
    // WITHOUT touching the selection.
    virtual bool SetSelection (const std::vector<ArchicadElementRef>& elements, std::string& error) = 0;

    // Archicad's 3D window camera.
    //
    // ⚠️ AN AXONOMETRIC WINDOW IS SUCCESS WITH `valid` FALSE, NOT A
    // FAILURE. There genuinely is no camera position in a parallel projection -
    // it is a matrix, not a point - so "there is nothing to capture, and here is
    // why" is the correct answer to a correct question. The call returns false
    // only when the read could not be attempted at all.
    virtual bool GetViewCamera (ViewCamera& camera, std::string& error) const = 0;

    // Points Archicad's 3D window at `camera`, switching it to a perspective
    // projection if it was not already in one.
    //
    // ⚠️ A HOST UI WRITE, and the only one here besides SetSelection.
    // It moves what the user is looking at, so it belongs to a button press and
    // never to an evaluation - see GraphRuntimeState::ApplyCameraAction, which is
    // the one caller.
    //
    // An invalid camera is refused rather than applied; writing a camera nobody
    // could have captured is how a 3D window ends up somewhere with no way back.
    //
    // ⚠️ `threeDWindowInFront` IS AN OUTPUT, AND IT IS NOT AN ERROR
    // CHANNEL. The projection belongs to the 3D window whether or not that window
    // is the one on screen, so writing it from the floor plan SUCCEEDS and the
    // user sees nothing until they switch. That is a true outcome reported as
    // one, rather than a refusal of something that worked - but it has to be
    // reported, because "I pressed Restore and nothing happened" is otherwise
    // indistinguishable from a broken write.
    virtual bool SetViewCamera (const ViewCamera& camera, bool& threeDWindowInFront, std::string& error) = 0;
};

// The active host, or nullptr when the runtime is running without Archicad -
// the offline suite, a headless test, or the add-on before a project opens.
// Callers must treat nullptr as ordinary, not exceptional.
IArchicadHost* ActiveArchicadHost ();

// Installed once during add-on startup and cleared on teardown. Passing nullptr
// detaches, which is what makes "the project closed mid-run" expressible.
void SetActiveArchicadHost (IArchicadHost* host);

} // namespace evp::nodegraph

#endif
