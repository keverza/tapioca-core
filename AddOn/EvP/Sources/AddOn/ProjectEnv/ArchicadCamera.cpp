#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ProjectEnv/ArchicadCamera.hpp"
#include "ProjectEnv/ProjectSun.hpp"

#include <cmath>

namespace geomsrv {

bool ReadArchicadCamera (ArchicadCamera& out, std::string& error)
{
    out = {};
    API_3DProjectionInfo projection = {};
    const GSErrCode err = ACAPI_View_Get3DProjectionSets (&projection);
    if (err != NoError) {
        error = "Archicad could not report the 3D window's projection (error " + std::to_string ((int) err) + ")";
        return false;
    }
    if (!projection.isPersp) {
        out.source = "the 3D window is axonometric, which has no camera position - switch it to perspective";
        return true;
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

    const ProjectSun sun = ResolveProjectSun ();
    if (sun.valid) {
        constexpr double kRadToDeg = 57.29577951308232;
        out.hasSun = true;
        out.sunAzimuthDegrees = sun.sunAngXY * kRadToDeg;
        out.sunAltitudeDegrees = sun.sunAngZ * kRadToDeg;
        double bearing = (sun.north - sun.sunAngXY) * kRadToDeg;
        out.sunBearingDegrees = bearing - 360.0 * std::floor (bearing / 360.0);
        out.sunSource = sun.source;
    }
    return true;
}

bool WriteArchicadCamera (const ArchicadCamera& camera, bool& threeDWindowInFront, std::string& error)
{
    threeDWindowInFront = false;
    if (!camera.valid) {
        error = "that camera was never captured from a perspective view, so there is nothing to restore";
        return false;
    }

    API_3DProjectionInfo projection = {};
    const GSErrCode read = ACAPI_View_Get3DProjectionSets (&projection);
    if (read != NoError) {
        error = "Archicad could not report the 3D window's projection (error " + std::to_string ((int) read) + ")";
        return false;
    }
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

    const double dx = camera.target[0] - camera.eye[0];
    const double dy = camera.target[1] - camera.eye[1];
    persp.distance = std::sqrt (dx * dx + dy * dy);
    persp.azimuth = std::atan2 (dy, dx) * 180.0 / 3.14159265358979323846;
    if (persp.azimuth < 0.0)
        persp.azimuth += 360.0;
    persp.rollAngle = 0.0;
    projection.camGuid = APINULLGuid;
    projection.actCamSet = APINULLGuid;
    projection.isPersp = true;

    const GSErrCode changed = ACAPI_View_Change3DProjectionSets (&projection);
    if (changed != NoError) {
        error = "Archicad refused the camera (error " + std::to_string ((int) changed) + ")";
        return false;
    }

    API_WindowInfo window = {};
    threeDWindowInFront = ACAPI_Window_GetCurrentWindow (&window) == NoError && window.typeID == APIWind_3DModelID;
    if (threeDWindowInFront) {
        bool regenerate = true;
        ACAPI_View_Rebuild (&regenerate);
        ACAPI_View_Redraw ();
    }

    ArchicadCamera applied;
    std::string readBackError;
    if (!ReadArchicadCamera (applied, readBackError) || !applied.valid) {
        error = readBackError.empty ()
                    ? "the camera was written but the 3D window did not come back as a perspective view"
                    : readBackError;
        return false;
    }
    constexpr double kTolerance = 1.0;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs (applied.eye[axis] - camera.eye[axis]) <= kTolerance &&
            std::abs (applied.target[axis] - camera.target[axis]) <= kTolerance)
            continue;
        error = "Archicad accepted the camera but reported a different one back";
        return false;
    }
    return true;
}

} // namespace geomsrv
