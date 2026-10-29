// ArchViz/ViewpointBake -- see the header.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/SelectionMetadata.hpp"
#include "ArchViz/ViewpointBake.hpp"

#include "Diagnostics/ApiError.hpp"                // DescribeErr - never print a bare GSErrCode
#include "NativeCommands/DraftingFillCommands.hpp" // BuildFillPolygonMemo: the one fill polygon build

#include <mutex>

namespace geomsrv::archviz::viewpointbake {

namespace {

std::mutex s_mutex;
Baked s_baked;

std::string Describe (GSErrCode err)
{
    return evp::DescribeErr (err).ToCStr (0, MaxUSize, CC_UTF8).Get ();
}

// The floor plan made current for the bake, and what was current restored after.
struct FloorPlan {
    API_DatabaseInfo original {};
    bool changed = false;
    GSErrCode error = NoError;
    FloorPlan ()
    {
        error = ACAPI_Database_GetCurrentDatabase (&original);
        if (error == NoError && original.typeID != APIWind_FloorPlanID) {
            API_DatabaseInfo plan {};
            plan.typeID = APIWind_FloorPlanID;
            error = ACAPI_Database_ChangeCurrentDatabase (&plan);
            changed = error == NoError;
        }
    }
    ~FloorPlan ()
    {
        if (changed)
            ACAPI_Database_ChangeCurrentDatabase (&original);
    }
};

// The story visible in 2D, and its name.
bool ActiveStory (short& index, std::string& name, std::string& error)
{
    API_StoryInfo info {};
    const GSErrCode err = ACAPI_ProjectSetting_GetStorySettings (&info);
    if (err != NoError || info.data == nullptr) {
        error = "the stories could not be read (ACAPI_ProjectSetting_GetStorySettings: " + Describe (err) + ")";
        return false;
    }
    index = info.actStory;
    name = "story " + std::to_string (index);
    for (short i = 0; i <= info.lastStory - info.firstStory; ++i)
        if ((*info.data)[i].index == index) {
            const std::string named = GS::UniString ((*info.data)[i].uName).ToCStr (0, MaxUSize, CC_UTF8).Get ();
            if (!named.empty ())
                name = named;
        }
    BMKillHandle (reinterpret_cast<GSHandle*> (&info.data));
    return true;
}

// MAIN THREAD.
void Bake (const evp::sunstudy::ViewpointPlanContour& contour, Baked& baked)
{
    if (contour.points.size () < 3 || contour.points.size () > 4096 ||
        contour.arcAngles.size () != contour.points.size ()) {
        baked.error = "the viewpoint area needs 3..4096 points to bake";
        return;
    }
    FloorPlan plan;
    if (plan.error != NoError) {
        baked.error = "the floor plan could not be made current (" + Describe (plan.error) + ")";
        return;
    }
    short story = 0;
    if (!ActiveStory (story, baked.story, baked.error))
        return;
    API_Element element {};
    element.header.type = API_HatchID; // the Fill tool, with its current settings
    GSErrCode err = ACAPI_Element_GetDefaults (&element, nullptr);
    if (err != NoError) {
        baked.error = "the Fill tool's settings could not be read (" + Describe (err) + ")";
        return;
    }
    element.header.floorInd = story;
    GS::Array<GS::ObjectState> points;
    GS::Array<double> arcAngles;
    for (size_t i = 0; i < contour.points.size (); ++i) {
        GS::ObjectState point;
        point.Add ("x", contour.points[i][0]);
        point.Add ("y", contour.points[i][1]);
        points.Push (point);
        arcAngles.Push (contour.arcAngles[i]);
    }
    GS::ObjectState fill;
    fill.Add ("polygonOutline", points);
    fill.Add ("arcAngles", arcAngles);
    API_ElementMemo memo {};
    GS::UniString polygonError;
    if (!BuildFillPolygonMemo (fill, element.hatch.poly, memo, polygonError)) {
        ACAPI_DisposeElemMemoHdls (&memo);
        baked.error = polygonError.ToCStr (0, MaxUSize, CC_UTF8).Get ();
        return;
    }
    err = ACAPI_CallUndoableCommand ("Tapioca bake viewpoint area",
                                     [&] () -> GSErrCode { return ACAPI_Element_Create (&element, &memo); });
    ACAPI_DisposeElemMemoHdls (&memo);
    if (err != NoError)
        baked.error = "the Fill could not be made (ACAPI_Element_Create: " + Describe (err) + ")";
}

} // namespace

void RequestBake (evp::sunstudy::ViewpointPlanContour contour)
{
    const auto finish = [] (Baked baked) {
        std::lock_guard<std::mutex> lock (s_mutex);
        baked.serial = s_baked.serial + 1;
        s_baked = std::move (baked);
    };
    if (selectionmetadata::Later ([contour = std::move (contour), finish] () {
            Baked baked;
            Bake (contour, baked);
            finish (std::move (baked));
        }))
        return;
    Baked refused;
    refused.error = "the bake was not queued: the project message loop is unavailable";
    finish (std::move (refused));
}

Baked LastBake ()
{
    std::lock_guard<std::mutex> lock (s_mutex);
    return s_baked;
}

} // namespace geomsrv::archviz::viewpointbake
