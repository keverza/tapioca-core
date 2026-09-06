#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NodeGraph/ArchicadHostImpl.hpp"

#include "NodeGraph/ElementReader.hpp"

#include "ProjectEnv/ProjectSun.hpp"
#include "Geometry/GeometryExtractor.hpp" // ResolveSelectableOwner
#include "Notify/ChangeTracker.hpp"
#include "Python/MainThreadGate.hpp"
#include "Server/ServerState.hpp"

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// One of TWO translation units in the graph runtime that include ACAPI; the
// other is ElementReaderImpl.cpp, which is the transcription of the element
// settings table and was split out precisely so this file could stay small.
//
// Everything above IArchicadHost is DevKit-free and covered by the offline
// suite; these two are the part that cannot be, so they are kept small and they
// do nothing clever. Two hazards it exists to contain, both learned the hard way
// elsewhere in this repository:
//
//  * ACAPI_Selection_Get ALLOCATES THE MARQUEE HANDLE even when nothing is
//    selected, and it is ours to free. ArchViz/SelectionBridge.cpp carries the
//    same note; a leak here is once per evaluation rather than once per frame,
//    but it is the same leak.
//  * A GPU or sub-part guid is not selectable. Columns, railings, curtain walls
//    and stairs enumerate as their sub-parts, and
//    ACAPI_Selection_SetSelectedElementNeig refuses those.
//    GeometryExtractor::ResolveSelectableOwner walks back up to the owner, and
//    reusing it is why Set Selection works on those four types.
//
// Every ACAPI call below runs inside MainThreadGate, and every gate lambda
// captures BY VALUE - the gate's own header explains why a by-reference capture
// is a use-after-free that only fires when the gate is slow.

namespace evp::nodegraph {
namespace {

constexpr int kGateTimeoutMs = evp::MainThreadGate::DefaultTimeoutMs;

std::string Utf8 (const GS::UniString& text)
{
    return text.ToCStr (0, GS::MaxUSize, CC_UTF8).Get ();
}

bool ProjectIsOpen ()
{
    return geomsrv::ServerState::Get ().modelOpen.load ();
}

// Runs `work` on Archicad's thread. Returns false with `error` set when the gate
// could not deliver it, which is a real outcome and not an exception.
bool OnHostThread (const std::function<void ()>& work, std::string& error)
{
    GS::UniString gateError;
    if (evp::MainThreadGate::Get ().Invoke (work, kGateTimeoutMs, gateError)) {
        return true;
    }
    error = gateError.IsEmpty () ? std::string ("Archicad did not respond") : Utf8 (gateError);
    return false;
}

// MAIN THREAD. The current selection as guid strings, in Archicad's order.
void ReadSelectionOnHostThread (std::vector<std::string>& out)
{
    API_SelectionInfo info = {};
    GS::Array<API_Neig> neigs;
    const GSErrCode err = ACAPI_Selection_Get (&info, &neigs, false);
    // Ours to free whether or not anything was selected, and whether or not the
    // call succeeded.
    if (info.marquee.coords != nullptr)
        BMKillHandle (reinterpret_cast<GSHandle*> (&info.marquee.coords));
    // APIERR_NOSEL among others means "nothing is selected", which is an answer,
    // not a failure. A graph asking what is selected when nothing is has a
    // correct empty result.
    if (err != NoError)
        return;
    for (UInt32 i = 0; i < neigs.GetSize (); ++i)
        out.push_back (Utf8 (APIGuidToString (neigs[i].guid)));
}

// ---------------------------------------------------------------------------
// THE 3D WINDOW'S CAMERA.
//
// ⚠️ THE READ IS ArchVizPanel::ReadArchicadCamera's, RESTATED RATHER THAN
// CALLED. That function answers with an ArchViz `CameraStart`, and the graph
// runtime may not name an ArchViz type: everything above IArchicadHost is
// DevKit-free AND renderer-free, which is what keeps these nodes covered by the
// offline suite. What the two share is the DevKit's field names, which cannot
// drift apart without the DevKit changing under both.

// MAIN THREAD. Archicad's 3D window camera, or why there is not one.
void ReadViewCameraOnHostThread (ViewCamera& out)
{
    API_3DProjectionInfo projection = {};
    const GSErrCode err = ACAPI_View_Get3DProjectionSets (&projection);
    if (err != NoError) {
        out.source = "Archicad could not report the 3D window's projection (error " + std::to_string ((int) err) + ")";
        return;
    }

    if (!projection.isPersp) {
        // NOT A FAILURE, and the wording matters because this is the sentence a
        // user reads when Add refuses. A parallel projection has no eye point at
        // all - it is a matrix - so there is nothing here to capture, and the
        // headless renderer would refuse the result anyway
        // (DiligentViewportControl.cpp's StartCapture).
        out.source = "the 3D window is axonometric, which has no camera position - switch it to perspective";
        return;
    }

    const API_PerspPars& persp = projection.u.persp;
    out.eye[0] = persp.pos.x;
    out.eye[1] = persp.pos.y;
    out.eye[2] = persp.cameraZ;
    out.target[0] = persp.target.x;
    out.target[1] = persp.target.y;
    out.target[2] = persp.targetZ;
    out.viewConeDegreesHorizontal = persp.viewCone;
    out.valid = true;
    out.source = "perspective";

    // ---- the view's own sun settings, VERBATIM -----------------------------
    //
    // ⚠️ COPIED OUT WHOLE SO RESTORE CAN PUT THEM BACK, and copied
    // rather than interpreted on purpose. The convention of
    // API_SunAngleSettings::sunAzimuth is measured rather than documented (see
    // ArchViz/ProjectSun.cpp), and a round trip needs no such knowledge:
    // whatever Archicad meant by these numbers, it means the same thing when it
    // reads them back. The angles the RENDERER gets are resolved separately,
    // just below, in a convention that is known.
    const API_SunAngleSettings& viewSun = persp.sunAngSets;
    out.sunFromDate = viewSun.sunPosOpt == API_SunPosition_GivenByDate;
    out.sunRawAzimuth = viewSun.sunAzimuth;
    out.sunRawAltitude = viewSun.sunAltitude;
    out.sunYear = viewSun.year;
    out.sunMonth = viewSun.month;
    out.sunDay = viewSun.day;
    out.sunHour = viewSun.hour;
    out.sunMinute = viewSun.minute;
    out.sunSecond = viewSun.second;
    out.sunSummerTime = viewSun.summerTime;

    // ---- the sun, from the ONE resolver ------------------------------------
    //
    // ⚠️ NOT READ HERE, AND THE THREE ATTEMPTS THAT PRECEDED THIS ARE THE
    // REASON. A project carries TWO independent suns and the 3D window shades
    // with the one in 3D Projection Settings, not the one in Project Location;
    // reading the place's CACHED angles freezes the sun across every capture,
    // and recomputing the place unconditionally discards a sun the user typed.
    // ArchViz/ProjectSun.cpp already knew all of that - it is PLAT-RE67, settled
    // live on 2026-08-14 - and the viewer has been lighting the model correctly
    // from it the whole time. Reproducing its branching here would have made the
    // captured sun and the rendered sun two implementations of one answer.
    const geomsrv::ProjectSun sun = geomsrv::ResolveProjectSun ();
    if (sun.valid) {
        constexpr double kRadToDeg = 57.29577951308232;
        out.hasSun = true;
        out.sunAzimuthDegrees = sun.sunAngXY * kRadToDeg;
        out.sunAltitudeDegrees = sun.sunAngZ * kRadToDeg;
        // ⚠️ `north - sunAngXY`, NOT `90 - sunAngXY`. The first run of this
        // arithmetic checked out perfectly with the 90 form - because that
        // project had north at exactly 90 degrees, the one value where the north
        // term vanishes. At north = 10 degrees it was off by exactly 80 degrees
        // on all four samples. See ProjectCommands.cpp, where that was paid for.
        double bearing = (sun.north - sun.sunAngXY) * kRadToDeg;
        bearing = bearing - 360.0 * std::floor (bearing / 360.0);
        out.sunBearingDegrees = bearing;
        // The resolver's own words, so a captured sun can be traced to the
        // dialog it came from without a rebuild.
        out.sunSource = sun.source;
    }
}

// MAIN THREAD. Points the 3D window at `camera`. An empty `failure` means it took.
void WriteViewCameraOnHostThread (const ViewCamera& camera, bool& threeDWindowInFront, std::string& failure)
{
    // Read first and write the whole struct back. API_PerspPars carries more
    // than this interface does - the sun angle settings among them - and a
    // zeroed struct would silently reset every one of them.
    API_3DProjectionInfo projection = {};
    const GSErrCode read = ACAPI_View_Get3DProjectionSets (&projection);
    if (read != NoError) {
        failure = "Archicad could not report the 3D window's projection (error " + std::to_string ((int) read) + ")";
        return;
    }

    // ⚠️ AN AXONOMETRIC WINDOW HAS NO PERSPECTIVE MEMBER TO PRESERVE. `u.persp`
    // and `u.axono` are the same bytes; treating axonometric parameters as
    // perspective ones and writing them back would push whatever those bytes
    // happen to say into the sun settings. Coming from axono, a zeroed struct is
    // the only safe starting point.
    if (!projection.isPersp)
        projection.u.persp = {};

    API_PerspPars& persp = projection.u.persp;
    persp.pos.x = camera.eye[0];
    persp.pos.y = camera.eye[1];
    persp.cameraZ = camera.eye[2];
    persp.target.x = camera.target[0];
    persp.target.y = camera.target[1];
    persp.targetZ = camera.target[2];
    persp.viewCone = camera.viewConeDegreesHorizontal;

    // ⚠️ THE SUN GOES BACK TOO, OR RESTORE IS ONLY HALF A RESTORE. A
    // viewpoint captured at four in the afternoon and restored under this
    // morning's sun is not the frame the user saved; they said so. The struct is
    // written back exactly as it was read - see ViewCamera on why round-tripping
    // beats interpreting - and only when the camera actually carries one, so a
    // row from an older graph leaves the current sun alone rather than resetting
    // it to midnight on the zeroth of January.
    if (camera.hasSun) {
        API_SunAngleSettings& sun = persp.sunAngSets;
        sun.sunPosOpt = camera.sunFromDate ? API_SunPosition_GivenByDate : API_SunPosition_GivenByAngles;
        sun.sunAzimuth = camera.sunRawAzimuth;
        sun.sunAltitude = camera.sunRawAltitude;
        sun.year = static_cast<unsigned short> (camera.sunYear);
        sun.month = static_cast<unsigned short> (camera.sunMonth);
        sun.day = static_cast<unsigned short> (camera.sunDay);
        sun.hour = static_cast<unsigned short> (camera.sunHour);
        sun.minute = static_cast<unsigned short> (camera.sunMinute);
        sun.second = static_cast<unsigned short> (camera.sunSecond);
        sun.summerTime = camera.sunSummerTime;
    }

    // ⚠️ azimuth AND distance ARE REDUNDANT WITH pos/target AND ARE WRITTEN
    // ANYWAY. No source states which of the two spellings Archicad reads, and
    // leaving the previous view's angle beside this view's points would be a
    // struct that disagrees with itself. Derived here so both halves say the
    // same thing whichever one wins.
    //
    // ⚠️ DEGREES, CCW FROM +X. Degrees because
    // NativeCommands/CaptureCommands.cpp already records that
    // API_PerspPars::azimuth is in degrees rather than radians - a mistake this
    // repository has paid for once. CCW from +X because that is Archicad's
    // convention for every other angle here (ArchViz/ExtractionEnvironment.cpp
    // on sunAngXY). The DIRECTION is the half no source states, which is why the
    // write is VERIFIED below rather than trusted.
    const double dx = camera.target[0] - camera.eye[0];
    const double dy = camera.target[1] - camera.eye[1];
    persp.distance = std::sqrt (dx * dx + dy * dy);
    double azimuth = std::atan2 (dy, dx) * 180.0 / 3.14159265358979323846;
    if (azimuth < 0.0)
        azimuth += 360.0;
    persp.azimuth = azimuth;
    persp.rollAngle = 0.0;

    // ⚠️ BOTH GUIDS CLEARED, OR EVERY NUMBER ABOVE IS IGNORED. The DevKit is
    // explicit about it: a non-zero camGuid or actCamSet makes the projection
    // follow a FLOORPLAN CAMERA ELEMENT and API_PerspPars is not read at all. A
    // 3D window last driven by a camera object would otherwise refuse every
    // restore while reporting success.
    projection.camGuid = APINULLGuid;
    projection.actCamSet = APINULLGuid;
    projection.isPersp = true;

    const GSErrCode err = ACAPI_View_Change3DProjectionSets (&projection);
    if (err != NoError) {
        failure = "Archicad refused the camera (error " + std::to_string ((int) err) + ")";
        return;
    }

    // ⚠️ REBUILD, NOT JUST REDRAW, AND THIS IS THE WHOLE FIX FOR
    // "RESTORE DOES NOTHING". Change3DProjectionSets STORES the projection; it
    // does not re-render the 3D window from it, and ACAPI_View_Redraw only
    // repaints the frame that is already there. So the settings changed, a
    // read-back of those settings agreed, the command reported success - and the
    // picture never moved. Screenshot/ScreenshotCapture.cpp has always known
    // this: its ForceRegenerate does exactly this pair, with the comment "apply
    // the new projection to the rendered frame", and its top-down capture would
    // photograph the old view without it.
    //
    // ⚠️ AND ONLY WHEN THE 3D WINDOW IS THE ONE IN FRONT. Rebuild acts
    // on the CURRENT window, so rebuilding the floor plan here would cost a
    // regeneration of a drawing nobody asked about - on a large project, a
    // visible stall for no effect at all.
    API_WindowInfo window = {};
    threeDWindowInFront = ACAPI_Window_GetCurrentWindow (&window) == NoError && window.typeID == APIWind_3DModelID;
    if (threeDWindowInFront) {
        bool regenerate = true;
        ACAPI_View_Rebuild (&regenerate);
        ACAPI_View_Redraw ();
    }

    // ⚠️ READ BACK RATHER THAN ASSUME. Archicad CLAMPS - CaptureCommands.cpp
    // records viewCone coming back different from what was asked for - and the
    // azimuth direction above is derived rather than documented. A restore that
    // pointed the window somewhere else must SAY SO: a wrong camera looks
    // exactly like a right one until the user recognises the building.
    ViewCamera applied;
    ReadViewCameraOnHostThread (applied);
    if (!applied.valid) {
        failure = "the camera was written but the 3D window did not come back as a perspective view";
        return;
    }
    // One metre. Loose enough for the float round trip a camera makes through
    // the renderer, tight enough that a wrong angle cannot pass as a right one.
    constexpr double kTolerance = 1.0;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs (applied.eye[axis] - camera.eye[axis]) <= kTolerance &&
            std::abs (applied.target[axis] - camera.target[axis]) <= kTolerance)
            continue;
        failure = "Archicad accepted the camera but reported a different one back";
        return;
    }
}

} // namespace

// --- generations ------------------------------------------------------------

bool ArchicadGenerationSource::Sample (GenerationDomain domain, uint64_t& value, std::string& error) const
{
    if (!ProjectIsOpen ()) {
        error = "no project is open";
        return false;
    }

    switch (domain) {
        case GenerationDomain::Project: {
            // ChangeTracker's token already is a model generation, maintained on
            // Archicad's thread with no ACAPI call to read it. Deliberately
            // CONSERVATIVE: it also moves for element edits and for Tapioca's own
            // writes, so a selection node may re-run when nothing it reads
            // changed. That costs one cheap re-read; the opposite error - a node
            // serving a stale answer after the model moved - is the one that
            // makes a BIM graph untrustworthy.
            value = geomsrv::ChangeTracker::Get ().Token ();
            return true;
        }

        case GenerationDomain::Selection: {
            // A hash of the current selection rather than a counter. Nothing in
            // Archicad notifies on selection change, and polling for one would
            // need a timer and a lifecycle; hashing the list is one batched read
            // that is exactly right by construction - equal selections hash
            // equal, so an unchanged selection is a cache hit.
            auto guids = std::make_shared<std::vector<std::string>> ();
            if (!OnHostThread ([guids] { ReadSelectionOnHostThread (*guids); }, error))
                return false;

            uint64_t hash = 1469598103934665603ULL; // FNV-1a, order-sensitive on purpose
            for (const std::string& guid : *guids) {
                for (const char character : guid) {
                    hash ^= static_cast<unsigned char> (character);
                    hash *= 1099511628211ULL;
                }
                hash ^= 0xFFULL;
                hash *= 1099511628211ULL;
            }
            value = hash;
            return true;
        }
    }

    error = "unknown generation domain";
    return false;
}

// --- references -------------------------------------------------------------

ReferenceResolution ArchicadReferenceResolver::Resolve (const Reference& reference) const
{
    return ResolveAll ({ reference }).front ();
}

std::vector<ReferenceResolution> ArchicadReferenceResolver::ResolveAll (const std::vector<Reference>& references) const
{
    std::vector<ReferenceResolution> resolutions (references.size ());

    if (references.empty ())
        return resolutions;

    if (!ProjectIsOpen ()) {
        for (ReferenceResolution& resolution : resolutions) {
            resolution.status = ResolutionStatus::Missing;
            resolution.detail = "no project is open";
        }
        return resolutions;
    }

    // ONE crossing for the whole batch. Per-reference would be one ~3ms round
    // trip per element.
    auto ids = std::make_shared<std::vector<std::string>> ();
    auto kinds = std::make_shared<std::vector<ReferenceKind>> ();
    for (const Reference& reference : references) {
        ids->push_back (reference.id);
        kinds->push_back (reference.kind);
    }
    auto results = std::make_shared<std::vector<ReferenceResolution>> (references.size ());

    std::string gateError;
    const bool delivered = OnHostThread (
        [ids, kinds, results] {
            for (size_t i = 0; i < ids->size (); ++i) {
                ReferenceResolution& resolution = (*results)[i];
                if ((*kinds)[i] != ReferenceKind::Element) {
                    resolution.status = ResolutionStatus::Incompatible;
                    resolution.detail = "only element references can be resolved by this build";
                    continue;
                }
                const API_Guid guid = APIGuidFromString ((*ids)[i].c_str ());
                if (guid == APINULLGuid) {
                    resolution.status = ResolutionStatus::Missing;
                    resolution.detail = "'" + (*ids)[i] + "' is not a valid element identifier";
                    continue;
                }
                API_Elem_Head head = {};
                head.guid = guid;
                if (ACAPI_Element_GetHeader (&head) != NoError) {
                    // Deleted, never existed, or outside this user's Teamwork
                    // workspace. All three are Missing from the graph's point of
                    // view, and the message says so without guessing which.
                    resolution.status = ResolutionStatus::Missing;
                    resolution.detail = "element " + (*ids)[i] +
                                        " is not in this project - it may have been deleted, or it may belong to "
                                        "another user's Teamwork workspace";
                    continue;
                }
                resolution.status = ResolutionStatus::Resolved;
            }
        },
        gateError);

    if (!delivered) {
        for (ReferenceResolution& resolution : resolutions) {
            resolution.status = ResolutionStatus::Missing;
            resolution.detail = gateError;
        }
        return resolutions;
    }
    return *results;
}

// --- host -------------------------------------------------------------------

bool ArchicadHostImpl::IsAvailable () const
{
    return ProjectIsOpen ();
}

const IProjectGenerationSource& ArchicadHostImpl::Generations () const
{
    return generations_;
}

const IReferenceResolver& ArchicadHostImpl::References () const
{
    return references_;
}

bool ArchicadHostImpl::GetSelection (std::vector<ArchicadElementRef>& elements, std::string& error) const
{
    if (!ProjectIsOpen ()) {
        error = "no project is open";
        return false;
    }

    auto guids = std::make_shared<std::vector<std::string>> ();
    if (!OnHostThread ([guids] { ReadSelectionOnHostThread (*guids); }, error))
        return false;

    for (const std::string& guid : *guids)
        elements.push_back (ArchicadElementRef { guid });
    return true;
}

bool ArchicadHostImpl::DescribeElements (const std::vector<ArchicadElementRef>& elements,
                                         std::vector<ElementDescription>& descriptions, std::string& error) const
{
    if (!ProjectIsOpen ()) {
        error = "no project is open";
        return false;
    }
    return ReadElementDescriptions (elements, descriptions, error);
}

bool ArchicadHostImpl::SetSelection (const std::vector<ArchicadElementRef>& elements, std::string& error)
{
    if (!ProjectIsOpen ()) {
        error = "no project is open";
        return false;
    }

    auto ids = std::make_shared<std::vector<std::string>> ();
    for (const ArchicadElementRef& element : elements)
        ids->push_back (element.guid);

    auto failure = std::make_shared<std::string> ();

    const bool delivered = OnHostThread (
        [ids, failure] {
            // Resolve every neig BEFORE deselecting. Deselecting first and then
            // discovering an unselectable element would leave the user with an
            // empty selection and an error - worse than the selection they had.
            GS::Array<API_Neig> neigs;
            for (const std::string& id : *ids) {
                const API_Guid picked = APIGuidFromString (id.c_str ());
                // Sub-parts of columns, railings, curtain walls and stairs are
                // not selectable; the owner is. See the file header.
                const API_Guid owner = geomsrv::ResolveSelectableOwner (picked);
                if (owner == APINULLGuid) {
                    *failure = "element " + id + " cannot be selected";
                    return;
                }
                API_Neig neig = {};
                if (ACAPI_Selection_SetSelectedElementNeig (&owner, &neig) != NoError) {
                    *failure = "element " + id + " cannot be selected";
                    return;
                }
                neigs.Push (neig);
            }

            ACAPI_Selection_DeselectAll ();
            if (neigs.IsEmpty ())
                return; // An empty result deselects, which is a legitimate answer.
            const GSErrCode err = ACAPI_Selection_Select (neigs, true);
            if (err != NoError)
                *failure = "Archicad refused the selection";
        },
        error);

    if (!delivered)
        return false;
    if (!failure->empty ()) {
        error = *failure;
        return false;
    }
    return true;
}

bool ArchicadHostImpl::GetViewCamera (ViewCamera& camera, std::string& error) const
{
    if (!ProjectIsOpen ()) {
        error = "no project is open";
        return false;
    }

    auto read = std::make_shared<ViewCamera> ();
    if (!OnHostThread ([read] { ReadViewCameraOnHostThread (*read); }, error))
        return false;
    camera = *read;
    return true;
}

bool ArchicadHostImpl::SetViewCamera (const ViewCamera& camera, bool& threeDWindowInFront, std::string& error)
{
    threeDWindowInFront = false;
    if (!ProjectIsOpen ()) {
        error = "no project is open";
        return false;
    }
    // Refused rather than applied. A camera that was never captured carries
    // zeroes, and writing those would put the 3D window at the origin looking at
    // the origin - a view with no way back that the user did not ask for.
    if (!camera.valid) {
        error = "that camera was never captured from a perspective view, so there is nothing to restore";
        return false;
    }

    // BY VALUE into the gate lambda, like every other job in this file: a
    // timed-out job may run after this frame is gone.
    auto wanted = std::make_shared<ViewCamera> (camera);
    auto failure = std::make_shared<std::string> ();
    auto inFront = std::make_shared<bool> (false);
    if (!OnHostThread ([wanted, inFront, failure] { WriteViewCameraOnHostThread (*wanted, *inFront, *failure); },
                       error))
        return false;
    if (!failure->empty ()) {
        error = *failure;
        return false;
    }
    threeDWindowInFront = *inFront;
    return true;
}

ArchicadHostImpl& ArchicadHostImpl::Get ()
{
    static ArchicadHostImpl host;
    return host;
}

} // namespace evp::nodegraph
